#pragma once
#include <Arduino.h>

// NTP time. WireGuard needs real time for its handshake, so this runs as soon as Wi-Fi is up.
// timePoll() (call every loop) starts the sync once Wi-Fi is connected and logs when it lands.
void timePoll();

bool timeSynced();

// "2026-10-19 14:03:22 UTC", or "not synced" if the clock has not been set yet.
String timeNowString();
