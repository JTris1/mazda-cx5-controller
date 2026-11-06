#pragma once
#include <Arduino.h>
#include "log.h"

// A log that survives power loss. The RAM/RTC log in log.cpp is lost when the supply is cut, which is exactly what
// happens when someone unplugs a stuck device. This one is written to flash (LittleFS on the "spiffs" partition):
//
//   * every warning and error, and the main network events, written within 30 seconds of happening
//   * a heartbeat line every 15 minutes (uptime, memory, cellular, tunnel, watchdog state). The last heartbeat before a
//     long outage shows when the device stopped being alive, even if nothing was logged at the time
//   * a BOOT line at every start with the reset reason and the warnings of the run that just ended
//
// Two files of up to 96 KB each are kept (about 20 days of heartbeats), oldest dropped first.
void flashLogBegin();                        // after logBegin() and linkWatchBegin()
void flashLogPoll();                         // call every loop
void flashLogQueue(LogTypes type, const char* line);   // called by Log(); cheap, never blocks on flash
String flashLogRead(size_t maxBytes);        // the newest part of the saved log, oldest line first
