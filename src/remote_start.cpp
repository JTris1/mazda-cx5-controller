#include <Arduino.h>
#include <Preferences.h>
#include <esp_timer.h>
#include "remote_start.h"
#include "config.h"
#include "engine.h"
#include "log.h"

void remoteStartBegin() {
  // Safe state first: the trigger output must be LOW before anything else runs.
  pinMode(RS_TRIGGER_PIN, OUTPUT);
  digitalWrite(RS_TRIGGER_PIN, LOW);
}

// --- policy for network-requested starts -----------------------------------------------
static constexpr uint32_t COOLDOWN_MS = 60UL * 1000;
static constexpr uint32_t HOUR_MS = 3600UL * 1000;
static constexpr uint32_t DAY_MS = 24UL * HOUR_MS;
static constexpr int HOURLY_LIMIT = 3;
static constexpr int DAILY_LIMIT = 10;
static uint64_t startTimes[DAILY_LIMIT];
static int startCount = 0;

static uint64_t nowMs() { return (uint64_t)esp_timer_get_time() / 1000ULL; }

bool remoteStartEnabled() {
  Preferences p;
  p.begin("rs", false);   // read-write so the namespace is created on first use (read-only logs an error until it exists)
  bool on = p.getBool("enabled", false);
  p.end();
  return on;
}

void setRemoteStartEnabled(bool on) {
  Preferences p;
  p.begin("rs", false);
  p.putBool("enabled", on);
  p.end();
}

bool remoteStartPolicyAllow(const char** reason) {
  if (!remoteStartEnabled()) {
    *reason = "remote start is disabled";
    return false;
  }
  uint64_t now = nowMs();
  int hourly = 0;
  uint64_t last = 0;
  int kept = 0;
  for (int i = 0; i < startCount; i++) {   // drop entries older than a day while counting
    if (now - startTimes[i] > DAY_MS) continue;
    startTimes[kept++] = startTimes[i];
    if (now - startTimes[i] <= HOUR_MS) hourly++;
    if (startTimes[i] > last) last = startTimes[i];
  }
  startCount = kept;
  if (startCount > 0 && now - last < COOLDOWN_MS) {
    *reason = "cooldown: wait 60 s between starts";
    return false;
  }
  if (hourly >= HOURLY_LIMIT) {
    *reason = "hourly limit reached";
    return false;
  }
  if (startCount >= DAILY_LIMIT) {
    *reason = "daily limit reached";
    return false;
  }
  startTimes[startCount++] = now;
  return true;
}

// HIGH turns the optocoupler on, which pulls the module's trigger wire to ground.
bool sendRemoteStart() {
  // The module may treat another pulse as a stop request, so never pulse a running engine.
  if (engineRunning) {
    Log(WARN, "Engine is already running, not sending a start pulse");
    return true;
  }

  Logf(INFO, "Remote start: %d pulse(s), %lu ms on, %lu ms gap", cfg.rsPulses,
       (unsigned long)cfg.rsPulseMs, (unsigned long)cfg.rsGapMs);
  for (int i = 0; i < cfg.rsPulses; i++) {
    digitalWrite(RS_TRIGGER_PIN, HIGH);
    delay(cfg.rsPulseMs);
    digitalWrite(RS_TRIGGER_PIN, LOW);
    if (i < cfg.rsPulses - 1) delay(cfg.rsGapMs);
  }
  Logf(INFO, "Trigger sent, waiting up to %lu s for the engine...",
       (unsigned long)(cfg.rsConfirmMs / 1000));

  // Wait for the debounced engine input to go true, or give up.
  uint32_t startedAt = millis();
  while (millis() - startedAt < cfg.rsConfirmMs) {
    if (engineRunning) {  // kept current by the engine task
      Logf(INFO, "Remote start CONFIRMED after %lu ms", (unsigned long)(millis() - startedAt));
      return true;
    }
    delay(10);
  }
  Logf(ERROR, "Remote start NOT confirmed within %lu s. Check the module's parking-light error code.",
       (unsigned long)(cfg.rsConfirmMs / 1000));
  return false;
}
