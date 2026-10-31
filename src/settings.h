#pragma once
#include <ArduinoJson.h>

// Persistent settings. cfg (config.h) holds the working values; this loads them from flash at boot
// and writes them back after every change, so tuning done from the console or the admin page survives
// a reboot. A stored copy that does not match the current Settings layout is ignored (defaults are used).
void settingsLoad();
void settingsSave();

// Logs every setting, one line each (the "settings" console command).
void settingsPrint();

// Admin page / API access. Field names are snake_case versions of the Settings members (see docs/api.md).
void settingsToJson(JsonObject out);

// Applies a partial update from JSON. Rejects (and changes nothing) on an unknown field, a wrong type or an
// out-of-range value, and returns false with `error` pointing at a short reason. On success the settings are
// saved to flash and tracked values that no longer fit the new limits become unknown.
bool settingsApply(JsonObjectConst in, const char** error);
