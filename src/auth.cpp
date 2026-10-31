#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include "auth.h"
#include "auth_vector.h"
#include "log.h"

// --- limits ---------------------------------------------------------------------------
static constexpr uint32_t NONCE_TTL_MS = 30000;      // a challenge is valid this long
static constexpr int NONCE_SLOTS = 6;
static constexpr int IP_SLOTS = 8;
static constexpr int FAIL_LIMIT = 5;                 // failures within the window ...
static constexpr uint32_t FAIL_WINDOW_MS = 60000;
static constexpr uint32_t BLOCK_MS = 60000;          // ... block the address this long
static constexpr int CHALLENGE_LIMIT = 30;           // challenges per window per address
static constexpr int LOG_SIZE = 32;

static uint64_t nowMs() { return (uint64_t)esp_timer_get_time() / 1000ULL; }

// --- state (all guarded by stateMutex) ----------------------------------------------
struct KeyRecord {
  char id[AUTH_ID_MAX + 1];
  uint8_t role;
  uint8_t used;
  uint8_t pub[65];
};

struct NonceSlot {
  bool used;
  char hex[33];
  uint64_t issuedMs;
};

struct IpState {
  bool used;
  uint32_t ip;
  uint8_t fails;
  uint64_t failWindowStart;
  uint64_t blockedUntil;
  uint8_t challenges;
  uint64_t challengeWindowStart;
};

struct LogEntry {
  uint64_t ms;
  uint32_t ip;
  AuthResult result;
  char keyId[AUTH_ID_MAX + 1];
};

static SemaphoreHandle_t stateMutex = nullptr;
static Preferences prefs;
static KeyRecord keys[AUTH_MAX_KEYS];
static NonceSlot nonces[NONCE_SLOTS];
static IpState ips[IP_SLOTS];
static LogEntry logRing[LOG_SIZE];
static int logCount = 0;

struct Lock {
  Lock() { xSemaphoreTake(stateMutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(stateMutex); }
};

// --- crypto helpers ------------------------------------------------------------------
static void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  mbedtls_sha256(data, len, out, 0);
#else
  mbedtls_sha256_ret(data, len, out, 0);
#endif
}

static void toHex(const uint8_t* in, size_t len, char* out) {
  static const char* digits = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 15];
  }
  out[2 * len] = '\0';
}

static bool pointOnCurve(const uint8_t pub[65]) {
  if (pub[0] != 0x04) return false;
  mbedtls_ecp_group grp;
  mbedtls_ecp_point Q;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&Q);
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_point_read_binary(&grp, &Q, pub, 65) == 0 &&
            mbedtls_ecp_check_pubkey(&grp, &Q) == 0;
  mbedtls_ecp_point_free(&Q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

static bool verifyP256(const uint8_t pub[65], const uint8_t digest[32], const uint8_t sig[64]) {
  mbedtls_ecp_group grp;
  mbedtls_ecp_point Q;
  mbedtls_mpi r, s;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&Q);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_point_read_binary(&grp, &Q, pub, 65) == 0 &&
            mbedtls_ecp_check_pubkey(&grp, &Q) == 0 &&
            mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
            mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0 &&
            mbedtls_ecdsa_verify(&grp, digest, 32, &Q, &r, &s) == 0;
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

// sha256 of the signed string described in auth.h. False if the inputs are too long.
static bool signedDigest(const char* nonce, const char* keyId, const char* method, const char* path,
                         const uint8_t* body, size_t bodyLen, uint8_t out[32]) {
  uint8_t bodyHash[32];
  char bodyHex[65];
  sha256(body, bodyLen, bodyHash);
  toHex(bodyHash, 32, bodyHex);

  char buf[320];
  int n = snprintf(buf, sizeof(buf), "CX5-V1\n%s\n%s\n%s\n%s\n%s", nonce, keyId, method, path, bodyHex);
  if (n < 0 || n >= (int)sizeof(buf)) return false;
  sha256((const uint8_t*)buf, (size_t)n, out);
  return true;
}

// --- key store -----------------------------------------------------------------------
static void saveKeys() {
  prefs.putBytes("keys", keys, sizeof(keys));
}

void authBegin() {
  stateMutex = xSemaphoreCreateMutex();
  memset(keys, 0, sizeof(keys));
  memset(nonces, 0, sizeof(nonces));
  memset(ips, 0, sizeof(ips));
  prefs.begin("auth", false);
  if (prefs.isKey("keys") && prefs.getBytesLength("keys") == sizeof(keys)) {
    prefs.getBytes("keys", keys, sizeof(keys));
  }
  Logf(INFO, "Auth: %d enrolled key(s)", authKeyCount());
}

void authFingerprint(const uint8_t pub[65], char out[17]) {
  uint8_t h[32];
  sha256(pub, 65, h);
  toHex(h, 8, out);
}

const char* roleName(Role r) {
  return r == Role::Admin ? "admin" : "control";
}

static bool validId(const char* id) {
  size_t n = id ? strlen(id) : 0;
  if (n < 1 || n > AUTH_ID_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    char c = id[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
  }
  return true;
}

KeyAdd authKeyAdd(const char* id, Role role, const uint8_t pub[65]) {
  if (!validId(id)) return KeyAdd::BadId;
  if (role != Role::Control && role != Role::Admin) return KeyAdd::BadRole;
  if (!pointOnCurve(pub)) return KeyAdd::BadKey;
  Lock l;
  int slot = -1;
  for (int i = 0; i < AUTH_MAX_KEYS; i++) {
    if (keys[i].used && strcmp(keys[i].id, id) == 0) { slot = i; break; }   // replace same id
  }
  for (int i = 0; slot < 0 && i < AUTH_MAX_KEYS; i++) {
    if (!keys[i].used) slot = i;
  }
  if (slot < 0) return KeyAdd::Full;
  memset(&keys[slot], 0, sizeof(KeyRecord));
  strncpy(keys[slot].id, id, AUTH_ID_MAX);
  keys[slot].role = (uint8_t)role;
  keys[slot].used = 1;
  memcpy(keys[slot].pub, pub, 65);
  saveKeys();
  return KeyAdd::Ok;
}

bool authKeyRemove(const char* id) {
  Lock l;
  for (int i = 0; i < AUTH_MAX_KEYS; i++) {
    if (keys[i].used && strcmp(keys[i].id, id) == 0) {
      memset(&keys[i], 0, sizeof(KeyRecord));
      saveKeys();
      return true;
    }
  }
  return false;
}

void authKeyWipe() {
  Lock l;
  memset(keys, 0, sizeof(keys));
  saveKeys();
}

int authKeyCount() {
  int n = 0;
  for (const KeyRecord& k : keys) n += k.used ? 1 : 0;
  return n;
}

bool authKeyAt(int index, KeyInfo& out) {
  Lock l;
  int seen = 0;
  for (const KeyRecord& k : keys) {
    if (!k.used) continue;
    if (seen++ == index) {
      memcpy(out.id, k.id, sizeof(out.id));
      out.role = (Role)k.role;
      memcpy(out.pub, k.pub, 65);
      return true;
    }
  }
  return false;
}

bool authParsePublicKey(const char* b64, uint8_t pub[65]) {
  size_t outLen = 0;
  uint8_t tmp[80];
  if (mbedtls_base64_decode(tmp, sizeof(tmp), &outLen, (const unsigned char*)b64, strlen(b64)) != 0) return false;
  if (outLen != 65 || !pointOnCurve(tmp)) return false;
  memcpy(pub, tmp, 65);
  return true;
}

// --- per-address limits (call with the lock held) ---------------------------------------
static IpState& ipState(uint32_t ip) {
  int oldest = 0;
  for (int i = 0; i < IP_SLOTS; i++) {
    if (ips[i].used && ips[i].ip == ip) return ips[i];
    if (!ips[i].used) { oldest = i; break; }
    if (ips[i].failWindowStart < ips[oldest].failWindowStart) oldest = i;
  }
  memset(&ips[oldest], 0, sizeof(IpState));
  ips[oldest].used = true;
  ips[oldest].ip = ip;
  return ips[oldest];
}

static bool blocked(IpState& st) {
  return st.blockedUntil > nowMs();
}

static void recordFailure(IpState& st) {
  uint64_t now = nowMs();
  if (now - st.failWindowStart > FAIL_WINDOW_MS) {
    st.failWindowStart = now;
    st.fails = 0;
  }
  if (++st.fails >= FAIL_LIMIT) {
    st.blockedUntil = now + BLOCK_MS;
    st.fails = 0;
  }
}

static void addLog(uint32_t ip, AuthResult r, const char* keyId) {
  LogEntry& e = logRing[logCount % LOG_SIZE];
  e.ms = nowMs();
  e.ip = ip;
  e.result = r;
  strncpy(e.keyId, keyId ? keyId : "", AUTH_ID_MAX);
  e.keyId[AUTH_ID_MAX] = '\0';
  logCount++;
}

// --- nonces -------------------------------------------------------------------------
bool authNewNonce(uint32_t remoteIp, char out[33]) {
  Lock l;
  IpState& st = ipState(remoteIp);
  uint64_t now = nowMs();
  if (now - st.challengeWindowStart > FAIL_WINDOW_MS) {
    st.challengeWindowStart = now;
    st.challenges = 0;
  }
  if (blocked(st) || st.challenges >= CHALLENGE_LIMIT) {
    addLog(remoteIp, AuthResult::RateLimited, "");
    return false;
  }
  st.challenges++;

  int slot = 0;
  for (int i = 0; i < NONCE_SLOTS; i++) {
    bool expired = nonces[i].used && now - nonces[i].issuedMs > NONCE_TTL_MS;
    if (!nonces[i].used || expired) { slot = i; break; }
    if (nonces[i].issuedMs < nonces[slot].issuedMs) slot = i;   // otherwise replace the oldest
  }
  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  toHex(raw, sizeof(raw), nonces[slot].hex);
  nonces[slot].used = true;
  nonces[slot].issuedMs = now;
  memcpy(out, nonces[slot].hex, 33);
  return true;
}

// Removes the nonce whatever happens next (a nonce is good for one attempt only).
// Returns its age, or -1 if it is unknown or expired.
static int64_t consumeNonce(const char* hex) {
  uint64_t now = nowMs();
  for (NonceSlot& n : nonces) {
    if (n.used && strcmp(n.hex, hex) == 0) {
      n.used = false;
      uint64_t age = now - n.issuedMs;
      return age <= NONCE_TTL_MS ? (int64_t)age : -1;
    }
  }
  return -1;
}

// --- verification -----------------------------------------------------------------------
AuthOutcome authVerify(const AuthRequest& req, Role needed) {
  AuthOutcome out{};
  out.result = AuthResult::BadRequest;
  out.role = Role::Control;
  strncpy(out.keyId, req.keyId ? req.keyId : "", AUTH_ID_MAX);

  Lock l;
  IpState& st = ipState(req.remoteIp);
  auto finish = [&](AuthResult r) {
    out.result = r;
    if (r == AuthResult::Ok) {
      st.fails = 0;
    } else if (r != AuthResult::RateLimited) {
      recordFailure(st);
    }
    addLog(req.remoteIp, r, out.keyId);
    return out;
  };

  if (blocked(st)) return finish(AuthResult::RateLimited);
  if (!req.keyId || !req.nonce || !req.method || !req.path || !req.sigB64 || strlen(req.nonce) != 32) {
    return finish(AuthResult::BadRequest);
  }

  const KeyRecord* key = nullptr;
  for (const KeyRecord& k : keys) {
    if (k.used && strcmp(k.id, req.keyId) == 0) key = &k;
  }
  // Consume the nonce even for unknown keys, so guessing key ids cannot be done for free.
  int64_t age = consumeNonce(req.nonce);
  if (!key) return finish(AuthResult::UnknownKey);
  if (age < 0) return finish(AuthResult::BadNonce);
  out.nonceAgeMs = (uint32_t)age;

  uint8_t sig[64];
  size_t sigLen = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen, (const unsigned char*)req.sigB64, strlen(req.sigB64)) != 0 ||
      sigLen != 64) {
    return finish(AuthResult::BadRequest);
  }
  uint8_t digest[32];
  if (!signedDigest(req.nonce, req.keyId, req.method, req.path, req.body, req.bodyLen, digest)) {
    return finish(AuthResult::BadRequest);
  }
  if (!verifyP256(key->pub, digest, sig)) return finish(AuthResult::BadSignature);

  out.role = (Role)key->role;
  if ((uint8_t)out.role < (uint8_t)needed) return finish(AuthResult::Forbidden);
  return finish(AuthResult::Ok);
}

const char* authResultText(AuthResult r) {
  switch (r) {
    case AuthResult::Ok:           return "ok";
    case AuthResult::RateLimited:  return "rate limited";
    case AuthResult::UnknownKey:   return "unknown key";
    case AuthResult::BadNonce:     return "bad or expired nonce";
    case AuthResult::BadSignature: return "bad signature";
    case AuthResult::Forbidden:    return "role not allowed";
    case AuthResult::BadRequest:   return "malformed request";
  }
  return "?";
}

void authPrintLog() {
  Lock l;
  int count = logCount < LOG_SIZE ? logCount : LOG_SIZE;
  if (count == 0) {
    Log(INFO, "Auth log is empty");
    return;
  }
  uint64_t now = nowMs();
  for (int i = count; i >= 1; i--) {   // newest first
    const LogEntry& e = logRing[(logCount - i) % LOG_SIZE];
    Logf(INFO, "%6lus ago  %u.%u.%u.%u  key '%s'  %s", (unsigned long)((now - e.ms) / 1000),
         (e.ip >> 24) & 255, (e.ip >> 16) & 255, (e.ip >> 8) & 255, e.ip & 255, e.keyId,
         authResultText(e.result));
  }
}

bool authLogAt(int index, AuthLogItem& out) {
  Lock l;
  int count = logCount < LOG_SIZE ? logCount : LOG_SIZE;
  if (index < 0 || index >= count) return false;
  const LogEntry& e = logRing[(logCount - 1 - index) % LOG_SIZE];
  out.ageSeconds = (uint32_t)((nowMs() - e.ms) / 1000);
  out.ip = e.ip;
  out.result = e.result;
  strcpy(out.keyId, e.keyId);
  return true;
}

// --- self test --------------------------------------------------------------------------
static int testsFailed = 0;
static void check(const char* name, bool pass) {
  Logf(pass ? INFO : ERROR, "  %s: %s", pass ? "PASS" : "FAIL", name);
  if (!pass) testsFailed++;
}

static void installTestNonce(const char* hex, uint64_t issuedMs) {
  Lock l;
  nonces[0].used = true;
  strncpy(nonces[0].hex, hex, 32);
  nonces[0].hex[32] = '\0';
  nonces[0].issuedMs = issuedMs;
}

bool authSelfTest() {
  testsFailed = 0;
  Log(INFO, "Auth self-test (vector signed by OpenSSL)");

  const size_t bodyLen = strlen(VEC_BODY);
  uint8_t digest[32];
  bool built = signedDigest(VEC_NONCE, VEC_KEY_ID, VEC_METHOD, VEC_PATH, (const uint8_t*)VEC_BODY, bodyLen, digest);
  check("signed string builds", built);
  check("valid signature verifies", built && verifyP256(VEC_PUB, digest, VEC_SIG));

  uint8_t bad[32];
  signedDigest(VEC_NONCE, VEC_KEY_ID, VEC_METHOD, VEC_PATH, (const uint8_t*)"{\"seat\":\"driver\",\"level\":\"low\"}", 30, bad);
  check("changed body is rejected", !verifyP256(VEC_PUB, bad, VEC_SIG));
  signedDigest(VEC_NONCE, VEC_KEY_ID, "GET", VEC_PATH, (const uint8_t*)VEC_BODY, bodyLen, bad);
  check("changed method is rejected", !verifyP256(VEC_PUB, bad, VEC_SIG));
  signedDigest(VEC_NONCE, VEC_KEY_ID, VEC_METHOD, "/api/v1/power", (const uint8_t*)VEC_BODY, bodyLen, bad);
  check("changed path is rejected", !verifyP256(VEC_PUB, bad, VEC_SIG));
  uint8_t flipped[64];
  memcpy(flipped, VEC_SIG, 64);
  flipped[10] ^= 0x01;
  check("flipped signature bit is rejected", !verifyP256(VEC_PUB, digest, flipped));
  check("another key is rejected", !verifyP256(VEC_OTHER_PUB, digest, VEC_SIG));
  check("public key point is validated", pointOnCurve(VEC_PUB) && !pointOnCurve(digest /* garbage */));

  // End to end through authVerify() with a temporary in-RAM key (never written to flash).
  int slot = -1;
  {
    Lock l;
    for (int i = AUTH_MAX_KEYS - 1; i >= 0; i--) {
      if (!keys[i].used) { slot = i; break; }
    }
    if (slot >= 0) {
      memset(&keys[slot], 0, sizeof(KeyRecord));
      strcpy(keys[slot].id, VEC_KEY_ID);
      keys[slot].role = (uint8_t)Role::Control;
      keys[slot].used = 1;
      memcpy(keys[slot].pub, VEC_PUB, 65);
    }
  }
  if (slot < 0) {
    Log(WARN, "  skipped end-to-end tests: key store is full");
  } else {
    char sigB64[96];
    size_t n = 0;
    mbedtls_base64_encode((unsigned char*)sigB64, sizeof(sigB64), &n, VEC_SIG, 64);
    sigB64[n] = '\0';
    AuthRequest req{VEC_KEY_ID, VEC_NONCE, VEC_METHOD, VEC_PATH, (const uint8_t*)VEC_BODY, bodyLen, sigB64, 0xFFFFFFF0};

    installTestNonce(VEC_NONCE, nowMs());
    AuthOutcome o = authVerify(req, Role::Control);
    check("end to end: accepted", o.result == AuthResult::Ok);
    o = authVerify(req, Role::Control);
    check("end to end: replay of the same nonce is rejected", o.result == AuthResult::BadNonce);

    installTestNonce(VEC_NONCE, nowMs() - NONCE_TTL_MS - 1000);
    o = authVerify(req, Role::Control);
    check("end to end: expired nonce is rejected", o.result == AuthResult::BadNonce);

    installTestNonce(VEC_NONCE, nowMs());
    o = authVerify(req, Role::Admin);
    check("end to end: control key cannot use an admin endpoint", o.result == AuthResult::Forbidden);

    AuthRequest other = req;
    other.keyId = "nobody";
    installTestNonce(VEC_NONCE, nowMs());
    o = authVerify(other, Role::Control);
    check("end to end: unknown key is rejected", o.result == AuthResult::UnknownKey);

    // Failures from one address eventually block that address (use a private test address).
    AuthRequest spam = req;
    spam.remoteIp = 0xFFFFFFF1;
    spam.body = (const uint8_t*)"x";
    spam.bodyLen = 1;
    AuthResult last = AuthResult::Ok;
    for (int i = 0; i < FAIL_LIMIT + 1; i++) {
      installTestNonce(VEC_NONCE, nowMs());
      last = authVerify(spam, Role::Control).result;
    }
    check("end to end: repeated failures get rate limited", last == AuthResult::RateLimited);

    Lock l;
    memset(&keys[slot], 0, sizeof(KeyRecord));
    for (IpState& s : ips) {
      if (s.used && (s.ip == 0xFFFFFFF0 || s.ip == 0xFFFFFFF1)) memset(&s, 0, sizeof(IpState));
    }
    nonces[0].used = false;
  }

  Logf(testsFailed ? ERROR : INFO, "Auth self-test: %s", testsFailed ? "FAILED" : "all checks passed");
  return testsFailed == 0;
}
