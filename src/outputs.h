#pragma once
#include <Adafruit_MCP23X17.h>

extern Adafruit_MCP23X17 mcp;
extern bool expanderOk;

// Starts I2C and the expander, drives every expander output LOW. Logs the result.
void outputsBegin();

// Reads the expander's registers and logs them (the "i2c" console command). Read-only: it changes nothing.
void outputsPrintStatus();

// Blocking for now: one press = HIGH for holdMs, LOW, then wait gapMs.
void pressButton(uint8_t pin);
void pressButton(uint8_t pin, int times);
