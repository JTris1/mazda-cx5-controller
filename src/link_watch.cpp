#include <esp_attr.h>
#include <esp_system.h>
#include "link_watch.h"
#include "actions.h"
#include "log.h"
#include "net_cell.h"
#include "net_time.h"
#include "net_wg.h"
#include "ota.h"

constexpr uint32_t HEALTHY_AFTER_MS = 30000;        // tunnel must stay up this long to count as repaired
constexpr uint32_t STABLE_RESET_MS = 600000;        // this long healthy forgets earlier watchdog restarts
constexpr uint32_t TUNNEL_RESTART_MS = 3 * 60000;
constexpr uint32_t TUNNEL_RETRY_MS = 5 * 60000;     // keep retrying the tunnel this often while it is down
constexpr uint32_t CELL_RESTART_MS = 8 * 60000;
constexpr uint32_t DEVICE_RESTART_MS = 20 * 60000;
constexpr uint32_t CELL_DOWN_RESTART_MS = 15 * 60000;
// A restart held back for a running command or an upload goes ahead anyway after this long (a stuck job must not
// keep a dead device dead).
constexpr uint32_t OVERDUE_MS = 30 * 60000;
constexpr uint32_t MAX_DEVICE_RESTART_MS = 120 * 60000;

constexpr uint32_t WD_MAGIC = 0x57444731;
RTC_NOINIT_ATTR static uint32_t wdMagic;
RTC_NOINIT_ATTR static uint32_t wdConsecutive;      // watchdog restarts in a row
RTC_NOINIT_ATTR static uint32_t wdRequested;        // set just before we restart the device

static bool restartedUs = false;
static uint32_t badSince = 0;        // millis() when the tunnel was last known healthy and then was not; 0 = healthy
static uint32_t healthySince = 0;
static uint32_t cellDownSince = 0;
static uint8_t step = 0;
static uint32_t lastTunnelRestart = 0;
static bool initDone = false;

bool linkWatchRestartedUs() {
  return restartedUs;
}

static void watchInit() {
  initDone = true;
  if (wdMagic != WD_MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
    wdMagic = WD_MAGIC;
    wdConsecutive = 0;
    wdRequested = 0;
  }
  restartedUs = wdRequested == 1;
  wdRequested = 0;
  if (restartedUs) Logf(WARN, "Link watchdog: the previous restart was requested by the watchdog (%lu in a row)", (unsigned long)wdConsecutive);
}

void linkWatchBegin() {
  if (!initDone) watchInit();
}

const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on (supply was cut)";
    case ESP_RST_SW:       return restartedUs ? "link watchdog restart" : "software restart";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_BROWNOUT: return "brownout (supply dipped)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog (the firmware hung)";
    case ESP_RST_EXT:      return "reset pin";
    default:               return "other";
  }
}

static bool safeToRestart() {
  uint32_t running = 0;
  int queued = 0;
  actionsBusyInfo(running, queued);
  return !running && !queued && !otaReceiving();
}

static void restartDevice(const char* why) {
  Logf(ERROR, "Link watchdog: restarting the device: %s", why);
  wdRequested = 1;
  wdConsecutive++;
  delay(300);
  esp_restart();
}

void linkWatchInfo(LinkWatchInfo& out) {
  out.tunnelUp = wgTunnelUp();
  out.badForSeconds = badSince ? (millis() - badSince) / 1000 : 0;
  out.step = step;
  out.watchdogRestarts = initDone ? (uint8_t)min<uint32_t>(wdConsecutive, 255) : 0;
}

void linkWatchPoll() {
  if (!initDone) watchInit();
  uint32_t now = millis();

  // --- the cellular link itself ---
  if (cellConnected()) {
    cellDownSince = 0;
  } else {
    if (!cellDownSince) cellDownSince = now;
    if (now - cellDownSince >= CELL_DOWN_RESTART_MS && (safeToRestart() || now - cellDownSince >= CELL_DOWN_RESTART_MS + OVERDUE_MS)) restartDevice("cellular link down for 15 minutes");
  }

  // --- the tunnel ---
  if (wgTunnelUp()) {
    if (!healthySince) healthySince = now;
    if (now - healthySince >= HEALTHY_AFTER_MS && badSince) {
      Logf(INFO, "Link watchdog: tunnel healthy again after %lu s", (unsigned long)((now - badSince) / 1000));
      badSince = 0;
      step = 0;
      lastTunnelRestart = 0;
    }
    if (wdConsecutive && now - healthySince >= STABLE_RESET_MS) wdConsecutive = 0;
    return;
  }
  healthySince = 0;
  if (!timeSynced()) return;                       // the tunnel cannot start before the clock is set
  if (!badSince) badSince = now;
  uint32_t bad = now - badSince;

  // The WireGuard library gives up after about 90 s of failed handshakes and never tries again by itself, so
  // a server that was unreachable for a while (a router update, say) must be knocked on again and again.
  if (bad >= TUNNEL_RESTART_MS && (!lastTunnelRestart || now - lastTunnelRestart >= TUNNEL_RETRY_MS)) {
    if (step < 1) step = 1;
    lastTunnelRestart = now;
    Logf(WARN, "Link watchdog: tunnel down %lu s, restarting the tunnel", (unsigned long)(bad / 1000));
    wgRestart();
  }
  if (step < 2 && bad >= CELL_RESTART_MS) {
    step = 2;
    Logf(WARN, "Link watchdog: tunnel down %lu s, restarting the cellular link", (unsigned long)(bad / 1000));
    cellRequestRestart();
  } else if (step < 3 && bad >= DEVICE_RESTART_MS) {
    step = 3;
  }
  if (step == 3) {
    // Each restart in a row waits longer: 20, 40, 80, then 120 minutes.
    uint32_t wait = min<uint32_t>(DEVICE_RESTART_MS << min<uint32_t>(wdConsecutive, 3), MAX_DEVICE_RESTART_MS);
    if (bad >= wait && (safeToRestart() || bad >= wait + OVERDUE_MS)) restartDevice("the tunnel never came back");
  }
}

void linkWatchPrintStatus() {
  LinkWatchInfo i;
  linkWatchInfo(i);
  Logf(INFO, "Link watchdog: tunnel %s, step %u, %lu s unhealthy, %u watchdog restart(s) in a row", i.tunnelUp ? "up" : "DOWN",
       i.step, (unsigned long)i.badForSeconds, i.watchdogRestarts);
}
