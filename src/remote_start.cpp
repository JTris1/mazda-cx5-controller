#include <Arduino.h>
#include "remote_start.h"
#include "config.h"
#include "engine.h"
#include "log.h"

void remoteStartBegin() {
  // Safe state first: the trigger output must be LOW before anything else runs.
  pinMode(RS_TRIGGER_PIN, OUTPUT);
  digitalWrite(RS_TRIGGER_PIN, LOW);
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
