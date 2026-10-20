#pragma once
#include <Arduino.h>

// WireGuard tunnel to the home server. The API will listen only on the tunnel address.
// wgPoll() (call every loop) starts the tunnel once Wi-Fi is up and the clock is synced,
// and tears it down when Wi-Fi drops so it can restart cleanly after a reconnect.
void wgPoll();

bool wgStarted();

// One-shot report used by the "wg" console command.
void wgPrintStatus();
