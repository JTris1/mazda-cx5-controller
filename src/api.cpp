#include <ArduinoJson.h>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPSServer.hpp>
#include <ResourceNode.hpp>
#include <SSLCert.hpp>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include "api.h"
#include "actions.h"
#include "auth.h"
#include "config.h"
#include "engine.h"
#include "log.h"
#include "net_wg.h"
#include "outputs.h"
#include "remote_start.h"

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

using namespace httpsserver;

static constexpr uint16_t API_PORT = 443;
static constexpr uint8_t MAX_CONNECTIONS = 2;
static constexpr size_t MAX_BODY = 512;
static constexpr uint32_t START_MAX_NONCE_AGE_MS = 15000;  // remote start needs a fresher challenge

static SSLCert* cert = nullptr;
static HTTPSServer* server = nullptr;
static volatile bool stopRequested = false;
static volatile bool taskDone = true;
static bool running = false;
static uint32_t nextStartTry = 0;

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

static void reply(HTTPResponse* res, int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  res->setStatusCode(code);
  res->setStatusText(statusText(code));
  res->setHeader("Content-Type", "application/json");
  res->setHeader("Cache-Control", "no-store");
  res->print(out.c_str());
}

static void replyError(HTTPResponse* res, int code, const char* error) {
  JsonDocument doc;
  doc["error"] = error;
  reply(res, code, doc);
}

static void replySubmit(HTTPResponse* res, Submit result, uint32_t id) {
  switch (result) {
    case Submit::Ok: {
      JsonDocument doc;
      doc["job_id"] = id;
      doc["status"] = "queued";
      reply(res, 202, doc);
      break;
    }
    case Submit::BadArgs:    replyError(res, 400, "invalid_arguments"); break;
    case Submit::NoExpander: replyError(res, 503, "expander_missing"); break;
    case Submit::Busy:       replyError(res, 503, "busy"); break;
  }
}

static uint32_t clientIp(HTTPRequest* req) {
  IPAddress ip = req->getClientIP();
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3];
}

// --- authentication ------------------------------------------------------------------
struct Call {
  AuthOutcome who;
  char body[MAX_BODY + 1];
  size_t bodyLen = 0;
};

// Reads the body, checks the signed headers and answers 4xx itself on failure.
// Auth failures always get the same generic body, whatever went wrong.
static bool authenticate(HTTPRequest* req, HTTPResponse* res, Role needed, Call& call) {
  size_t len = req->getContentLength();
  if (len > MAX_BODY) {
    replyError(res, 413, "body_too_large");
    return false;
  }
  if (len > 0) {
    call.bodyLen = req->readBytes((byte*)call.body, len);
    if (call.bodyLen != len) {
      replyError(res, 400, "bad_body");
      return false;
    }
  }
  call.body[call.bodyLen] = '\0';

  std::string target = req->getRequestString();
  std::string method = req->getMethod();
  if (target.find('?') != std::string::npos) {   // no endpoint takes a query string
    replyError(res, 400, "unexpected_query");
    return false;
  }
  std::string key = req->getHeader("x-cx5-key");
  std::string nonce = req->getHeader("x-cx5-nonce");
  std::string sig = req->getHeader("x-cx5-signature");

  AuthRequest ar{key.c_str(), nonce.c_str(), method.c_str(), target.c_str(),
                 (const uint8_t*)call.body, call.bodyLen, sig.c_str(), clientIp(req)};
  call.who = authVerify(ar, needed);
  switch (call.who.result) {
    case AuthResult::Ok:          return true;
    case AuthResult::RateLimited: replyError(res, 429, "rate_limited"); return false;
    case AuthResult::Forbidden:   replyError(res, 403, "forbidden"); return false;
    default:                      replyError(res, 401, "unauthorized"); return false;
  }
}

static bool parseBody(const Call& call, JsonDocument& doc) {
  return call.bodyLen > 0 && deserializeJson(doc, call.body, call.bodyLen) == DeserializationError::Ok;
}

// --- endpoints: unauthenticated --------------------------------------------------------
static void handleChallenge(HTTPRequest* req, HTTPResponse* res) {
  char nonce[33];
  if (!authNewNonce(clientIp(req), nonce)) {
    replyError(res, 429, "rate_limited");
    return;
  }
  JsonDocument doc;
  doc["nonce"] = nonce;
  doc["expires_in"] = 30;
  reply(res, 200, doc);
}

static void handleNotFound(HTTPRequest* req, HTTPResponse* res) {
  replyError(res, 404, "not_found");
}

// --- endpoints: control ---------------------------------------------------------------
// POST /api/v1/heater  {"seat":"driver|passenger","level":"off|low|medium|high"}
static void handleHeater(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Control, call)) return;
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(res, 400, "invalid_json");
  const char* seat = doc["seat"] | "";
  const char* level = doc["level"] | "";

  Seat s;
  if (strcmp(seat, "driver") == 0) s = SEAT_DRIVER;
  else if (strcmp(seat, "passenger") == 0) s = SEAT_PASSENGER;
  else return replyError(res, 400, "invalid_arguments");

  int lv;
  if (strcmp(level, "off") == 0) lv = 0;
  else if (strcmp(level, "high") == 0) lv = 1;
  else if (strcmp(level, "medium") == 0) lv = 2;
  else if (strcmp(level, "low") == 0) lv = 3;
  else return replyError(res, 400, "invalid_arguments");

  uint32_t id = 0;
  replySubmit(res, actionSetHeater(s, lv, &id), id);
}

// POST /api/v1/defrost  {"zone":"front|rear","action":"on|off"}
static void handleDefrost(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Control, call)) return;
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(res, 400, "invalid_json");
  const char* zone = doc["zone"] | "";
  const char* action = doc["action"] | "";

  DefrostZone z;
  if (strcmp(zone, "front") == 0) z = DEFROST_FRONT;
  else if (strcmp(zone, "rear") == 0) z = DEFROST_REAR;
  else return replyError(res, 400, "invalid_arguments");
  bool on;
  if (strcmp(action, "on") == 0) on = true;
  else if (strcmp(action, "off") == 0) on = false;
  else return replyError(res, 400, "invalid_arguments");

  uint32_t id = 0;
  replySubmit(res, actionSetDefrost(z, on, &id), id);
}

// POST /api/v1/temperature  {"value": 60..84}
static void handleTemperature(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Control, call)) return;
  JsonDocument doc;
  if (!parseBody(call, doc) || !doc["value"].is<int>()) return replyError(res, 400, "invalid_json");
  uint32_t id = 0;
  replySubmit(res, actionSetTemperature(doc["value"].as<int>(), &id), id);
}

// POST /api/v1/fan  {"action":"up|down","steps":1..}   (steps defaults to 1)
static void handleFan(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Control, call)) return;
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(res, 400, "invalid_json");
  const char* action = doc["action"] | "";
  bool up;
  if (strcmp(action, "up") == 0) up = true;
  else if (strcmp(action, "down") == 0) up = false;
  else return replyError(res, 400, "invalid_arguments");
  int steps = doc["steps"] | 1;
  uint32_t id = 0;
  replySubmit(res, actionFanStep(up, steps, &id), id);
}

// POST /api/v1/power  {"action":"toggle"}
static void handlePower(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Control, call)) return;
  JsonDocument doc;
  if (!parseBody(call, doc) || strcmp(doc["action"] | "", "toggle") != 0) return replyError(res, 400, "invalid_arguments");
  uint32_t id = 0;
  replySubmit(res, actionPower(&id), id);
}

// POST /api/v1/remote-start  {}
static void handleRemoteStart(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Control, call)) return;
  if (call.who.nonceAgeMs > START_MAX_NONCE_AGE_MS) return replyError(res, 401, "unauthorized");  // stale challenge
  const char* reason = "";
  if (!remoteStartPolicyAllow(&reason)) {
    Logf(WARN, "API: remote start refused: %s", reason);
    JsonDocument doc;
    doc["error"] = "refused";
    doc["reason"] = reason;
    return reply(res, 403, doc);
  }
  Logf(INFO, "API: remote start requested by key '%s'", call.who.keyId);
  uint32_t id = 0;
  replySubmit(res, actionRemoteStart(&id), id);
}

// GET /api/v1/state
static void handleState(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Control, call)) return;
  JsonDocument doc;
  doc["engine_running"] = (bool)engineRunning;
  doc["expander_ok"] = expanderOk;
  doc["remote_start_enabled"] = remoteStartEnabled();
  doc["uptime_s"] = millis() / 1000;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["firmware"] = FIRMWARE_VERSION;
  reply(res, 200, doc);
}

// GET /api/v1/jobs/<id>
static void handleJob(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Control, call)) return;
  std::string idText = req->getParams()->getPathParameter(0);
  char* end = nullptr;
  unsigned long id = strtoul(idText.c_str(), &end, 10);
  JobInfo info;
  if (idText.empty() || *end != '\0' || !actionsGet((uint32_t)id, info)) return replyError(res, 404, "not_found");
  JsonDocument doc;
  doc["id"] = info.id;
  doc["status"] = info.state == JobState::Done ? "done" : info.state == JobState::Running ? "running" : "queued";
  if (info.state == JobState::Done) {
    doc["ok"] = info.ok;
    doc["result"] = info.message;
  }
  reply(res, 200, doc);
}

// --- endpoints: admin --------------------------------------------------------------------
// GET /api/v1/admin/keys
static void handleKeysList(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Admin, call)) return;
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
  }
  reply(res, 200, doc);
}

// POST /api/v1/admin/keys  {"id":"...","role":"control|admin","public_key":"<base64 65 bytes>"}
static void handleKeysAdd(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Admin, call)) return;
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(res, 400, "invalid_json");
  const char* id = doc["id"] | "";
  const char* roleText = doc["role"] | "";
  const char* pubText = doc["public_key"] | "";
  Role role;
  if (strcmp(roleText, "admin") == 0) role = Role::Admin;
  else if (strcmp(roleText, "control") == 0) role = Role::Control;
  else return replyError(res, 400, "invalid_arguments");
  uint8_t pub[65];
  if (!authParsePublicKey(pubText, pub)) return replyError(res, 400, "invalid_public_key");
  switch (authKeyAdd(id, role, pub)) {
    case KeyAdd::Ok: {
      Logf(INFO, "API: key '%s' enrolled as %s by '%s'", id, roleText, call.who.keyId);
      JsonDocument out;
      out["id"] = id;
      out["role"] = roleText;
      reply(res, 201, out);
      break;
    }
    case KeyAdd::Full: replyError(res, 409, "key_store_full"); break;
    default:           replyError(res, 400, "invalid_arguments"); break;
  }
}

// DELETE /api/v1/admin/keys/<id>   (a key cannot delete itself, to avoid locking yourself out)
static void handleKeysDelete(HTTPRequest* req, HTTPResponse* res) {
  Call call;
  if (!authenticate(req, res, Role::Admin, call)) return;
  std::string id = req->getParams()->getPathParameter(0);
  if (id == call.who.keyId) return replyError(res, 409, "cannot_delete_own_key");
  if (!authKeyRemove(id.c_str())) return replyError(res, 404, "not_found");
  Logf(INFO, "API: key '%s' removed by '%s'", id.c_str(), call.who.keyId);
  JsonDocument out;
  out["removed"] = id.c_str();
  reply(res, 200, out);
}

// --- server lifecycle -----------------------------------------------------------------
static void apiTask(void*) {
  while (!stopRequested) {
    server->loop();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  taskDone = true;
  vTaskDelete(nullptr);
}

static void add(const char* path, const char* method, HTTPSCallbackFunction* handler) {
  server->registerNode(new ResourceNode(path, method, handler));
}

static bool startServer() {
  IPAddress bindIp;
  if (!bindIp.fromString(WG_LOCAL_IP)) {
    Logf(ERROR, "API: bad WG_LOCAL_IP '%s'", WG_LOCAL_IP);
    return false;
  }
  cert = new SSLCert(TLS_CERT_DER, sizeof(TLS_CERT_DER), TLS_KEY_DER, sizeof(TLS_KEY_DER));
  // Bound to the tunnel address only: nothing answers on the hotspot or any other interface.
  server = new HTTPSServer(cert, API_PORT, MAX_CONNECTIONS, (in_addr_t)bindIp);

  add("/api/v1/challenge", "GET", handleChallenge);
  add("/api/v1/heater", "POST", handleHeater);
  add("/api/v1/defrost", "POST", handleDefrost);
  add("/api/v1/temperature", "POST", handleTemperature);
  add("/api/v1/fan", "POST", handleFan);
  add("/api/v1/power", "POST", handlePower);
  add("/api/v1/remote-start", "POST", handleRemoteStart);
  add("/api/v1/state", "GET", handleState);
  add("/api/v1/jobs/*", "GET", handleJob);
  add("/api/v1/admin/keys", "GET", handleKeysList);
  add("/api/v1/admin/keys", "POST", handleKeysAdd);
  add("/api/v1/admin/keys/*", "DELETE", handleKeysDelete);
  server->setDefaultNode(new ResourceNode("", "GET", handleNotFound));

  if (server->start() == 0 || !server->isRunning()) {
    Log(ERROR, "API: HTTPS server failed to start");
    delete server;
    delete cert;
    server = nullptr;
    cert = nullptr;
    return false;
  }
  stopRequested = false;
  taskDone = false;
  xTaskCreatePinnedToCore(apiTask, "api", 16384, nullptr, 1, nullptr, 0);
  Logf(INFO, "API: listening on https://%s:%u/ (certificate fingerprint %s)", WG_LOCAL_IP, API_PORT, TLS_CERT_FINGERPRINT);
  return true;
}

static void stopServer() {
  stopRequested = true;
  for (int i = 0; i < 200 && !taskDone; i++) delay(10);
  server->stop();
  delete server;
  delete cert;
  server = nullptr;
  cert = nullptr;
  Log(WARN, "API: stopped (tunnel is down)");
}

void apiPoll() {
  bool want = wgStarted();
  if (want && !running && (int32_t)(millis() - nextStartTry) >= 0) {
    running = startServer();
    if (!running) nextStartTry = millis() + 10000;
  } else if (!want && running) {
    stopServer();
    running = false;
  }
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
