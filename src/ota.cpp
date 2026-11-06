#include <esp_ota_ops.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include "ota.h"
#include "actions.h"
#include "api.h"
#include "engine.h"
#include "log.h"
#include "net_cell.h"
#include "net_wg.h"

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
#define SHA_STARTS(c) mbedtls_sha256_starts(c, 0)
#define SHA_UPDATE(c, d, n) mbedtls_sha256_update(c, d, n)
#define SHA_FINISH(c, out) mbedtls_sha256_finish(c, out)
#else
#define SHA_STARTS(c) mbedtls_sha256_starts_ret(c, 0)
#define SHA_UPDATE(c, d, n) mbedtls_sha256_update_ret(c, d, n)
#define SHA_FINISH(c, out) mbedtls_sha256_finish_ret(c, out)
#endif

// The Arduino core marks a freshly updated image valid at startup unless this returns true. We confirm it
// ourselves, in otaPoll(), only after the network and API have proven healthy.
extern "C" bool verifyRollbackLater() {
  return true;
}

static OtaState state = OtaState::Idle;
static char token[33];
static uint32_t armedAt = 0;
static uint32_t lastDataAt = 0;
static uint32_t declaredSize = 0;
static uint32_t received = 0;
static uint8_t declaredHash[32];
static char declaredVersion[32];
static const esp_partition_t* target = nullptr;
static esp_ota_handle_t handle = 0;
static mbedtls_sha256_context shaCtx;

static bool hexToBytes(const char* hex, uint8_t* out, size_t n) {
  if (strlen(hex) != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char pair[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char* end = nullptr;
    long v = strtol(pair, &end, 16);
    if (*end != '\0' || !isxdigit((unsigned char)pair[0]) || !isxdigit((unsigned char)pair[1])) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

static void reset() {
  if (state == OtaState::Receiving && handle) esp_ota_abort(handle);
  handle = 0;
  token[0] = '\0';
  state = OtaState::Idle;
}

bool otaReceiving() {
  return state == OtaState::Receiving;
}

bool otaStart(uint32_t size, const char* sha256Hex, const char* version, char outToken[33], const char** error) {
  if (state == OtaState::Receiving) { *error = "an upload is already in progress"; return false; }
  uint32_t running = 0;
  int queued = 0;
  actionsBusyInfo(running, queued);
  if (running || queued) { *error = "the car is busy with a command"; return false; }
  if (engineRunning) { *error = "the engine is running"; return false; }
  target = esp_ota_get_next_update_partition(nullptr);
  if (!target) { *error = "no update slot"; return false; }
  if (size < 100000 || size > target->size) { *error = "size is not plausible for this device"; return false; }
  if (!hexToBytes(sha256Hex, declaredHash, 32)) { *error = "sha256 must be 64 hex digits"; return false; }
  strlcpy(declaredVersion, version, sizeof(declaredVersion));
  declaredSize = size;
  received = 0;
  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  for (int i = 0; i < 16; i++) snprintf(token + 2 * i, 3, "%02x", raw[i]);
  strcpy(outToken, token);
  armedAt = millis();
  state = OtaState::Armed;
  Logf(WARN, "OTA: update to '%s' (%lu bytes) armed for slot %s", declaredVersion, (unsigned long)size, target->label);
  return true;
}

bool otaWriteBegin(const char* presented, const char** error) {
  if (state != OtaState::Armed) { *error = "no update is armed"; return false; }
  bool fresh = millis() - armedAt < OTA_TOKEN_MS;
  bool match = strlen(presented) == 32 && memcmp(presented, token, 32) == 0;
  token[0] = '\0';                        // single use, whatever happens next
  if (!fresh || !match) { state = OtaState::Idle; *error = "token invalid or expired"; return false; }
  esp_err_t err = esp_ota_begin(target, declaredSize, &handle);
  if (err != ESP_OK) {
    Logf(ERROR, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
    state = OtaState::Idle;
    *error = "could not prepare the update slot";
    return false;
  }
  mbedtls_sha256_init(&shaCtx);
  SHA_STARTS(&shaCtx);
  received = 0;
  lastDataAt = millis();
  state = OtaState::Receiving;
  return true;
}

bool otaWriteChunk(const uint8_t* data, size_t len, const char** error) {
  if (state != OtaState::Receiving) {
    Logf(ERROR, "OTA: chunk arrived but the update is not active (state %d, %lu of %lu bytes)", (int)state, (unsigned long)received, (unsigned long)declaredSize);
    *error = "not receiving";
    return false;
  }
  if (received + len > declaredSize) { *error = "more data than declared"; otaAbort(); return false; }
  esp_err_t err = esp_ota_write(handle, data, len);
  if (err != ESP_OK) {
    Logf(ERROR, "OTA: flash write failed: %s", esp_err_to_name(err));
    *error = "flash write failed";
    otaAbort();
    return false;
  }
  SHA_UPDATE(&shaCtx, data, len);
  received += len;
  lastDataAt = millis();
  return true;
}

bool otaWriteFinish(const char** error) {
  if (state != OtaState::Receiving) {
    Logf(ERROR, "OTA: upload ended but the update is not active (state %d, %lu of %lu bytes)", (int)state, (unsigned long)received, (unsigned long)declaredSize);
    *error = "not receiving";
    return false;
  }
  uint8_t digest[32];
  SHA_FINISH(&shaCtx, digest);
  mbedtls_sha256_free(&shaCtx);
  if (received != declaredSize) { *error = "size does not match what was declared"; otaAbort(); return false; }
  if (memcmp(digest, declaredHash, 32) != 0) {
    Log(ERROR, "OTA: SHA-256 of the upload does not match the signed manifest, discarded");
    *error = "sha256 mismatch";
    otaAbort();
    return false;
  }
  esp_err_t err = esp_ota_end(handle);
  handle = 0;
  if (err != ESP_OK) {
    Logf(ERROR, "OTA: image rejected: %s", esp_err_to_name(err));
    state = OtaState::Idle;
    *error = "not a valid firmware image";
    return false;
  }
  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    Logf(ERROR, "OTA: could not select the new slot: %s", esp_err_to_name(err));
    state = OtaState::Idle;
    *error = "could not select the new slot";
    return false;
  }
  state = OtaState::Ready;
  Logf(WARN, "OTA: '%s' written to %s and verified; reboot to run it", declaredVersion, target->label);
  return true;
}

void otaAbort() {
  if (state == OtaState::Receiving) mbedtls_sha256_free(&shaCtx);
  reset();
}

void otaInfo(OtaInfo& out) {
  out.state = state;
  out.received = received;
  out.size = declaredSize;
  strlcpy(out.version, state == OtaState::Idle ? "" : declaredVersion, sizeof(out.version));
  esp_ota_img_states_t imgState;
  const esp_partition_t* run = esp_ota_get_running_partition();
  out.pendingVerify = run && esp_ota_get_state_partition(run, &imgState) == ESP_OK && imgState == ESP_OTA_IMG_PENDING_VERIFY;
  out.confirmInSeconds = 0;
}

// Confirms the running image once everything has been healthy long enough.
static uint32_t healthySince = 0;
static bool wasHealthy = false;

void otaPoll() {
  uint32_t now = millis();
  if (state == OtaState::Armed && (int32_t)(now - armedAt) > (int32_t)OTA_TOKEN_MS) {
    Log(WARN, "OTA: armed update expired");
    reset();
  }
  if (state == OtaState::Receiving && (int32_t)(now - lastDataAt) > (int32_t)OTA_IDLE_ABORT_MS) {
    Logf(WARN, "OTA: no data for %lu s, upload abandoned (%lu of %lu bytes)", (unsigned long)((now - lastDataAt) / 1000), (unsigned long)received, (unsigned long)declaredSize);
    otaAbort();
  }

  static bool checked = false, pending = false;
  if (!checked) {                       // only the first boot of a new image is pending
    OtaInfo info;
    otaInfo(info);
    pending = info.pendingVerify;
    checked = true;
    if (pending) Log(WARN, "OTA: running a new image on probation; it is confirmed after 3 minutes of healthy running");
  }
  if (!pending) return;
  if (now > OTA_GIVE_UP_MS && !otaReceiving()) {
    Log(ERROR, "OTA: the new image never became healthy, going back to the previous one");
    delay(200);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
  bool healthy = cellConnected() && wgStarted() && apiRunning();
  if (!healthy) { wasHealthy = false; return; }
  if (!wasHealthy) { wasHealthy = true; healthySince = now; }
  if (now - healthySince >= OTA_CONFIRM_MS) {
    esp_ota_mark_app_valid_cancel_rollback();
    pending = false;
    Log(WARN, "OTA: new image confirmed, rollback cancelled");
  }
}
