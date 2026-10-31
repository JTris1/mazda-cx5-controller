#include <ArduinoJson.h>
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <lwip/sockets.h>
#include <mbedtls/base64.h>
#include <string>
#include "api.h"
#include "api_internal.h"
#include "actions.h"
#include "auth.h"
#include "config.h"
#include "engine.h"
#include "log.h"
#include "net_cell.h"
#include "net_wg.h"
#include "outputs.h"
#include "remote_start.h"
#include "vehicle_state.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing src/secrets.h: copy src/secrets.example.h to src/secrets.h and fill it in."
#endif
#if __has_include("tls_cert.h")
#include "tls_cert.h"
#else
#error "Missing src/tls_cert.h: run tools/gen_cert.sh"
#endif

static constexpr uint16_t API_PORT = 443;
static constexpr uint32_t START_MAX_NONCE_AGE_MS = 15000;  // remote start needs a fresher challenge

static httpd_handle_t server = nullptr;
static bool running = false;
static uint32_t nextStartTry = 0;
static uint32_t tunnelAddr = 0;   // WG_LOCAL_IP in network byte order

// --- response helpers ----------------------------------------------------------------
static const char* statusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 503: return "Service Unavailable";
    default:  return "Error";
  }
}

esp_err_t reply(httpd_req_t* req, int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  char status[40];
  snprintf(status, sizeof(status), "%d %s", code, statusText(code));
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out.c_str(), out.length());
}

esp_err_t replyError(httpd_req_t* req, int code, const char* error) {
  JsonDocument doc;
  doc["error"] = error;
  return reply(req, code, doc);
}

static esp_err_t replySubmit(httpd_req_t* req, Submit result, uint32_t id) {
  switch (result) {
    case Submit::Ok: {
      JsonDocument doc;
      doc["job_id"] = id;
      doc["status"] = "queued";
      return reply(req, 202, doc);
    }
    case Submit::BadArgs:    return replyError(req, 400, "invalid_arguments");
    case Submit::NoExpander: return replyError(req, 503, "expander_missing");
    case Submit::Busy:       return replyError(req, 503, "busy");
    case Submit::Disabled:   return replyError(req, 503, "disabled");
  }
  return ESP_FAIL;
}

static const char* methodName(int method) {
  switch (method) {
    case HTTP_GET:    return "GET";
    case HTTP_POST:   return "POST";
    case HTTP_DELETE: return "DELETE";
    case HTTP_PUT:    return "PUT";
    default:          return "OTHER";
  }
}

// --- connection helpers ----------------------------------------------------------------
// The IDF server listens on a dual-stack socket, so an IPv4 client shows up as an IPv4-mapped IPv6
// address (::ffff:a.b.c.d). Returns that IPv4 address in network byte order; false if it is not IPv4.
static bool v4Of(const sockaddr_storage& a, uint32_t& out) {
  if (a.ss_family == AF_INET) {
    out = ((const sockaddr_in*)&a)->sin_addr.s_addr;
    return true;
  }
  if (a.ss_family == AF_INET6) {
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    const uint8_t* b = (const uint8_t*)&((const sockaddr_in6*)&a)->sin6_addr;
    if (memcmp(b, mapped, sizeof(mapped)) == 0) {
      memcpy(&out, b + 12, 4);
      return true;
    }
  }
  return false;
}

uint32_t clientIp(httpd_req_t* req) {
  sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  uint32_t v4 = 0;
  if (getpeername(httpd_req_to_sockfd(req), (sockaddr*)&addr, &len) != 0 || !v4Of(addr, v4)) return 0;
  return ntohl(v4);
}

// The server listens on every interface (the ESP-IDF server cannot bind to one address), so
// each request checks that it arrived on the tunnel address and closes the connection otherwise.
bool arrivedOnTunnel(httpd_req_t* req) {
  uint32_t peer = clientIp(req);
  sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  uint32_t local = 0;   // network byte order
  bool haveLocal = getsockname(httpd_req_to_sockfd(req), (sockaddr*)&addr, &len) == 0 && v4Of(addr, local);
  Logf(INFO, "API: %s %s from %u.%u.%u.%u to %u.%u.%u.%u", methodName(req->method), req->uri,
       (peer >> 24) & 255, (peer >> 16) & 255, (peer >> 8) & 255, peer & 255,
       local & 255, (local >> 8) & 255, (local >> 16) & 255, (local >> 24) & 255);
  if (haveLocal && local == tunnelAddr) return true;
  Logf(WARN, "API: refused: request arrived on %u.%u.%u.%u, only %s is served", local & 255, (local >> 8) & 255,
       (local >> 16) & 255, (local >> 24) & 255, WG_LOCAL_IP);
  httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
  return false;
}

static bool header(httpd_req_t* req, const char* name, char* out, size_t outLen) {
  size_t n = httpd_req_get_hdr_value_len(req, name);
  return n > 0 && n < outLen && httpd_req_get_hdr_value_str(req, name, out, outLen) == ESP_OK;
}

// --- authentication ------------------------------------------------------------------
bool readBody(httpd_req_t* req, Call& call) {
  size_t len = req->content_len;
  if (len > MAX_BODY) {
    replyError(req, 413, "body_too_large");
    return false;
  }
  while (call.bodyLen < len) {
    int got = httpd_req_recv(req, call.body + call.bodyLen, len - call.bodyLen);
    if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (got <= 0) {
      replyError(req, 400, "bad_body");
      return false;
    }
    call.bodyLen += got;
  }
  call.body[call.bodyLen] = '\0';

  if (strchr(req->uri, '?') != nullptr || strlen(req->uri) >= sizeof(call.path)) {   // no endpoint takes a query
    replyError(req, 400, "unexpected_query");
    return false;
  }
  strcpy(call.path, req->uri);
  return true;
}

// Reads the body, checks the signed headers and answers 4xx itself on failure (returns false).
// Auth failures always get the same generic body, whatever went wrong.
bool authenticate(httpd_req_t* req, Role needed, Call& call) {
  if (!readBody(req, call)) return false;
  char key[24], nonce[40], sig[100];
  bool haveHeaders = header(req, "X-CX5-Key", key, sizeof(key)) && header(req, "X-CX5-Nonce", nonce, sizeof(nonce)) &&
                     header(req, "X-CX5-Signature", sig, sizeof(sig));
  if (!haveHeaders) {
    key[0] = nonce[0] = sig[0] = '\0';   // still goes through authVerify so it is counted and logged
  }

  AuthRequest ar{key, nonce, methodName(req->method), call.path, (const uint8_t*)call.body, call.bodyLen, sig, clientIp(req)};
  call.who = authVerify(ar, needed);
  switch (call.who.result) {
    case AuthResult::Ok:          return true;
    case AuthResult::RateLimited: replyError(req, 429, "rate_limited"); return false;
    case AuthResult::Forbidden:   replyError(req, 403, "forbidden"); return false;
    default:                      replyError(req, 401, "unauthorized"); return false;
  }
}

bool parseBody(const Call& call, JsonDocument& doc) {
  return call.bodyLen > 0 && deserializeJson(doc, call.body, call.bodyLen) == DeserializationError::Ok;
}

// --- endpoints: unauthenticated --------------------------------------------------------
static esp_err_t handleChallenge(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  char nonce[33];
  if (!authNewNonce(clientIp(req), nonce)) return replyError(req, 429, "rate_limited");
  JsonDocument doc;
  doc["nonce"] = nonce;
  doc["expires_in"] = 30;
  return reply(req, 200, doc);
}

static esp_err_t handleNotFound(httpd_req_t* req, httpd_err_code_t) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  return replyError(req, 404, "not_found");
}

// --- endpoints: control ---------------------------------------------------------------
// POST /api/v1/heater  {"seat":"driver|passenger","level":"off|low|medium|high"}
static esp_err_t handleHeater(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Control, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(req, 400, "invalid_json");
  const char* seat = doc["seat"] | "";
  const char* level = doc["level"] | "";

  Seat s;
  if (strcmp(seat, "driver") == 0) s = SEAT_DRIVER;
  else if (strcmp(seat, "passenger") == 0) s = SEAT_PASSENGER;
  else return replyError(req, 400, "invalid_arguments");

  int lv;
  if (strcmp(level, "off") == 0) lv = 0;
  else if (strcmp(level, "high") == 0) lv = 1;
  else if (strcmp(level, "medium") == 0) lv = 2;
  else if (strcmp(level, "low") == 0) lv = 3;
  else return replyError(req, 400, "invalid_arguments");

  uint32_t id = 0;
  return replySubmit(req, actionSetHeater(s, lv, &id), id);
}

// POST /api/v1/defrost  {"zone":"front|rear","action":"on|off"}
static esp_err_t handleDefrost(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Control, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(req, 400, "invalid_json");
  const char* zone = doc["zone"] | "";
  const char* action = doc["action"] | "";

  DefrostZone z;
  if (strcmp(zone, "front") == 0) z = DEFROST_FRONT;
  else if (strcmp(zone, "rear") == 0) z = DEFROST_REAR;
  else return replyError(req, 400, "invalid_arguments");
  bool on;
  if (strcmp(action, "on") == 0) on = true;
  else if (strcmp(action, "off") == 0) on = false;
  else return replyError(req, 400, "invalid_arguments");

  uint32_t id = 0;
  return replySubmit(req, actionSetDefrost(z, on, &id), id);
}

// POST /api/v1/temperature  {"value": 60..84}
static esp_err_t handleTemperature(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Control, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc) || !doc["value"].is<int>()) return replyError(req, 400, "invalid_json");
  uint32_t id = 0;
  return replySubmit(req, actionSetTemperature(doc["value"].as<int>(), &id), id);
}

// POST /api/v1/fan  {"level": 1..fanSteps}   absolute (the device homes to the lowest speed, then counts up)
//               or  {"action":"up|down","steps":1..}   relative (steps defaults to 1)
static esp_err_t handleFan(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Control, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(req, 400, "invalid_json");
  uint32_t id = 0;
  if (doc["level"].is<int>()) {
    return replySubmit(req, actionSetFan(doc["level"].as<int>(), &id), id);
  }
  const char* action = doc["action"] | "";
  bool up;
  if (strcmp(action, "up") == 0) up = true;
  else if (strcmp(action, "down") == 0) up = false;
  else return replyError(req, 400, "invalid_arguments");
  int steps = doc["steps"] | 1;
  return replySubmit(req, actionFanStep(up, steps, &id), id);
}

// POST /api/v1/power  {"action":"toggle"}
static esp_err_t handlePower(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Control, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc) || strcmp(doc["action"] | "", "toggle") != 0) return replyError(req, 400, "invalid_arguments");
  uint32_t id = 0;
  return replySubmit(req, actionPower(&id), id);
}

// POST /api/v1/remote-start  {}
static esp_err_t handleRemoteStart(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Control, call)) return ESP_OK;
  if (call.who.nonceAgeMs > START_MAX_NONCE_AGE_MS) return replyError(req, 401, "unauthorized");  // stale challenge
  const char* reason = "";
  if (!remoteStartPolicyAllow(&reason)) {
    Logf(WARN, "API: remote start refused: %s", reason);
    remoteStartNoteRefused(reason);
    JsonDocument doc;
    doc["error"] = "refused";
    doc["reason"] = reason;
    return reply(req, 403, doc);
  }
  Logf(INFO, "API: remote start requested by key '%s'", call.who.keyId);
  uint32_t id = 0;
  return replySubmit(req, actionRemoteStart(&id), id);
}

static const char* heaterName(int level) {
  static const char* names[] = {"off", "high", "medium", "low"};
  return names[level & 3];
}

// GET /api/v1/state
static esp_err_t handleState(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Control, call)) return ESP_OK;
  JsonDocument doc;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["uptime_s"] = millis() / 1000;
  doc["cellular"] = cellConnected();
  doc["tunnel"] = wgStarted();
  doc["expander_ok"] = expanderOk;
  doc["engine_running"] = (bool)engineRunning;
  doc["engine_age_s"] = (millis() - engineChangedAt) / 1000;

  RemoteStartInfo rs;
  remoteStartInfo(rs);
  JsonObject r = doc["remote_start"].to<JsonObject>();
  r["enabled"] = remoteStartEnabled();
  r["state"] = remoteStartStateName(rs.state);
  r["detail"] = rs.detail;
  r["age_s"] = rs.state == RemoteStartState::Idle ? 0 : (millis() - rs.sinceMs) / 1000;

  uint32_t running = 0;
  int queued = 0;
  actionsBusyInfo(running, queued);
  JsonObject busy = doc["busy"].to<JsonObject>();
  if (running) busy["running"] = running; else busy["running"] = nullptr;
  busy["queued"] = queued;

  VehicleSnapshot v = vstateGet();
  JsonObject c = doc["climate"].to<JsonObject>();
  c["tracked"] = true;   // everything below is the last commanded state, not a reading from the car
  if (v.tempF >= 0) c["temperature_f"] = v.tempF; else c["temperature_f"] = nullptr;
  JsonObject h = c["heater"].to<JsonObject>();
  h["driver"] = heaterName(v.heater[SEAT_DRIVER]);
  h["passenger"] = heaterName(v.heater[SEAT_PASSENGER]);
  JsonObject d = c["defrost"].to<JsonObject>();
  d["front"] = v.defrost[DEFROST_FRONT];
  d["rear"] = v.defrost[DEFROST_REAR];
  if (v.fan >= 1) c["fan"] = v.fan; else c["fan"] = nullptr;

  JsonObject lim = doc["limits"].to<JsonObject>();
  lim["temp_min_f"] = cfg.tempMinF;
  lim["temp_max_f"] = cfg.tempMaxF;
  lim["fan_steps"] = cfg.fanSteps;
  return reply(req, 200, doc);
}

// POST /api/v1/assume  {"temperature":72, "heater":{"driver":"off","passenger":"low"},
//                       "defrost":{"front":false,"rear":true}, "fan":3}
// Tells the device what the controls really are, without pressing anything. Every key is optional;
// null for temperature or fan means "unknown".
static int heaterLevel(const char* name) {
  if (strcmp(name, "off") == 0) return 0;
  if (strcmp(name, "high") == 0) return 1;
  if (strcmp(name, "medium") == 0) return 2;
  if (strcmp(name, "low") == 0) return 3;
  return -1;
}

static esp_err_t handleAssume(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Control, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(req, 400, "invalid_json");

  VehicleSnapshot v;
  uint8_t fields = 0;
  if (doc["temperature"].is<JsonVariantConst>()) {
    if (doc["temperature"].isNull()) {
      v.tempF = -1;
    } else if (doc["temperature"].is<int>() && doc["temperature"].as<int>() >= cfg.tempMinF &&
               doc["temperature"].as<int>() <= cfg.tempMaxF) {
      v.tempF = doc["temperature"].as<int>();
    } else {
      return replyError(req, 400, "invalid_arguments");
    }
    fields |= VS_TEMP;
  }
  if (doc["fan"].is<JsonVariantConst>()) {
    if (doc["fan"].isNull()) {
      v.fan = -1;
    } else if (doc["fan"].is<int>() && doc["fan"].as<int>() >= 1 && doc["fan"].as<int>() <= cfg.fanSteps) {
      v.fan = doc["fan"].as<int>();
    } else {
      return replyError(req, 400, "invalid_arguments");
    }
    fields |= VS_FAN;
  }
  if (doc["heater"]["driver"].is<const char*>()) {
    int l = heaterLevel(doc["heater"]["driver"]);
    if (l < 0) return replyError(req, 400, "invalid_arguments");
    v.heater[SEAT_DRIVER] = l;
    fields |= VS_HEATER_DRIVER;
  }
  if (doc["heater"]["passenger"].is<const char*>()) {
    int l = heaterLevel(doc["heater"]["passenger"]);
    if (l < 0) return replyError(req, 400, "invalid_arguments");
    v.heater[SEAT_PASSENGER] = l;
    fields |= VS_HEATER_PASSENGER;
  }
  if (doc["defrost"]["front"].is<bool>()) {
    v.defrost[DEFROST_FRONT] = doc["defrost"]["front"].as<bool>();
    fields |= VS_DEFROST_FRONT;
  }
  if (doc["defrost"]["rear"].is<bool>()) {
    v.defrost[DEFROST_REAR] = doc["defrost"]["rear"].as<bool>();
    fields |= VS_DEFROST_REAR;
  }
  if (fields == 0) return replyError(req, 400, "nothing_to_assume");

  vstateAssume(v, fields);
  Logf(INFO, "API: tracked state corrected by '%s' (fields 0x%02X)", call.who.keyId, fields);
  JsonDocument out;
  out["assumed"] = true;
  return reply(req, 200, out);
}

// GET /api/v1/jobs/<id>
static esp_err_t handleJob(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Control, call)) return ESP_OK;
  const char* idText = call.path + strlen("/api/v1/jobs/");
  char* end = nullptr;
  unsigned long id = strtoul(idText, &end, 10);
  JobInfo info;
  if (*idText == '\0' || *end != '\0' || !actionsGet((uint32_t)id, info)) return replyError(req, 404, "not_found");
  JsonDocument doc;
  doc["id"] = info.id;
  doc["status"] = info.state == JobState::Done ? "done" : info.state == JobState::Running ? "running" : "queued";
  if (info.state == JobState::Done) {
    doc["ok"] = info.ok;
    doc["result"] = info.message;
  }
  return reply(req, 200, doc);
}

// --- endpoints: admin --------------------------------------------------------------------
// GET /api/v1/admin/keys
static esp_err_t handleKeysList(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  JsonArray arr = doc["keys"].to<JsonArray>();
  KeyInfo k;
  for (int i = 0; i < authKeyCount() && authKeyAt(i, k); i++) {
    char fp[17];
    authFingerprint(k.pub, fp);
    JsonObject o = arr.add<JsonObject>();
    o["id"] = k.id;
    o["role"] = roleName(k.role);
    o["fingerprint"] = fp;
    char b64[100];
    size_t n = 0;
    if (mbedtls_base64_encode((unsigned char*)b64, sizeof(b64), &n, k.pub, 65) == 0) o["public_key"] = b64;   // public, so the admin page can change a role by re-adding
  }
  return reply(req, 200, doc);
}

// POST /api/v1/admin/keys  {"id":"...","role":"control|admin","public_key":"<base64 65 bytes>"}
static esp_err_t handleKeysAdd(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(req, 400, "invalid_json");
  const char* id = doc["id"] | "";
  const char* roleText = doc["role"] | "";
  const char* pubText = doc["public_key"] | "";
  Role role;
  if (strcmp(roleText, "admin") == 0) role = Role::Admin;
  else if (strcmp(roleText, "control") == 0) role = Role::Control;
  else return replyError(req, 400, "invalid_arguments");
  uint8_t pub[65];
  if (!authParsePublicKey(pubText, pub)) return replyError(req, 400, "invalid_public_key");
  if (strcmp(id, call.who.keyId) == 0 && role != call.who.role) return replyError(req, 409, "cannot_change_own_role");
  switch (authKeyAdd(id, role, pub)) {
    case KeyAdd::Ok: {
      Logf(INFO, "API: key '%s' enrolled as %s by '%s'", id, roleText, call.who.keyId);
      JsonDocument out;
      out["id"] = id;
      out["role"] = roleText;
      return reply(req, 201, out);
    }
    case KeyAdd::Full: return replyError(req, 409, "key_store_full");
    default:           return replyError(req, 400, "invalid_arguments");
  }
}

// DELETE /api/v1/admin/keys/<id>   (a key cannot delete itself, to avoid locking yourself out)
static esp_err_t handleKeysDelete(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  const char* id = call.path + strlen("/api/v1/admin/keys/");
  if (strcmp(id, call.who.keyId) == 0) return replyError(req, 409, "cannot_delete_own_key");
  if (!authKeyRemove(id)) return replyError(req, 404, "not_found");
  Logf(INFO, "API: key '%s' removed by '%s'", id, call.who.keyId);
  JsonDocument out;
  out["removed"] = id;
  return reply(req, 200, out);
}

// --- server lifecycle -----------------------------------------------------------------
void apiAddHandler(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = method;
  u.handler = handler;
  httpd_register_uri_handler(server, &u);
}

static bool startServer() {
  IPAddress bindIp;
  if (!bindIp.fromString(WG_LOCAL_IP)) {
    Logf(ERROR, "API: bad WG_LOCAL_IP '%s'", WG_LOCAL_IP);
    return false;
  }
  tunnelAddr = (uint32_t)bindIp;

  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.servercert = TLS_CERT_DER;        // DER is accepted as well as PEM
  conf.servercert_len = sizeof(TLS_CERT_DER);
  conf.prvtkey_pem = TLS_KEY_DER;
  conf.prvtkey_len = sizeof(TLS_KEY_DER);
  conf.port_secure = API_PORT;
  conf.httpd.max_open_sockets = 2;
  conf.httpd.max_uri_handlers = 32;
  conf.httpd.uri_match_fn = httpd_uri_match_wildcard;
  conf.httpd.lru_purge_enable = true;
  conf.httpd.stack_size = 12288;

  if (httpd_ssl_start(&server, &conf) != ESP_OK) {
    Log(ERROR, "API: HTTPS server failed to start");
    server = nullptr;
    return false;
  }
  apiAddHandler("/api/v1/challenge", HTTP_GET, handleChallenge);
  apiAddHandler("/api/v1/heater", HTTP_POST, handleHeater);
  apiAddHandler("/api/v1/defrost", HTTP_POST, handleDefrost);
  apiAddHandler("/api/v1/temperature", HTTP_POST, handleTemperature);
  apiAddHandler("/api/v1/fan", HTTP_POST, handleFan);
  apiAddHandler("/api/v1/assume", HTTP_POST, handleAssume);
  apiAddHandler("/api/v1/power", HTTP_POST, handlePower);
  apiAddHandler("/api/v1/remote-start", HTTP_POST, handleRemoteStart);
  apiAddHandler("/api/v1/state", HTTP_GET, handleState);
  apiAddHandler("/api/v1/jobs/*", HTTP_GET, handleJob);
  apiAddHandler("/api/v1/admin/keys", HTTP_GET, handleKeysList);
  apiAddHandler("/api/v1/admin/keys", HTTP_POST, handleKeysAdd);
  apiAddHandler("/api/v1/admin/keys/*", HTTP_DELETE, handleKeysDelete);
  adminRegister();
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handleNotFound);

  Logf(INFO, "API: listening on https://%s:%u/ (certificate fingerprint %s)", WG_LOCAL_IP, API_PORT, TLS_CERT_FINGERPRINT);
  return true;
}

static void stopServer() {
  httpd_ssl_stop(server);
  server = nullptr;
  Log(WARN, "API: stopped (tunnel is down)");
}

void apiPoll() {
  adminPoll();
  bool want = wgStarted();
  if (want && !running && (int32_t)(millis() - nextStartTry) >= 0) {
    running = startServer();
    if (!running) nextStartTry = millis() + 10000;
  } else if (!want && running) {
    stopServer();
    running = false;
  }
}

const char* apiCertFingerprint() {
  return TLS_CERT_FINGERPRINT;
}

bool apiRunning() {
  return running;
}

void apiPrintStatus() {
  Logf(INFO, "API: %s, https://%s:%u/", running ? "running" : "not running (needs the WireGuard tunnel)", WG_LOCAL_IP, API_PORT);
  Logf(INFO, "Certificate fingerprint: %s", TLS_CERT_FINGERPRINT);
  Logf(INFO, "Enrolled keys: %d, remote start over the network: %s", authKeyCount(),
       remoteStartEnabled() ? "enabled" : "disabled");
}
