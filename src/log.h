#pragma once

enum LogTypes {INFO, DEBUG, WARN, ERROR};

// Call once after Serial.begin(); makes Log safe to use from several tasks.
void logBegin();

void Log(LogTypes type, const char* message);

// printf-style wrapper around Log (formats into a small stack buffer, no heap).
void Logf(LogTypes type, const char* fmt, ...);
