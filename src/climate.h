#pragma once
#include <Arduino.h>

// Climate controls. These only press buttons: argument validation and the
// "expander present" check belong to the caller (console now, API later).

enum Seat { SEAT_DRIVER, SEAT_PASSENGER };
enum DefrostZone { DEFROST_FRONT, DEFROST_REAR };

// Press the temperature up/down button n times.
void climateTempStep(bool up, int n);

// Absolute temperature: press down enough to clamp at LO, then count up.
// targetF must already be within cfg.tempMinF..cfg.tempMaxF.
void climateSetTemperature(int targetF);

// Seat heater level: 0 off, 1 high, 2 medium, 3 low (level <= cfg.seatHeaterStages).
// The stage is tracked in software because the car gives no feedback.
void climateSetHeater(Seat seat, int level);

// Defrost buttons toggle; the state is tracked in software and a press only happens
// when the tracked state differs from the request.
void climateSetDefrost(DefrostZone zone, bool wantOn);

// Fan speed up/down button n times.
void climateFanStep(bool up, int n);

// Climate power button: a momentary switch, one press toggles. No state is tracked.
void climatePowerPress();
