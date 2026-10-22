#pragma once
#include <Arduino.h>

// HTTPS API on the WireGuard tunnel address only. apiPoll() (call every loop) starts the
// server when the tunnel is up and stops it when the tunnel goes away; the server itself runs
// in its own task. Every endpoint except GET /api/v1/challenge needs a signed request (auth.h).
void apiPoll();

bool apiRunning();

// One-shot report used by the "api" console command.
void apiPrintStatus();
