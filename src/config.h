#pragma once
#include <Arduino.h>

// --- Pins (native ESP32-S3 GPIO) -------------------------------------------
constexpr int PIN_I2C_SDA = 8;
constexpr int PIN_I2C_SCL = 9;

// Remote-start trigger: drives a PC817 that pulls the iDatastart trigger wire to
// ground. Native GPIO on purpose, not the expander, so it stays independent of I2C.
// TODO: confirm this pin is free and broken out on your board.
constexpr int RS_TRIGGER_PIN = 14;

// Engine-running input: the iDatastart GWR output (M2-06 BLUE/WHITE) grounds this wire
// while the engine runs and leaves it floating otherwise, so we use the internal
// pull-up and read LOW as "running". Add a ~1k series resistor in the wiring.
// TODO: confirm this pin is free and broken out on your board.
constexpr int ENGINE_RUNNING_PIN = 21;

// --- MCP23017 output channels (expander pin numbers 0-15) ------------------
// Library pins 0-7 are bank A (GPA0-7), 8-15 are bank B (GPB0-7).
constexpr uint8_t MCP_ADDR = 0x27;

constexpr uint8_t TEMP_UP_PIN = 0;                // GPA0
constexpr uint8_t TEMP_DOWN_PIN = 1;              // GPA1
constexpr uint8_t DRIVER_SEAT_HEATER_PIN = 2;     // GPA2
constexpr uint8_t PASSENGER_SEAT_HEATER_PIN = 3;  // GPA3
constexpr uint8_t FRONT_DEFROST_PIN = 4;          // GPA4
constexpr uint8_t CLIMATE_POWER_PIN = 8;          // GPB0
constexpr uint8_t REAR_DEFROST_PIN = 9;           // GPB1
constexpr uint8_t FAN_UP_PIN = 10;                // GPB2
constexpr uint8_t FAN_DOWN_PIN = 11;              // GPB3

// --- Limits -----------------------------------------------------------------
constexpr uint32_t HOLD_MS_MAX = 500;  // fail-safe: an output is never held longer
constexpr uint32_t RS_PULSE_MS_MAX = 2000;
constexpr uint32_t ENGINE_DEBOUNCE_MS = 200;  // engine input must be stable this long to count
constexpr uint32_t BENCH_MAX_S = 30;

// --- Runtime settings (changeable from the serial console) ------------------
struct Settings {
  uint32_t holdMs = 150;      // how long a button is "held"
  uint32_t gapMs = 150;       // pause between presses
  int tempMinF = 60;          // dial value when clamped at LO (unmeasured default)
  int tempMaxF = 84;          // dial value when clamped at HI (unmeasured default)
  int clampExtra = 2;         // extra down presses beyond the range, to be sure of LO
  uint32_t rsPulseMs = 300;   // remote-start trigger pulse width (module spec unknown, tune it)
  uint32_t rsGapMs = 300;     // gap between pulses when sending more than one
  int rsPulses = 2;           // 2 = the module's "double pulse" setting (1-3)
  uint32_t rsConfirmMs = 20000;  // how long to wait for the engine to report running
  int seatHeaterStages = 3;   // number of stages on the seat heater button (ex: 3 means the button can be pressed 3 times before looping around to 0)
};
extern Settings cfg;
