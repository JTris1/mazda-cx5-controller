#include "climate.h"
#include "config.h"
#include "log.h"
#include "outputs.h"

void climateTempStep(bool up, int n) {
  Logf(INFO, "%s x%d", up ? "up" : "down", n);
  pressButton(up ? TEMP_UP_PIN : TEMP_DOWN_PIN, n);
}

void climateSetTemperature(int targetF) {
  int downs = (cfg.tempMaxF - cfg.tempMinF) + cfg.clampExtra;
  int ups = targetF - cfg.tempMinF;
  Logf(INFO, "Temp %d F: %d down, then %d up", targetF, downs, ups);
  pressButton(TEMP_DOWN_PIN, downs);
  pressButton(TEMP_UP_PIN, ups);
  Logf(INFO, "Temp set to %d", targetF);
}

// Seat heater state is tracked in software: the car gives no feedback, so we count
// presses since "off". Each press steps down one level and the cycle is stages+1 presses:
//   press 1 = high, press 2 = medium, press 3 = low, press 4 = off (back to the start).
// So the stage number is both the press count and the level: 0 off, 1 high, 2 medium, 3 low.
static int driverHeaterStage = 0;
static int passengerHeaterStage = 0;

// Press the button as many times as needed to land on targetStage.
static void setSeatHeater(uint8_t pin, int& stage, int targetStage) {
  int cycle = cfg.seatHeaterStages + 1;
  int presses = (targetStage - stage + cycle) % cycle;
  Logf(INFO, "Seat heater: stage %d -> %d (%d presses)", stage, targetStage, presses);
  pressButton(pin, presses);
  stage = targetStage;
}

void climateSetHeater(Seat seat, int level) {
  if (seat == SEAT_DRIVER) {
    setSeatHeater(DRIVER_SEAT_HEATER_PIN, driverHeaterStage, level);
  } else {
    setSeatHeater(PASSENGER_SEAT_HEATER_PIN, passengerHeaterStage, level);
  }
}

// Each defrost button toggles, and the car gives no feedback, so we track the state.
static bool frontDefrostOn = false;
static bool rearDefrostOn = false;

// Press the button only if the tracked state differs from what was asked for.
static void setDefrost(const char* name, uint8_t pin, bool& state, bool wantOn) {
  const char* word = wantOn ? "on" : "off";
  if (state == wantOn) {
    Logf(INFO, "%s defrost is already %s.", name, word);
    return;
  }
  pressButton(pin);
  state = wantOn;
  Logf(INFO, "%s defrost turned %s", name, word);
}

void climateSetDefrost(DefrostZone zone, bool wantOn) {
  if (zone == DEFROST_FRONT) {
    setDefrost("Front", FRONT_DEFROST_PIN, frontDefrostOn, wantOn);
  } else {
    setDefrost("Rear", REAR_DEFROST_PIN, rearDefrostOn, wantOn);
  }
}

void climateFanStep(bool up, int n) {
  Logf(INFO, "%s x%d", up ? "fan up" : "fan down", n);
  pressButton(up ? FAN_UP_PIN : FAN_DOWN_PIN, n);
}

void climatePowerPress() {
  Log(INFO, "Climate power button pressed");
  pressButton(CLIMATE_POWER_PIN);
}
