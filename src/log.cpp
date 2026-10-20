#include <Arduino.h>
#include <stdarg.h>
#include "log.h"

void Log(LogTypes type, const char* message) {
  switch (type) {
    case INFO:
      Serial.printf("INFO  | %s\n", message);
      break;
    case DEBUG:
      Serial.printf("DEBUG | %s\n", message);
      break;
    case WARN:
      Serial.printf("WARN  | %s\n", message);
      break;
    case ERROR:
      Serial.printf("ERROR | %s\n", message);
      break;
  }
}

void Logf(LogTypes type, const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Log(type, buf);
}
