#include <Preferences.h>
#include <string.h>
#include "settings.h"
#include "config.h"
#include "log.h"
#include "vehicle_state.h"

static constexpr uint16_t SETTINGS_VERSION = 1;   // bump when Settings changes shape

struct Stored {
  uint16_t version;
  uint16_t size;
  Settings settings;
};

// Keep every value inside the range the console and the API accept, whatever is in flash.
static void sanitize(Settings& s) {
  s.holdMs = constrain(s.holdMs, 10u, HOLD_MS_MAX);
  s.gapMs = constrain(s.gapMs, 0u, 5000u);
  if (s.tempMinF >= s.tempMaxF) { s.tempMinF = 60; s.tempMaxF = 84; }
  s.clampExtra = constrain(s.clampExtra, 0, 10);
  s.rsPulseMs = constrain(s.rsPulseMs, 20u, RS_PULSE_MS_MAX);
  s.rsGapMs = constrain(s.rsGapMs, 0u, 5000u);
  s.rsPulses = constrain(s.rsPulses, 1, 3);
  s.rsConfirmMs = constrain(s.rsConfirmMs, 1000u, 120000u);
  s.seatHeaterStages = constrain(s.seatHeaterStages, 1, 6);
  s.fanSteps = constrain(s.fanSteps, 2, FAN_STEPS_LIMIT);
  s.fanHomeExtra = constrain(s.fanHomeExtra, 0, 5);
}

void settingsLoad() {
  Preferences p;
  p.begin("settings", false);
  Stored st;
  if (p.isKey("cfg") && p.getBytesLength("cfg") == sizeof(Stored)) {
    p.getBytes("cfg", &st, sizeof(st));
    if (st.version == SETTINGS_VERSION && st.size == sizeof(Settings)) {
      sanitize(st.settings);
      cfg = st.settings;
      Log(INFO, "Settings loaded from flash");
      p.end();
      return;
    }
  }
  p.end();
  Log(INFO, "Settings: using defaults (nothing saved yet)");
}

void settingsSave() {
  Stored st;
  memset(&st, 0, sizeof(st));
  st.version = SETTINGS_VERSION;
  st.size = sizeof(Settings);
  st.settings = cfg;
  sanitize(st.settings);
  Preferences p;
  p.begin("settings", false);
  p.putBytes("cfg", &st, sizeof(st));
  p.end();
}

void settingsPrint() {
  Logf(INFO, "hold %lu ms, gap %lu ms", (unsigned long)cfg.holdMs, (unsigned long)cfg.gapMs);
  Logf(INFO, "temperature range %d-%d F, clamp extra %d", cfg.tempMinF, cfg.tempMaxF, cfg.clampExtra);
  Logf(INFO, "fan steps %d, fan home extra %d", cfg.fanSteps, cfg.fanHomeExtra);
  Logf(INFO, "seat heater stages %d", cfg.seatHeaterStages);
  Logf(INFO, "remote start: %d pulse(s), %lu ms on, %lu ms gap, confirm wait %lu s", cfg.rsPulses,
       (unsigned long)cfg.rsPulseMs, (unsigned long)cfg.rsGapMs, (unsigned long)(cfg.rsConfirmMs / 1000));
  Logf(INFO, "reset tracked heater/defrost state when the engine stops: %s", cfg.resetOnEngineStop ? "yes" : "no");
}

// --- JSON access (admin API) ---------------------------------------------------------
struct IntField { const char* name; int Settings::*i; uint32_t Settings::*u; long lo; long hi; };
static const IntField INT_FIELDS[] = {
  {"hold_ms",            nullptr, &Settings::holdMs,      10, (long)HOLD_MS_MAX},
  {"gap_ms",             nullptr, &Settings::gapMs,        0, 5000},
  {"temp_min_f",         &Settings::tempMinF, nullptr,    40, 100},
  {"temp_max_f",         &Settings::tempMaxF, nullptr,    40, 100},
  {"clamp_extra",        &Settings::clampExtra, nullptr,   0, 10},
  {"rs_pulse_ms",        nullptr, &Settings::rsPulseMs,   20, (long)RS_PULSE_MS_MAX},
  {"rs_gap_ms",          nullptr, &Settings::rsGapMs,      0, 5000},
  {"rs_pulses",          &Settings::rsPulses, nullptr,     1, 3},
  {"rs_confirm_ms",      nullptr, &Settings::rsConfirmMs, 1000, 120000},
  {"fan_steps",          &Settings::fanSteps, nullptr,     2, FAN_STEPS_LIMIT},
  {"fan_home_extra",     &Settings::fanHomeExtra, nullptr, 0, 5},
  {"seat_heater_stages", &Settings::seatHeaterStages, nullptr, 1, 6},
};

void settingsToJson(JsonObject out) {
  for (const IntField& f : INT_FIELDS) out[f.name] = f.i ? (long)(cfg.*f.i) : (long)(cfg.*f.u);
  out["reset_on_engine_stop"] = cfg.resetOnEngineStop;
}

bool settingsApply(JsonObjectConst in, const char** error) {
  Settings next = cfg;
  for (JsonPairConst kv : in) {
    const char* key = kv.key().c_str();
    if (strcmp(key, "reset_on_engine_stop") == 0) {
      if (!kv.value().is<bool>()) { *error = "reset_on_engine_stop must be true or false"; return false; }
      next.resetOnEngineStop = kv.value().as<bool>();
      continue;
    }
    const IntField* field = nullptr;
    for (const IntField& f : INT_FIELDS) if (strcmp(f.name, key) == 0) field = &f;
    if (!field) { *error = "unknown setting"; return false; }
    if (!kv.value().is<long>()) { *error = "setting must be a whole number"; return false; }
    long v = kv.value().as<long>();
    if (v < field->lo || v > field->hi) { *error = "setting out of range"; return false; }
    if (field->i) next.*(field->i) = (int)v; else next.*(field->u) = (uint32_t)v;
  }
  if (next.tempMinF >= next.tempMaxF) { *error = "temp_min_f must be below temp_max_f"; return false; }
  cfg = next;
  settingsSave();
  VehicleSnapshot v = vstateGet();
  if (v.tempF >= 0 && (v.tempF < cfg.tempMinF || v.tempF > cfg.tempMaxF)) vstateSetTemp(-1);
  if (v.fan > cfg.fanSteps) vstateSetFan(-1);
  Log(INFO, "Settings changed from the API");
  return true;
}
