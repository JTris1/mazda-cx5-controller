#pragma once
#include <Arduino.h>

// Engine state (updated by pollEngine)
extern bool engineRunning;
extern uint32_t engineChangedAt;  // when engineRunning last changed (millis)

// Sets up the input and takes the first reading without debouncing or logging.
void engineBegin();

// Raw, undebounced reading (true = GWR is pulling the wire low = engine running).
bool readEngineRaw();

// Call every loop. Debounces the input and logs each change.
void pollEngine();
