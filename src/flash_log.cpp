#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include "flash_log.h"
#include "config.h"
#include "link_watch.h"
#include "log.h"
#include "net_cell.h"
#include "net_wg.h"

static constexpr const char* FILE_NOW = "/log.txt";
static constexpr const char* FILE_OLD = "/log.1.txt";
static constexpr size_t FILE_MAX = 96 * 1024;
static constexpr uint32_t FLUSH_MS = 30000;
static constexpr uint32_t HEARTBEAT_MS = 15 * 60000;
static constexpr size_t PENDING_MAX = 2048;

static bool mounted = false;
static portMUX_TYPE pendingLock = portMUX_INITIALIZER_UNLOCKED;
static char pending[PENDING_MAX];
static size_t pendingLen = 0;
static uint32_t lastFlush = 0;
static uint32_t lastBeat = 0;
static SemaphoreHandle_t fsMutex = nullptr;

// Which lines are worth the flash: problems, and the events that explain a network outage.
static bool worthKeeping(LogTypes type, const char* line) {
  if (type == WARN || type == ERROR) return true;
  return strncmp(line, "Cellular: connected", 19) == 0 || strncmp(line, "WireGuard: started", 18) == 0 ||
         strncmp(line, "Link watchdog", 13) == 0 || strncmp(line, "OTA:", 4) == 0 || strncmp(line, "Last reset", 10) == 0;
}

void flashLogQueue(LogTypes type, const char* line) {
  if (!worthKeeping(type, line)) return;
  char stamp[24];
  logStamp(stamp, sizeof(stamp));
  char out[200];
  int n = snprintf(out, sizeof(out), "%s %c %s\n", stamp, "IDWE"[type], line);
  if (n <= 0) return;
  portENTER_CRITICAL(&pendingLock);
  if (pendingLen + n < PENDING_MAX) {
    memcpy(pending + pendingLen, out, n);
    pendingLen += n;
  }                                         // a full buffer drops the newest line: the RTC log still has it
  portEXIT_CRITICAL(&pendingLock);
}

static void appendRaw(const char* text, size_t len) {
  if (!mounted || !len) return;
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  File f = LittleFS.open(FILE_NOW, FILE_APPEND);
  if (f) {
    f.write((const uint8_t*)text, len);
    size_t size = f.size();
    f.close();
    if (size > FILE_MAX) {                  // rotate
      LittleFS.remove(FILE_OLD);
      LittleFS.rename(FILE_NOW, FILE_OLD);
    }
  }
  xSemaphoreGive(fsMutex);
}

static void appendLine(const char* text) {
  char stamp[24];
  logStamp(stamp, sizeof(stamp));
  char out[320];
  int n = snprintf(out, sizeof(out), "%s %s\n", stamp, text);
  if (n > 0) appendRaw(out, min((size_t)n, sizeof(out) - 1));
}

static void flushPending() {
  char copy[PENDING_MAX];
  size_t len;
  portENTER_CRITICAL(&pendingLock);
  len = pendingLen;
  memcpy(copy, pending, len);
  pendingLen = 0;
  portEXIT_CRITICAL(&pendingLock);
  appendRaw(copy, len);
}

static void heartbeat() {
  LinkWatchInfo w;
  linkWatchInfo(w);
  char line[200];
  snprintf(line, sizeof(line), "HB up=%lus heap=%u min=%u cell=%s tunnel=%s bad=%lus wd_step=%u wd_restarts=%u",
           (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
           cellConnected() ? "up" : "DOWN", w.tunnelUp ? "up" : "DOWN", (unsigned long)w.badForSeconds, w.step, w.watchdogRestarts);
  appendLine(line);
}

void flashLogBegin() {
  fsMutex = xSemaphoreCreateMutex();
  mounted = LittleFS.begin(true, "/lfs", 5, "spiffs");     // formats the partition the first time
  if (!mounted) { Log(WARN, "Flash log: could not mount the log partition"); return; }
  char line[96];
  snprintf(line, sizeof(line), "BOOT reset=%s fw=%s", resetReasonText(), FIRMWARE_VERSION);
  appendLine(line);
  // Keep the warnings of the run that just ended, even if power is lost before anyone reads them.
  String prev = logHistory(true);
  if (prev.length()) {
    int kept = 0;
    int end = prev.length();
    String keep;
    while (end > 0 && kept < 30) {                         // newest first, then put back in order
      int start = prev.lastIndexOf('\n', end - 2) + 1;
      String l = prev.substring(start, end - 1);
      if (l.length() > 16 && (l.indexOf(" W ") >= 0 || l.indexOf(" E ") >= 0) && l.indexOf(" W ") < 24) {
        keep = l + "\n" + keep;
        kept++;
      }
      end = start;
    }
    if (kept) appendRaw(("---- warnings of the previous run ----\n" + keep + "----\n").c_str(), 64 + keep.length());
  }
  lastBeat = lastFlush = millis();
  heartbeat();
}

void flashLogPoll() {
  if (!mounted) return;
  uint32_t now = millis();
  if (now - lastFlush >= FLUSH_MS) {
    lastFlush = now;
    if (pendingLen) flushPending();
  }
  if (now - lastBeat >= HEARTBEAT_MS) {
    lastBeat = now;
    heartbeat();
  }
}

String flashLogRead(size_t maxBytes) {
  String out;
  if (!mounted) return out;
  if (pendingLen) flushPending();
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  const char* files[2] = {FILE_OLD, FILE_NOW};
  for (const char* path : files) {
    File f = LittleFS.open(path, FILE_READ);
    if (!f) continue;
    out += f.readString();
    f.close();
  }
  xSemaphoreGive(fsMutex);
  if (out.length() > maxBytes) {
    out = out.substring(out.length() - maxBytes);
    int nl = out.indexOf('\n');
    if (nl >= 0) out = out.substring(nl + 1);
  }
  return out;
}
