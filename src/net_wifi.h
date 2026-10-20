#pragma once
#include <Arduino.h>

// Station-mode Wi-Fi to the hotspot. Non-blocking: wifiBegin() starts the first attempt,
// wifiPoll() (call every loop) retries with a growing delay and logs state changes.
void wifiBegin();
void wifiPoll();

bool wifiConnected();

// One-shot report used by the "net" console command.
void wifiPrintStatus();
