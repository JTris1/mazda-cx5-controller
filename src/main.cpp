#include <Arduino.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "actions.h"
#include "api.h"
#include "auth.h"
#include "console.h"
#include "engine.h"
#include "flash_log.h"
#include "link_watch.h"
#include "log.h"
#include "net_wg.h"
#include "net_cell.h"
#include "net_time.h"
#include "outputs.h"
#include "remote_start.h"
#include "settings.h"
#include "vehicle_state.h"

void setup() {
  remoteStartBegin();  // safe state first: the trigger output must be LOW before anything else
  engineBegin();

  Serial.setRxBufferSize(1024);  // a pasted line must survive a busy loop
  Serial.begin(115200);
  delay(2000);  // give USB-CDC a moment so early prints are not lost
  esp_log_level_set("*", ESP_LOG_WARN);   // show warnings and errors from IDF components (httpd, TLS, modem)
  logBegin();  // from here on Log() is safe to call from several tasks
  Log(INFO, "Booting...");
  linkWatchBegin();
  flashLogBegin();
  settingsLoad();
  vstateBegin();
  {
    // Why did the chip last restart? A brownout means the supply dipped (for example while the starter cranks), a
    // panic is a firmware crash, a watchdog is a hang.
    esp_reset_reason_t r = esp_reset_reason();
    bool bad = r == ESP_RST_BROWNOUT || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    Logf(bad ? WARN : INFO, "Last reset: %s", resetReasonText());
  }

  outputsBegin();
  authBegin();
  actionsBegin();
  engineStartTask();
  cellBegin();
  consoleBegin();

  // If loop() ever stops running (a hang), the chip restarts itself after 60 s instead of staying dead. Generous,
  // because starting the tunnel can block the loop for about ten seconds.
  esp_task_wdt_config_t wdt = {.timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  enableLoopWDT();
}

void loop() {
  timePoll();
  wgPoll();
  linkWatchPoll();
  flashLogPoll();
  apiPoll();
  consolePoll();
}
