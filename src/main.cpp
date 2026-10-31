#include <Arduino.h>
#include <esp_log.h>
#include <esp_system.h>
#include "actions.h"
#include "api.h"
#include "auth.h"
#include "console.h"
#include "engine.h"
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
  settingsLoad();
  vstateBegin();
  {
    // Why did the chip last restart? A brownout means the supply voltage dipped (for example while
    // the starter cranks); a panic is a firmware crash; a watchdog is a task that stopped responding.
    esp_reset_reason_t r = esp_reset_reason();
    const char* why = r == ESP_RST_POWERON ? "power-on" : r == ESP_RST_SW ? "software restart" :
                      r == ESP_RST_PANIC ? "CRASH (panic)" : r == ESP_RST_BROWNOUT ? "BROWNOUT (supply dipped)" :
                      (r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) ? "watchdog" :
                      r == ESP_RST_EXT ? "reset pin" : "other";
    Logf(r == ESP_RST_BROWNOUT || r == ESP_RST_PANIC ? WARN : INFO, "Last reset: %s", why);
  }

  outputsBegin();
  authBegin();
  actionsBegin();
  engineStartTask();
  cellBegin();
  consoleBegin();
}

void loop() {
  timePoll();
  wgPoll();
  apiPoll();
  consolePoll();
}
