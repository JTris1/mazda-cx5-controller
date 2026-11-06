#pragma once
#include <Arduino.h>

enum LogTypes {INFO, DEBUG, WARN, ERROR};

// Call once after Serial.begin(); makes Log safe to use from several tasks and sets up the log history.
void logBegin();

void Log(LogTypes type, const char* message);

// printf-style wrapper around Log (formats into a small stack buffer, no heap).
void Logf(LogTypes type, const char* fmt, ...);

// Log history for the admin page. Every line also goes into a ring buffer kept in RTC memory, which survives a
// crash, a watchdog or brownout restart and software restarts (but not a full power loss). After a restart the
// previous run's last lines are available as "previous", which is what shows why the device went away.
String logHistory(bool previous);   // oldest line first, one per line; empty if there is none
