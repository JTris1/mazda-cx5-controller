#include <Arduino.h>
#include <esp_log.h>
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

void setup() {
  remoteStartBegin();  // safe state first: the trigger output must be LOW before anything else
  engineBegin();

  Serial.setRxBufferSize(1024);  // a pasted line must survive a busy loop
  Serial.begin(115200);
  delay(2000);  // give USB-CDC a moment so early prints are not lost
  esp_log_level_set("*", ESP_LOG_WARN);   // show warnings and errors from IDF components (httpd, TLS, modem)
  logBegin();  // from here on Log() is safe to call from several tasks
  Log(INFO, "Booting...");

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
