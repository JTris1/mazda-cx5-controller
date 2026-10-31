#include "engine.h"
#include "config.h"
#include "log.h"
#include "vehicle_state.h"

volatile bool engineRunning = false;
static bool engineCandidate = false;       // most recent raw reading
static uint32_t engineCandidateSince = 0;  // when the raw reading last changed
uint32_t engineChangedAt = 0;

// GWR pulls the wire to ground while running, so LOW means running.
bool readEngineRaw() {
  return digitalRead(ENGINE_RUNNING_PIN) == LOW;
}

void engineBegin() {
  // Floats high via the pull-up, the module grounds it while running.
  pinMode(ENGINE_RUNNING_PIN, INPUT_PULLUP);
  engineRunning = engineCandidate = readEngineRaw();
}

void pollEngine() {
  bool raw = readEngineRaw();
  uint32_t now = millis();
  if (raw != engineCandidate) {
    engineCandidate = raw;
    engineCandidateSince = now;
  }
  if (engineCandidate != engineRunning && now - engineCandidateSince >= ENGINE_DEBOUNCE_MS) {
    engineRunning = engineCandidate;
    engineChangedAt = now;
    Log(INFO, engineRunning ? "Engine running" : "Engine stopped");
    if (!engineRunning) vstateEngineStopped();
  }
}

static void engineTask(void*) {
  for (;;) {
    pollEngine();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void engineStartTask() {
  xTaskCreatePinnedToCore(engineTask, "engine", 3072, nullptr, 1, nullptr, 0);
}
