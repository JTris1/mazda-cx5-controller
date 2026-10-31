#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include "vehicle_state.h"
#include "config.h"
#include "log.h"

static SemaphoreHandle_t mutex = nullptr;
static VehicleSnapshot state;

struct Stored {
  uint16_t version;
  uint16_t size;
  VehicleSnapshot snapshot;
};
static constexpr uint16_t VSTATE_VERSION = 1;

struct Lock {
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
};

// Called with the lock held.
static void save() {
  Stored st;
  memset(&st, 0, sizeof(st));
  st.version = VSTATE_VERSION;
  st.size = sizeof(VehicleSnapshot);
  st.snapshot = state;
  Preferences p;
  p.begin("vstate", false);
  p.putBytes("v", &st, sizeof(st));
  p.end();
}

void vstateBegin() {
  mutex = xSemaphoreCreateMutex();
  Preferences p;
  p.begin("vstate", false);
  Stored st;
  if (p.isKey("v") && p.getBytesLength("v") == sizeof(Stored)) {
    p.getBytes("v", &st, sizeof(st));
    if (st.version == VSTATE_VERSION && st.size == sizeof(VehicleSnapshot)) {
      state = st.snapshot;
      for (int& h : state.heater) h = constrain(h, 0, cfg.seatHeaterStages);
      if (state.fan > cfg.fanSteps) state.fan = -1;
      Log(INFO, "Tracked climate state restored from flash");
    }
  }
  p.end();
}

VehicleSnapshot vstateGet() {
  Lock l;
  return state;
}

void vstateSetTemp(int f) {
  Lock l;
  state.tempF = f;
  save();
}

void vstateAdjustTemp(int delta) {
  Lock l;
  if (state.tempF >= 0) state.tempF = constrain(state.tempF + delta, cfg.tempMinF, cfg.tempMaxF);
  save();
}

void vstateSetHeater(Seat seat, int level) {
  Lock l;
  state.heater[seat] = level;
  save();
}

void vstateSetDefrost(DefrostZone zone, bool on) {
  Lock l;
  state.defrost[zone] = on;
  save();
}

void vstateSetFan(int level) {
  Lock l;
  state.fan = level;
  save();
}

void vstateAdjustFan(int delta) {
  Lock l;
  if (state.fan >= 1) state.fan = constrain(state.fan + delta, 1, cfg.fanSteps);
  save();
}

void vstateAssume(const VehicleSnapshot& s, uint8_t fields) {
  Lock l;
  if (fields & VS_TEMP) state.tempF = s.tempF;
  if (fields & VS_HEATER_DRIVER) state.heater[SEAT_DRIVER] = s.heater[SEAT_DRIVER];
  if (fields & VS_HEATER_PASSENGER) state.heater[SEAT_PASSENGER] = s.heater[SEAT_PASSENGER];
  if (fields & VS_DEFROST_FRONT) state.defrost[DEFROST_FRONT] = s.defrost[DEFROST_FRONT];
  if (fields & VS_DEFROST_REAR) state.defrost[DEFROST_REAR] = s.defrost[DEFROST_REAR];
  if (fields & VS_FAN) state.fan = s.fan;
  save();
}

void vstateEngineStopped() {
  if (!cfg.resetOnEngineStop) return;
  Lock l;
  bool changed = state.heater[0] || state.heater[1] || state.defrost[0] || state.defrost[1];
  state.heater[0] = state.heater[1] = 0;
  state.defrost[0] = state.defrost[1] = false;
  if (changed) {
    save();
    Log(INFO, "Engine stopped: tracked seat heater and defrost state reset to off");
  }
}
