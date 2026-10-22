#include <Arduino.h>
#include "actions.h"
#include "api.h"
#include "auth.h"
#include "console.h"
#include "engine.h"
#include "log.h"
#include "net_wg.h"
#include "net_wifi.h"
#include "outputs.h"
#include "remote_start.h"

void setup() {
  remoteStartBegin();  // safe state first: the trigger output must be LOW before anything else
  engineBegin();

  Serial.setRxBufferSize(1024);  // a pasted line must survive a busy loop
  Serial.begin(115200);
  delay(2000);  // give USB-CDC a moment so early prints are not lost
  logBegin();  // from here on Log() is safe to call from several tasks
  Log(INFO, "Booting...");

  outputsBegin();
  authBegin();
  actionsBegin();
  engineStartTask();
  wifiBegin();
  consoleBegin();
}

void loop() {
  wifiPoll();
  wgPoll();
  apiPoll();
  consolePoll();
}
