#pragma once
#include <stdint.h>

// Pulse timing (tunable at runtime from the serial console, persisted in NVS).
constexpr uint16_t DEFAULT_PRESS_MS = 80;    // how long the "switch" is held closed
constexpr uint16_t DEFAULT_GAP_MS   = 150;   // pause between repeated presses
constexpr uint16_t MIN_PRESS_MS     = 30;
constexpr uint16_t MAX_PRESS_MS     = 500;   // hard ceiling: an output is never held longer than this
constexpr uint16_t MIN_GAP_MS       = 50;
constexpr uint16_t MAX_GAP_MS       = 2000;

// Safety caps on a single press command.
constexpr uint8_t MAX_TEMP_STEPS     = 60;   // dial clamps at LO/HI, so over-pressing is harmless
constexpr uint8_t MAX_SEAT_PRESSES   = 4;
constexpr uint8_t MAX_DEFROST_PRESSES = 2;   // defrost buttons toggle; no state feedback yet

// Absolute temperature. UNMEASURED defaults, calibrate on the car with `tempcal`.
// Position from the LO clamp = loOffset + round((F - minF) * steps / (maxF - minF)).
constexpr int16_t  DEFAULT_TEMP_MIN_F    = 60;
constexpr int16_t  DEFAULT_TEMP_MAX_F    = 84;
constexpr uint8_t  DEFAULT_TEMP_STEPS    = 24;   // presses from minF to maxF
constexpr uint8_t  DEFAULT_TEMP_LO_OFFSET = 0;   // presses from the LO clamp to minF
constexpr uint8_t  TEMP_HOME_MARGIN      = 4;    // extra down presses to be sure we're clamped
constexpr uint16_t TEMP_SETTLE_MS        = 400;  // pause between homing and counting up

// Button debounce.
constexpr uint32_t DEBOUNCE_MS = 30;

// 12 V divider: Vbat -- R1 --+-- R2 -- GND, ADC reads the middle node. Add 100 nF at the pin.
constexpr float VBAT_R1_OHMS = 220000.0f;
constexpr float VBAT_R2_OHMS = 47000.0f;
