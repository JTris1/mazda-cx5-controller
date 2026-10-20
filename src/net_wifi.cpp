#include <WiFi.h>
#include "net_wifi.h"
#include "log.h"
#include "net_time.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing src/secrets.h: copy src/secrets.example.h to src/secrets.h and fill it in."
#endif

static constexpr uint32_t RETRY_MIN_MS = 5000;
static constexpr uint32_t RETRY_MAX_MS = 60000;
static constexpr uint32_t ATTEMPT_TIMEOUT_MS = 20000;  // give up on one attempt after this

static bool wasConnected = false;
static bool attemptActive = false;
static uint32_t attemptStartedAt = 0;
static uint32_t nextAttemptAt = 0;
static uint32_t retryDelayMs = RETRY_MIN_MS;

static void startAttempt() {
  Logf(INFO, "Wi-Fi: connecting to '%s'...", WIFI_SSID);
  WiFi.disconnect(false);   // clear any half-open attempt, keep the radio on
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  attemptActive = true;
  attemptStartedAt = millis();
}

void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);       // do not write credentials to flash
  WiFi.setAutoReconnect(false); // we manage retries (backoff) ourselves
  WiFi.setHostname("cx5-controller");
  startAttempt();
}

void wifiPoll() {
  bool up = (WiFi.status() == WL_CONNECTED);
  uint32_t now = millis();

  if (up && !wasConnected) {
    wasConnected = true;
    attemptActive = false;
    retryDelayMs = RETRY_MIN_MS;
    Logf(INFO, "Wi-Fi: connected, IP %s, RSSI %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (!up && wasConnected) {
    wasConnected = false;
    Log(WARN, "Wi-Fi: connection lost");
    nextAttemptAt = now + retryDelayMs;
  } else if (!up && attemptActive && now - attemptStartedAt > ATTEMPT_TIMEOUT_MS) {
    // The attempt timed out; wait with a growing delay before the next one.
    attemptActive = false;
    nextAttemptAt = now + retryDelayMs;
    Logf(WARN, "Wi-Fi: attempt failed, retrying in %lu s", (unsigned long)(retryDelayMs / 1000));
    retryDelayMs = min(retryDelayMs * 2, RETRY_MAX_MS);
  } else if (!up && !attemptActive && (int32_t)(now - nextAttemptAt) >= 0) {
    startAttempt();
  }

  timePoll();
}

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

void wifiPrintStatus() {
  if (!wifiConnected()) {
    Logf(INFO, "Wi-Fi: not connected (target '%s')", WIFI_SSID);
  } else {
    Logf(INFO, "Wi-Fi: '%s' IP %s RSSI %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Logf(INFO, "Gateway %s, DNS %s", WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
  }
  Logf(INFO, "Time: %s", timeNowString().c_str());
}
