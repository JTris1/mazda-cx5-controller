#pragma once
#include <Arduino.h>
#include "climate.h"

// What the firmware believes the climate controls are set to. The car cannot report any of this (the
// panel talks CAN only), so every value is the LAST COMMANDED state, kept in flash so a reboot does not
// lose it. It can drift if someone uses the physical buttons; "assume" (the Resync action) corrects it.
struct VehicleSnapshot {
  int tempF = -1;               // -1 = unknown
  int heater[2] = {0, 0};       // per Seat: 0 off, 1 high, 2 medium, 3 low
  bool defrost[2] = {false, false};   // per DefrostZone
  int fan = -1;                 // 1..fanSteps, -1 = unknown
};

// Which fields vstateAssume() should take from the snapshot.
enum VStateField : uint8_t {
  VS_TEMP = 1, VS_HEATER_DRIVER = 2, VS_HEATER_PASSENGER = 4, VS_DEFROST_FRONT = 8, VS_DEFROST_REAR = 16, VS_FAN = 32
};

void vstateBegin();                         // loads from flash; call after settingsLoad()
VehicleSnapshot vstateGet();

void vstateSetTemp(int f);                  // -1 = unknown
void vstateAdjustTemp(int delta);           // moves a known value (clamped to the range); unknown stays unknown
void vstateSetHeater(Seat seat, int level);
void vstateSetDefrost(DefrostZone zone, bool on);
void vstateSetFan(int level);               // -1 = unknown
void vstateAdjustFan(int delta);

// Resync: replace only the fields named in `fields`, without pressing anything.
void vstateAssume(const VehicleSnapshot& snapshot, uint8_t fields);

// Called when the engine goes from running to stopped: heaters and defrost are assumed off if the
// setting resetOnEngineStop is on (state only, nothing is pressed).
void vstateEngineStopped();
