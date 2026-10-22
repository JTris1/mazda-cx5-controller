#pragma once
#include <Arduino.h>

// Per-device signed-request authentication (see the plan: ECDSA P-256 over a one-time nonce).
//
// A client signs this exact string (UTF-8) with its private key:
//   "CX5-V1\n" + nonce + "\n" + key_id + "\n" + METHOD + "\n" + path + "\n" + hex(sha256(body))
// using ECDSA P-256 with SHA-256, and sends the signature as base64 of the raw 64-byte r||s
// (the WebCrypto format). Public keys are enrolled as base64 of the 65-byte uncompressed
// point 04||X||Y (what WebCrypto's exportKey("raw") returns).

enum class Role : uint8_t { Control = 1, Admin = 2 };  // Admin includes Control

constexpr int AUTH_MAX_KEYS = 8;
constexpr size_t AUTH_ID_MAX = 15;

struct KeyInfo {
  char id[AUTH_ID_MAX + 1];
  Role role;
  uint8_t pub[65];
};

enum class AuthResult : uint8_t { Ok, RateLimited, UnknownKey, BadNonce, BadSignature, Forbidden, BadRequest };

struct AuthRequest {
  const char* keyId;
  const char* nonce;      // 32 hex chars, from authNewNonce()
  const char* method;     // "GET", "POST", ...
  const char* path;       // request path without query
  const uint8_t* body;    // raw request body (may be empty)
  size_t bodyLen;
  const char* sigB64;     // base64 of 64-byte r||s
  uint32_t remoteIp;      // IPv4 as a number, for per-address rate limiting (0 = local)
};

struct AuthOutcome {
  AuthResult result;
  Role role;              // the key's role when result == Ok
  char keyId[AUTH_ID_MAX + 1];
  uint32_t nonceAgeMs;    // age of the nonce when it was used (for the stricter start policy)
};

// Loads enrolled keys from flash. Call after Serial.begin().
void authBegin();

// --- Key store (changed from the serial console and, later, by admin API calls) ---------
enum class KeyAdd : uint8_t { Ok, BadId, BadRole, BadKey, Full };
KeyAdd authKeyAdd(const char* id, Role role, const uint8_t pub[65]);
bool authKeyRemove(const char* id);
void authKeyWipe();
int authKeyCount();
bool authKeyAt(int index, KeyInfo& out);
bool authParsePublicKey(const char* b64, uint8_t pub[65]);  // also checks the point is on the curve
const char* roleName(Role r);

// Short identifier for a public key: the first 8 bytes of its SHA-256 as hex (17 chars with NUL).
void authFingerprint(const uint8_t pub[65], char out[17]);

// --- Challenge and verification ------------------------------------------------------
// New one-time nonce (32 hex chars + NUL). False if this address asks for too many.
bool authNewNonce(uint32_t remoteIp, char out[33]);

// Checks the key, consumes the nonce, verifies the signature and role. Every outcome is
// logged; callers should answer any non-Ok result with a generic 401/403/429.
AuthOutcome authVerify(const AuthRequest& req, Role needed);

const char* authResultText(AuthResult r);
void authPrintLog();

// Runs the built-in tests (OpenSSL-made vector, nonce reuse and expiry, rate limit,
// roles). Prints one line per check and returns true only if all pass.
bool authSelfTest();
