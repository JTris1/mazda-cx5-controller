#include <Arduino.h>
#include "util.h"

bool parseInt(const char* text, long& out) {
  if (!text || !*text) return false;
  char* end;
  out = strtol(text, &end, 10);
  return *end == '\0';
}
