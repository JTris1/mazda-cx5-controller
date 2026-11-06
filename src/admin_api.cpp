#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include "actions.h"
#include "api_internal.h"
#include "admin_page.gen.h"
#include "api.h"
#include "auth.h"
#include "config.h"
#include "link_watch.h"
#include "log.h"
#include "net_cell.h"
#include "net_wg.h"
#include "ota.h"
#include "remote_start.h"
#include "settings.h"

// Admin page, device information, settings, first-key setup and reboot. Everything here is only reachable on
// the tunnel (see arrivedOnTunnel) and, except the page itself and the setup calls, needs a signed admin request.

static uint32_t rebootAt = 0;   // millis() deadline; 0 = none

// GET /admin  (the page is public over the tunnel and holds no secrets; every action in it is a signed call)
static esp_err_t handlePage(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; "
                     "img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
  return httpd_resp_send(req, (const char*)ADMIN_PAGE_GZ, ADMIN_PAGE_GZ_LEN);
}

// GET /api/v1/setup  (unauthenticated) -> {"setup_open":bool,"firmware":"..."}
// The admin page uses this to know whether the device still has no keys at all.
static esp_err_t handleSetupInfo(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  JsonDocument doc;
  doc["setup_open"] = authKeyCount() == 0;
  doc["firmware"] = FIRMWARE_VERSION;
  return reply(req, 200, doc);
}

// POST /api/v1/setup/first-key  {"id":"...","public_key":"<base64>"}   unauthenticated, only while NO key exists.
// Always creates an admin key. Adding any key (here, on the console, or through the API) closes this for good;
// "keywipe yes" on the console reopens it.
static esp_err_t handleFirstKey(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  static uint32_t windowStart = 0;
  static int attempts = 0;
  uint32_t now = millis();
  if (now - windowStart > 60000 || attempts == 0) { windowStart = now; attempts = 0; }
  if (++attempts > 5) return replyError(req, 429, "rate_limited");

  Call call;
  if (!readBody(req, call)) return ESP_OK;
  if (authKeyCount() != 0) {
    Log(WARN, "Setup: first-key refused, keys already exist");
    return replyError(req, 409, "setup_closed");
  }
  JsonDocument doc;
  if (!parseBody(call, doc)) return replyError(req, 400, "invalid_json");
  const char* id = doc["id"] | "";
  uint8_t pub[65];
  if (!authParsePublicKey(doc["public_key"] | "", pub)) return replyError(req, 400, "invalid_public_key");
  if (authKeyAdd(id, Role::Admin, pub) != KeyAdd::Ok) return replyError(req, 400, "invalid_arguments");
  Logf(WARN, "Setup: first admin key '%s' enrolled from %u.%u.%u.%u", id, (clientIp(req) >> 24) & 255,
       (clientIp(req) >> 16) & 255, (clientIp(req) >> 8) & 255, clientIp(req) & 255);
  JsonDocument out;
  out["id"] = id;
  out["role"] = "admin";
  return reply(req, 201, out);
}

static const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return linkWatchRestartedUs() ? "link watchdog restart" : "software restart";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_BROWNOUT: return "brownout (supply dipped)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_EXT:      return "reset pin";
    default:               return "other";
  }
}

// GET /api/v1/admin/info
static esp_err_t handleInfo(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["cert_fingerprint"] = apiCertFingerprint();
  doc["build"] = __DATE__ " " __TIME__;
  doc["uptime_s"] = millis() / 1000;
  doc["reset_reason"] = resetReasonText();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["min_free_heap"] = ESP.getMinFreeHeap();
  doc["cellular"] = cellConnected();
  doc["tunnel"] = wgStarted();
  doc["keys"] = authKeyCount();
  LinkWatchInfo lw;
  linkWatchInfo(lw);
  JsonObject link = doc["link"].to<JsonObject>();
  link["tunnel_up"] = lw.tunnelUp;
  link["bad_for_s"] = lw.badForSeconds;
  link["watchdog_step"] = lw.step;
  link["watchdog_restarts"] = lw.watchdogRestarts;
  doc["you"] = call.who.keyId;
  doc["remote_start_enabled"] = remoteStartEnabled();
  const esp_partition_t* run = esp_ota_get_running_partition();
  doc["running_slot"] = run ? run->label : "?";
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  doc["next_slot"] = next ? next->label : "?";
  doc["slot_size"] = next ? next->size : 0;
  return reply(req, 200, doc);
}

// GET /api/v1/admin/log
static esp_err_t handleLog(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  JsonArray arr = doc["entries"].to<JsonArray>();
  AuthLogItem item;
  for (int i = 0; authLogAt(i, item); i++) {
    JsonObject o = arr.add<JsonObject>();
    o["age_s"] = item.ageSeconds;
    char ip[16];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (item.ip >> 24) & 255, (item.ip >> 16) & 255, (item.ip >> 8) & 255, item.ip & 255);
    o["ip"] = ip;
    o["key"] = item.keyId;
    o["result"] = authResultText(item.result);
  }
  return reply(req, 200, doc);
}

// GET /api/v1/admin/logs  -> {"current":"...","previous":"..."}  (every line the device printed, newest last)
static esp_err_t handleLogs(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  doc["current"] = logHistory(false);
  doc["previous"] = logHistory(true);
  doc["reset_reason"] = resetReasonText();
  return reply(req, 200, doc);
}

// GET /api/v1/admin/settings
static esp_err_t handleSettingsGet(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  settingsToJson(doc.to<JsonObject>());
  return reply(req, 200, doc);
}

// POST /api/v1/admin/settings  {any subset of the fields GET returns}
static esp_err_t handleSettingsSet(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc) || !doc.is<JsonObject>()) return replyError(req, 400, "invalid_json");
  uint32_t running = 0;
  int queued = 0;
  actionsBusyInfo(running, queued);
  if (running || queued) return replyError(req, 503, "busy");   // limits must not change under a running job
  const char* why = "";
  if (!settingsApply(doc.as<JsonObjectConst>(), &why)) {
    JsonDocument out;
    out["error"] = "invalid_arguments";
    out["reason"] = why;
    return reply(req, 400, out);
  }
  Logf(INFO, "API: settings changed by '%s'", call.who.keyId);
  JsonDocument out;
  settingsToJson(out.to<JsonObject>());
  return reply(req, 200, out);
}

// POST /api/v1/admin/remote-start-enabled  {"enabled":true|false}
static esp_err_t handleRemoteStartEnabled(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc) || !doc["enabled"].is<bool>()) return replyError(req, 400, "invalid_arguments");
  setRemoteStartEnabled(doc["enabled"].as<bool>());
  Logf(WARN, "API: remote start %s by '%s'", remoteStartEnabled() ? "ENABLED" : "disabled", call.who.keyId);
  JsonDocument out;
  out["enabled"] = remoteStartEnabled();
  return reply(req, 200, out);
}

// POST /api/v1/admin/reboot  (replies first, restarts about a second later)
static esp_err_t handleReboot(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  if (otaReceiving()) return replyError(req, 409, "update_in_progress");
  Logf(WARN, "API: reboot requested by '%s'", call.who.keyId);
  rebootAt = millis() + 1000;
  if (rebootAt == 0) rebootAt = 1;
  JsonDocument out;
  out["rebooting"] = true;
  return reply(req, 202, out);
}

// --- firmware update (see ota.h) ------------------------------------------------------------
static const char* otaStateName(OtaState st) {
  switch (st) {
    case OtaState::Idle:      return "idle";
    case OtaState::Armed:     return "armed";
    case OtaState::Receiving: return "receiving";
    case OtaState::Ready:     return "ready_to_reboot";
  }
  return "idle";
}

static void fillOtaStatus(JsonObject o) {
  OtaInfo info;
  otaInfo(info);
  o["state"] = otaStateName(info.state);
  o["received"] = info.received;
  o["size"] = info.size;
  o["version"] = info.version;
  o["pending_verify"] = info.pendingVerify;     // the running image is still on probation (rollback possible)
  o["running_slot"] = esp_ota_get_running_partition()->label;
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  o["slot_size"] = next ? next->size : 0;
}

// GET /api/v1/admin/firmware
static esp_err_t handleFirmwareStatus(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  fillOtaStatus(doc.to<JsonObject>());
  return reply(req, 200, doc);
}

// POST /api/v1/admin/firmware  {"size":N,"sha256":"<hex>","version":"..."}   signed by an admin key
// -> {"upload_token":"...","expires_in":600}. The signature covers the declared hash, so the image bytes that
// follow are bound to an admin's authority without signing the whole upload inside the 30 s nonce window.
static esp_err_t handleFirmwareStart(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  Call call;
  if (!authenticate(req, Role::Admin, call)) return ESP_OK;
  JsonDocument doc;
  if (!parseBody(call, doc) || !doc["size"].is<uint32_t>()) return replyError(req, 400, "invalid_json");
  const char* version = doc["version"] | "";
  if (strlen(version) == 0 || strlen(version) > 31) return replyError(req, 400, "invalid_arguments");
  char token[33];
  const char* why = "";
  if (!otaStart(doc["size"].as<uint32_t>(), doc["sha256"] | "", version, token, &why)) {
    JsonDocument out;
    out["error"] = "refused";
    out["reason"] = why;
    return reply(req, 409, out);
  }
  Logf(WARN, "API: firmware update '%s' armed by '%s'", version, call.who.keyId);
  JsonDocument out;
  out["upload_token"] = token;
  out["expires_in"] = OTA_TOKEN_MS / 1000;
  return reply(req, 200, out);
}

// PUT /api/v1/admin/firmware/<token>   body = the raw image, streamed to flash
static esp_err_t handleFirmwareUpload(httpd_req_t* req) {
  if (!arrivedOnTunnel(req)) return ESP_FAIL;
  const char* presented = req->uri + strlen("/api/v1/admin/firmware/");
  const char* why = "";
  if (!otaWriteBegin(presented, &why)) {
    Logf(WARN, "OTA: upload refused: %s", why);
    replyError(req, 403, "forbidden");
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return ESP_OK;
  }
  static uint8_t buf[4096];       // one upload at a time (otaWriteBegin refuses a second), so static is safe
  size_t remaining = req->content_len;
  int idle = 0;
  uint32_t startedAt = millis();
  size_t total = remaining, nextLog = total / 10;
  while (remaining > 0) {
    int n = httpd_req_recv(req, (char*)buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++idle >= 5) { why = "upload stalled"; break; }
      continue;
    }
    if (n <= 0) { why = "connection lost during upload"; break; }
    idle = 0;
    if (!otaWriteChunk(buf, (size_t)n, &why)) { remaining = 1; break; }
    remaining -= n;
    if (total - remaining >= nextLog) {
      Logf(INFO, "OTA: %u%% (%lu s)", (unsigned)((total - remaining) * 100 / total), (unsigned long)((millis() - startedAt) / 1000));
      nextLog += total / 10;
    }
  }
  if (remaining == 0 && otaWriteFinish(&why)) {
    Logf(INFO, "OTA: %u bytes in %lu s", (unsigned)total, (unsigned long)((millis() - startedAt) / 1000));
    JsonDocument out;
    out["ok"] = true;
    out["reboot_required"] = true;
    return reply(req, 200, out);
  }
  otaAbort();
  Logf(ERROR, "OTA: failed: %s", why);
  JsonDocument out;
  out["error"] = "upload_failed";
  out["reason"] = why;
  reply(req, 400, out);
  httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
  return ESP_OK;
}

void adminRegister() {
  apiAddHandler("/admin", HTTP_GET, handlePage);
  apiAddHandler("/api/v1/setup", HTTP_GET, handleSetupInfo);
  apiAddHandler("/api/v1/setup/first-key", HTTP_POST, handleFirstKey);
  apiAddHandler("/api/v1/admin/info", HTTP_GET, handleInfo);
  apiAddHandler("/api/v1/admin/log", HTTP_GET, handleLog);
  apiAddHandler("/api/v1/admin/logs", HTTP_GET, handleLogs);
  apiAddHandler("/api/v1/admin/settings", HTTP_GET, handleSettingsGet);
  apiAddHandler("/api/v1/admin/settings", HTTP_POST, handleSettingsSet);
  apiAddHandler("/api/v1/admin/remote-start-enabled", HTTP_POST, handleRemoteStartEnabled);
  apiAddHandler("/api/v1/admin/reboot", HTTP_POST, handleReboot);
  apiAddHandler("/api/v1/admin/firmware", HTTP_GET, handleFirmwareStatus);
  apiAddHandler("/api/v1/admin/firmware", HTTP_POST, handleFirmwareStart);
  apiAddHandler("/api/v1/admin/firmware/*", HTTP_PUT, handleFirmwareUpload);
}

void adminPoll() {
  otaPoll();
  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) {
    Log(WARN, "Rebooting now");
    delay(100);
    ESP.restart();
  }
}
