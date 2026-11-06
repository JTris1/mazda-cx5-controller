#include <Arduino.h>
#include <stdarg.h>
#include "log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_system.h>
#include <time.h>

static SemaphoreHandle_t logMutex = nullptr;

constexpr size_t RING_SIZE = 6144;
constexpr uint32_t RING_MAGIC = 0x4c4f4731;
RTC_NOINIT_ATTR static uint32_t ringMagic;
RTC_NOINIT_ATTR static uint32_t ringHead;     // next write position
RTC_NOINIT_ATTR static uint32_t ringWrapped;  // 1 once the buffer has gone around
RTC_NOINIT_ATTR static char ring[RING_SIZE];
static String previousRun;                    // the ring as it was when this run started

static String ringText() {
  String out;
  if (ringMagic != RING_MAGIC || ringHead >= RING_SIZE) return out;
  out.reserve(RING_SIZE);
  size_t start = ringWrapped ? ringHead : 0;
  size_t count = ringWrapped ? RING_SIZE : ringHead;
  for (size_t i = 0; i < count; i++) out += ring[(start + i) % RING_SIZE];
  int nl = ringWrapped ? out.indexOf('\n') : -1;       // drop the first, possibly cut, line
  return nl >= 0 ? out.substring(nl + 1) : out;
}

static void ringAdd(const char* line) {
  for (const char* c = line; ; c++) {
    char ch = *c ? *c : '\n';
    ring[ringHead++] = ch;
    if (ringHead >= RING_SIZE) { ringHead = 0; ringWrapped = 1; }
    if (!*c) break;
  }
}

String logHistory(bool previous) {
  if (previous) return previousRun;
  if (logMutex) xSemaphoreTake(logMutex, portMAX_DELAY);
  String s = ringText();
  if (logMutex) xSemaphoreGive(logMutex);
  return s;
}

void logBegin() {
  if (!logMutex) logMutex = xSemaphoreCreateMutex();
  esp_reset_reason_t r = esp_reset_reason();
  if (r != ESP_RST_POWERON) previousRun = ringText();   // RTC memory is only trustworthy after a non-power-on reset
  ringMagic = RING_MAGIC;
  ringHead = 0;
  ringWrapped = 0;
  ring[0] = '\0';
}

void Log(LogTypes type, const char* message) {
  if (logMutex) xSemaphoreTake(logMutex, portMAX_DELAY);
  if (ringMagic == RING_MAGIC) {
    char stamp[24];
    time_t now = time(nullptr);
    if (now > 1700000000) {                       // the clock has been set (NTP)
      struct tm t;
      gmtime_r(&now, &t);
      snprintf(stamp, sizeof(stamp), "%02d-%02d %02d:%02d:%02dZ", t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      snprintf(stamp, sizeof(stamp), "+%lus", (unsigned long)(millis() / 1000));
    }
    char line[176];
    snprintf(line, sizeof(line), "%s %c %s", stamp, "IDWE"[type], message);
    ringAdd(line);
  }
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
  if (logMutex) xSemaphoreGive(logMutex);
}

void Logf(LogTypes type, const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Log(type, buf);
}
