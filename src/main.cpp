#include <Arduino.h>
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

  Serial.begin(115200);
  delay(2000);  // give USB-CDC a moment so early prints are not lost
  Log(INFO, "Booting...");

  outputsBegin();
  wifiBegin();
  consoleBegin();
}

void loop() {
  pollEngine();
  wifiPoll();
  wgPoll();
  consolePoll();
}
