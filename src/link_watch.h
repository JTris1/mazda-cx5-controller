#pragma once
#include <Arduino.h>

// Link watchdog. The device is unattended in a car, so it has to repair its own network. It watches whether the
// WireGuard tunnel has a valid session and escalates, one step at a time, while that is not the case:
//
//   tunnel down 3 min   restart the tunnel (also looks the server name up again), then every 5 min while it stays down
//   tunnel down 8 min   restart the cellular link
//   tunnel down 20 min  restart the whole device
//   cellular down 15 min restart the whole device (a stuck modem)
//
// A restart is held back while a command is running or firmware is being written. After a watchdog restart the
// next one waits longer (up to 2 hours), so a place with no coverage does not cause a restart loop.
void linkWatchPoll();            // call every loop
void linkWatchPrintStatus();

struct LinkWatchInfo {
  bool tunnelUp;
  uint32_t badForSeconds;        // how long the tunnel has been down (0 when healthy)
  uint8_t step;                  // 0 healthy, 1 tunnel restarted, 2 cellular restarted, 3 device restart pending
  uint8_t watchdogRestarts;      // consecutive watchdog restarts without a stable tunnel in between
};
void linkWatchInfo(LinkWatchInfo& out);

// True if the previous restart was requested by this watchdog (valid for the whole run).
bool linkWatchRestartedUs();

// Call once at boot (before the first log lines about the reset reason are written).
void linkWatchBegin();

// Why the chip last restarted, as text ("power-on", "crash (panic)", "brownout (supply dipped)", "watchdog", ...).
const char* resetReasonText();
