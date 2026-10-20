#pragma once
#include <Arduino.h>

// Engine state (updated by pollEngine)
extern volatile bool engineRunning;
extern uint32_t engineChangedAt;  // when engineRunning last changed (millis)

// Sets up the input and takes the first reading without debouncing or logging.
void engineBegin();

// Raw, undebounced reading (true = GWR is pulling the wire low = engine running).
bool readEngineRaw();

// Debounces the input and logs each change. Runs inside the engine task.
void pollEngine();

// Starts the task that calls pollEngine() every 10 ms, so the engine state stays current
// even while the main loop or an action is busy. Call after Serial.begin().
void engineStartTask();
