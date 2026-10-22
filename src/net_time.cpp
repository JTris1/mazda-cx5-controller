#include <time.h>
#include "net_time.h"
#include "net_cell.h"
#include "log.h"

// Any time earlier than this means the clock has not been set (it starts at 1970).
static constexpr time_t MIN_VALID_TIME = 1700000000;  // Nov 2023

static bool started = false;
static bool announced = false;

bool timeSynced() {
  return time(nullptr) > MIN_VALID_TIME;
}

void timePoll() {
  if (!cellConnected()) return;

  if (!started) {
    started = true;
    configTime(0, 0, "pool.ntp.org", "time.google.com");  // UTC; the SNTP client keeps it updated
    Log(INFO, "Time: NTP sync started");
  }
  if (!announced && timeSynced()) {
    announced = true;
    Logf(INFO, "Time: synced, %s", timeNowString().c_str());
  }
}

String timeNowString() {
  if (!timeSynced()) return "not synced";
  time_t now = time(nullptr);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tmv);
  return String(buf);
}
