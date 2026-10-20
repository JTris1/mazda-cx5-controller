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
    FAN_DOWN_PIN,
    CLIMATE_POWER_PIN
  }) {
    mcp.pinMode(pin, OUTPUT);
    mcp.digitalWrite(pin, LOW);
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
