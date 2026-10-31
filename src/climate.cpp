#include "climate.h"
#include "config.h"
#include "log.h"
#include "outputs.h"
#include "vehicle_state.h"

void climateTempStep(bool up, int n) {
  Logf(INFO, "%s x%d", up ? "up" : "down", n);
  pressButton(up ? TEMP_UP_PIN : TEMP_DOWN_PIN, n);
  vstateAdjustTemp(up ? n : -n);
}

void climateSetTemperature(int targetF) {
  int downs = (cfg.tempMaxF - cfg.tempMinF) + cfg.clampExtra;
  int ups = targetF - cfg.tempMinF;
  Logf(INFO, "Temp %d F: %d down, then %d up", targetF, downs, ups);
  pressButton(TEMP_DOWN_PIN, downs);
  pressButton(TEMP_UP_PIN, ups);
  vstateSetTemp(targetF);
  Logf(INFO, "Temp set to %d", targetF);
}

// Seat heater state is tracked (vehicle_state): the car gives no feedback, so we count presses since
// "off". Each press steps down one level and the cycle is stages+1 presses:
//   press 1 = high, press 2 = medium, press 3 = low, press 4 = off (back to the start).
// So the stage number is both the press count and the level: 0 off, 1 high, 2 medium, 3 low.
void climateSetHeater(Seat seat, int level) {
  uint8_t pin = seat == SEAT_DRIVER ? DRIVER_SEAT_HEATER_PIN : PASSENGER_SEAT_HEATER_PIN;
  int stage = vstateGet().heater[seat];
  int cycle = cfg.seatHeaterStages + 1;
  int presses = (level - stage + cycle) % cycle;
  Logf(INFO, "Seat heater: stage %d -> %d (%d presses)", stage, level, presses);
  pressButton(pin, presses);
  vstateSetHeater(seat, level);
}

// Each defrost button toggles, and the car gives no feedback, so the state is tracked and a press
// only happens when the tracked state differs from the request.
void climateSetDefrost(DefrostZone zone, bool wantOn) {
  const char* name = zone == DEFROST_FRONT ? "Front" : "Rear";
  const char* word = wantOn ? "on" : "off";
  if (vstateGet().defrost[zone] == wantOn) {
    Logf(INFO, "%s defrost is already %s.", name, word);
    return;
  }
  pressButton(zone == DEFROST_FRONT ? FRONT_DEFROST_PIN : REAR_DEFROST_PIN);
  vstateSetDefrost(zone, wantOn);
  Logf(INFO, "%s defrost turned %s", name, word);
}

void climateFanStep(bool up, int n) {
  Logf(INFO, "%s x%d", up ? "fan up" : "fan down", n);
  pressButton(up ? FAN_UP_PIN : FAN_DOWN_PIN, n);
  vstateAdjustFan(up ? n : -n);
}

void climateSetFan(int level) {
  int downs = (cfg.fanSteps - 1) + cfg.fanHomeExtra;
  int ups = level - 1;
  Logf(INFO, "Fan %d: %d down, then %d up", level, downs, ups);
  pressButton(FAN_DOWN_PIN, downs);
  pressButton(FAN_UP_PIN, ups);
  vstateSetFan(level);
  Logf(INFO, "Fan set to %d", level);
}

void climatePowerPress() {
  Log(INFO, "Climate power button pressed");
  pressButton(CLIMATE_POWER_PIN);
}
