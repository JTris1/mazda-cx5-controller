#pragma once
#include <Arduino.h>
#include <Wire.h>

// Minimal output-only driver for the MCP23017 (BANK=0 register map).
// Pins 0-7 = GPA0-7, pins 8-15 = GPB0-7. Everything is driven as an output;
// OLAT is cleared *before* IODIR is switched, so no pin ever pulses at start-up.
// Not thread-safe: only one task may talk to the expander (the output task).
class Mcp23017 {
 public:
  bool begin(TwoWire &bus, uint8_t addr) {
    bus_ = &bus;
    addr_ = addr;
    olat_[0] = olat_[1] = 0;
    bus_->beginTransmission(addr_);
    if (bus_->endTransmission() != 0) return ready_ = false;
    if (!writeReg(OLATA, 0) || !writeReg(OLATB, 0)) return ready_ = false;
    if (!writeReg(IODIRA, 0x00) || !writeReg(IODIRB, 0x00)) return ready_ = false;
    return ready_ = true;
  }

  bool ready() const { return ready_; }

  bool set(uint8_t pin, bool level) {
    if (!ready_ || pin > 15) return false;
    const uint8_t port = pin >> 3;
    uint8_t v = olat_[port];
    if (level) v |= (1u << (pin & 7));
    else v &= ~(1u << (pin & 7));
    if (!writeReg(port ? OLATB : OLATA, v)) return false;
    olat_[port] = v;
    return true;
  }

  bool allLow() {
    const bool a = writeReg(OLATA, 0);
    const bool b = writeReg(OLATB, 0);
    if (a && b) olat_[0] = olat_[1] = 0;
    return a && b;
  }

 private:
  enum : uint8_t { IODIRA = 0x00, IODIRB = 0x01, OLATA = 0x14, OLATB = 0x15 };

  bool writeReg(uint8_t reg, uint8_t val) {
    for (int attempt = 0; attempt < 2; attempt++) {
      bus_->beginTransmission(addr_);
      bus_->write(reg);
      bus_->write(val);
      if (bus_->endTransmission() == 0) return true;
    }
    return false;
  }

  TwoWire *bus_ = nullptr;
  uint8_t addr_ = 0x20;
  uint8_t olat_[2] = {0, 0};
  bool ready_ = false;
};
