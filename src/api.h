#pragma once
#include <Arduino.h>

// HTTPS API on the WireGuard tunnel address only. apiPoll() (call every loop) starts the
// server when the tunnel is up and stops it when the tunnel goes away; the server itself runs
// in its own task. Every endpoint except GET /api/v1/challenge needs a signed request (auth.h).
void apiPoll();

bool apiRunning();

// SHA-256 fingerprint of the HTTPS certificate ("AA:BB:..."), for the admin page and Home Assistant setup.
const char* apiCertFingerprint();

// One-shot report used by the "api" console command.
void apiPrintStatus();
