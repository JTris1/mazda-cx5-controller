#pragma once
#include <Arduino.h>

// GPIO assignments for the LilyGO T-SIM7670G-S3 *Standard* board.
//
// Already used on this board (LilyGo-Modem-Series utilities.h, *_S3_STAN block):
//   modem UART TX 4 / RX 5, RI 6, DTR 7, PWRKEY 46        <-- do NOT reuse
//   battery ADC 8, solar ADC 18, power-save-mode 42
//   I2C 2/3, SD card 10-13, GPS 0/1/17/45/48
//   33-37 are octal PSRAM, 19/20 are USB
// Also named in LilyGO's header for the optional camera / SimShield add-ons:
//   9, 14, 15, 16, 21, 38, 39, 40, 41, 47. Free as long as those add-ons aren't fitted.
namespace pins {

// --- I2C bus to the MCP23017 output expander (the board's own I2C pins).
//     MCP23017: A0/A1/A2 -> GND (address 0x20), RESET -> 3V3, VDD -> 3V3.
constexpr uint8_t I2C_SDA = 3;
constexpr uint8_t I2C_SCL = 2;
constexpr uint8_t MCP_ADDR = 0x20;

// --- MCP23017 pins driving the PC817 LED side (pin -> 220R -> LED anode, cathode -> GND).
//     Add a 10k pulldown pin->GND on each so an opto stays off while the expander resets.
//     0-7 = GPA0-7, 8-15 = GPB0-7.
constexpr uint8_t MCP_TEMP_UP       = 0;   // GPA0
constexpr uint8_t MCP_TEMP_DOWN     = 1;   // GPA1
constexpr uint8_t MCP_SEAT_DRIVER   = 2;   // GPA2
constexpr uint8_t MCP_SEAT_PASS     = 3;   // GPA3
constexpr uint8_t MCP_FRONT_DEFROST = 4;   // GPA4
constexpr uint8_t MCP_REAR_DEFROST  = 5;   // GPA5
// constexpr uint8_t MCP_REMOTE_START = 6; // GPA6, phase 3: iDatastart M3-06 (-) trigger
// GPA7 and GPB0-7 are spare.

// --- Bench-test buttons: one side to GPIO, other side to GND (internal pull-up used).
constexpr uint8_t BTN_TEMP_UP       = 15;
constexpr uint8_t BTN_TEMP_DOWN     = 16;
constexpr uint8_t BTN_SEAT_DRIVER   = 14;
constexpr uint8_t BTN_SEAT_PASS     = 21;
constexpr uint8_t BTN_FRONT_DEFROST = 38;
constexpr uint8_t BTN_REAR_DEFROST  = 39;

// --- 12 V sense (resistor divider, see README). Must be an ADC1 pin (GPIO 1-10).
constexpr uint8_t VBAT_SENSE = 9;

// --- Still free natives: 40, 41, 47.
// Phase 3 inputs from the iDatastart module (active LOW, see docs/design-notes.md):
//   RS_RUNNING_N  <- M2-06 "ground while running"
//   IGN_ON_N      <- a POC set to "GND with vehicle ignition"

}  // namespace pins
