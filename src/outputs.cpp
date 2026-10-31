#include <Arduino.h>
#include <Wire.h>
#include "outputs.h"
#include "config.h"
#include "log.h"

Adafruit_MCP23X17 mcp;
bool expanderOk = false;

void outputsBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  expanderOk = mcp.begin_I2C(MCP_ADDR, &Wire);

  if (!expanderOk) {
    Log(ERROR, "MCP23017 not found. Outputs have been disabled.");
    return;
  }

  // Outputs start LOW so no optocoupler is ever driven at boot.
  for (uint8_t pin : {
    TEMP_UP_PIN,
    TEMP_DOWN_PIN,
    DRIVER_SEAT_HEATER_PIN,
    PASSENGER_SEAT_HEATER_PIN,
    FRONT_DEFROST_PIN,
    REAR_DEFROST_PIN,
    FAN_UP_PIN,
    FAN_DOWN_PIN
  }) {
    mcp.pinMode(pin, OUTPUT);
    mcp.digitalWrite(pin, LOW);
  }
  if (CLIMATE_POWER_ENABLED) {
    mcp.pinMode(CLIMATE_POWER_PIN, OUTPUT);
    mcp.digitalWrite(CLIMATE_POWER_PIN, LOW);
  } else {
    Log(INFO, "Climate power button is disabled (CLIMATE_POWER_ENABLED); its pin is left undriven.");
  }
  Log(INFO, "MCP23017 intialized.");
}

void pressButton(uint8_t pin) {
  mcp.digitalWrite(pin, HIGH);
  delay(cfg.holdMs);
  mcp.digitalWrite(pin, LOW);
  delay(cfg.gapMs);
}

void pressButton(uint8_t pin, int times) {
  for (int i = 0; i < times; i++) pressButton(pin);
}

// --- read-only diagnostics ------------------------------------------------------------
// MCP23017 registers (BANK=0, the power-on default).
static bool readReg(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(MCP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MCP_ADDR, (uint8_t)1) != 1) return false;
  value = Wire.read();
  return true;
}

void outputsPrintStatus() {
  Logf(INFO, "I2C: SDA GPIO %d, SCL GPIO %d, expander address 0x%02X", PIN_I2C_SDA, PIN_I2C_SCL, MCP_ADDR);
  static const char* names[] = {"Direction A", "Direction B", "Latch A", "Latch B", "Pins A", "Pins B"};
  static const uint8_t regs[] = {0x00, 0x01, 0x14, 0x15, 0x12, 0x13};
  uint8_t v[6];
  for (int i = 0; i < 6; i++) {
    if (!readReg(regs[i], v[i])) {
      Log(ERROR, "The expander does not answer on the I2C bus.");
      return;
    }
  }
  Logf(INFO, "%s = 0x%02X, %s = 0x%02X   (direction: 0 = output, 1 = input)", names[0], v[0], names[1], v[1]);
  Logf(INFO, "%s = 0x%02X, %s = 0x%02X   (what was last written to each output)", names[2], v[2], names[3], v[3]);
  Logf(INFO, "%s = 0x%02X, %s = 0x%02X   (what is on the pins right now)", names[4], v[4], names[5], v[5]);
  Log(INFO, "Expect direction 0x00 for bank A and bit 0 clear for bank B (pin 8 = GPB0).");
}
