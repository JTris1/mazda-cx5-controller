#pragma once
#include <Arduino.h>

// Over-the-air firmware update, streamed into the inactive app slot.
//
//   1. otaStart()        an admin (signed request) declares size, SHA-256 and version; gets a one-time token
//   2. otaWriteBegin()   the upload request presents the token (single use, 10 minutes)
//   3. otaWriteChunk()   data is written to flash while the SHA-256 is computed
//   4. otaWriteFinish()  the hash must match the declared one and the image must pass the IDF checks, only
//                        then is the new slot made the boot slot. A reboot (separate call) starts it.
//
// Rollback: the new image boots "pending verify". otaPoll() confirms it only after cellular, the tunnel and
// the API have been up together for OTA_CONFIRM_MS. If it crashes or restarts before that, the bootloader
// returns to the previous image by itself.

enum class OtaState : uint8_t { Idle, Armed, Receiving, Ready };

struct OtaInfo {
  OtaState state;
  uint32_t received;
  uint32_t size;
  char version[32];        // declared version of the upload in progress or waiting for a reboot
  bool pendingVerify;      // the running image has not been confirmed yet (rollback still possible)
  uint32_t confirmInSeconds;   // when pendingVerify: seconds of healthy running still needed
};

constexpr uint32_t OTA_CONFIRM_MS = 180000;
// A new image that has not become healthy this long after booting is abandoned for the previous one.
constexpr uint32_t OTA_GIVE_UP_MS = 20 * 60 * 1000;
constexpr uint32_t OTA_TOKEN_MS = 600000;
constexpr uint32_t OTA_IDLE_ABORT_MS = 30000;   // an upload that stops sending for this long is abandoned

// False with a short reason in `error` when an update cannot start now.
bool otaStart(uint32_t size, const char* sha256Hex, const char* version, char token[33], const char** error);
bool otaWriteBegin(const char* token, const char** error);
bool otaWriteChunk(const uint8_t* data, size_t len, const char** error);
bool otaWriteFinish(const char** error);
void otaAbort();

bool otaReceiving();          // an upload is in progress: commands are refused meanwhile
void otaInfo(OtaInfo& out);
void otaPoll();               // call every loop
