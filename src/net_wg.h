#pragma once
#include <Arduino.h>

// WireGuard tunnel to the home server. The API will listen only on the tunnel address.
// wgPoll() (call every loop) starts the tunnel once Wi-Fi is up and the clock is synced,
// and tears it down when Wi-Fi drops so it can restart cleanly after a reconnect.
void wgPoll();

bool wgStarted();

// Stops the tunnel; wgPoll() starts it again on its next pass (handy while debugging).
void wgRestart();

// One-shot report used by the "wg" console command.
void wgPrintStatus();
