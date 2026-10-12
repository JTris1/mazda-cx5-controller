// CX-5 climate control: phase 1 (bench test).
//
// Pulses PC817 optocouplers (driven from an MCP23017 expander) that bridge the CCU
// switch pads. Commands come from local buttons or the USB serial console. The same
// enqueue*() entry points are what the SMS/MQTT layer will call in later phases.
//
// Core layout (per project requirements): output pulsing runs on core 1 in its own
// task; networking will be pinned to core 0 later.

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>

#include "config.h"
#include "mcp23017.h"
#include "pins.h"

enum class Channel : uint8_t {
  TempUp,
  TempDown,
  SeatDriver,
  SeatPassenger,
  FrontDefrost,
  RearDefrost,
  Count
};

struct ChannelInfo {
  const char *name;
  uint8_t mcpPin;
  uint8_t maxCount;
};

static const ChannelInfo CHANNELS[] = {
    {"temp_up",        pins::MCP_TEMP_UP,       MAX_TEMP_STEPS},
    {"temp_down",      pins::MCP_TEMP_DOWN,     MAX_TEMP_STEPS},
    {"seat_driver",    pins::MCP_SEAT_DRIVER,   MAX_SEAT_PRESSES},
    {"seat_passenger", pins::MCP_SEAT_PASS,     MAX_SEAT_PRESSES},
    {"front_defrost",  pins::MCP_FRONT_DEFROST, MAX_DEFROST_PRESSES},
    {"rear_defrost",   pins::MCP_REAR_DEFROST,  MAX_DEFROST_PRESSES},
};
static_assert(sizeof(CHANNELS) / sizeof(CHANNELS[0]) == (size_t)Channel::Count,
              "channel table out of sync with enum");

static const ChannelInfo &info(Channel c) { return CHANNELS[(size_t)c]; }

enum class Kind : uint8_t { Press, SetTemp };

constexpr int16_t TEMP_LO = -1;   // home to the LO clamp only
constexpr int16_t TEMP_HI = -2;   // press up until clamped at HI

struct Command {
  Kind kind;
  Channel ch;        // Press only
  uint8_t count;     // Press only
  int16_t targetF;   // SetTemp only (degrees F, or TEMP_LO / TEMP_HI)
};

static QueueHandle_t cmdQueue;
static Preferences prefs;
static Mcp23017 expander;
static volatile bool abortReq = false;

static volatile uint16_t pressMs = DEFAULT_PRESS_MS;
static volatile uint16_t gapMs = DEFAULT_GAP_MS;
static volatile int16_t tempMinF = DEFAULT_TEMP_MIN_F;
static volatile int16_t tempMaxF = DEFAULT_TEMP_MAX_F;
static volatile uint8_t tempSteps = DEFAULT_TEMP_STEPS;
static volatile uint8_t tempLoOffset = DEFAULT_TEMP_LO_OFFSET;

// ---- command intake --------------------------------------------------------

static bool queueCommand(const Command &c) {
  if (!expander.ready()) {
    Serial.println("! MCP23017 not found, refusing command (check wiring, `status`)");
    return false;
  }
  if (xQueueSend(cmdQueue, &c, 0) != pdTRUE) {
    Serial.println("! queue full, command dropped");
    return false;
  }
  return true;
}

// Thread-safe entry point for every command source (buttons, serial, later MQTT/SMS).
static bool enqueuePress(Channel ch, uint8_t count) {
  if (count == 0) return false;
  if (count > info(ch).maxCount) count = info(ch).maxCount;
  const Command c{Kind::Press, ch, count, 0};
  if (!queueCommand(c)) return false;
  Serial.printf("> %s x%u queued\n", info(ch).name, count);
  return true;
}

static bool enqueueTemp(int16_t targetF) {
  const Command c{Kind::SetTemp, Channel::TempUp, 0, targetF};
  if (!queueCommand(c)) return false;
  if (targetF == TEMP_LO) Serial.println("> temp LO queued");
  else if (targetF == TEMP_HI) Serial.println("> temp HI queued");
  else Serial.printf("> temp %dF queued\n", targetF);
  return true;
}

// ---- output task (core 1): the only code that ever touches the expander ------

static bool pulse(Channel ch) {
  const uint8_t pin = info(ch).mcpPin;
  if (!expander.set(pin, true)) {
    expander.allLow();
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(pressMs));
  bool low = expander.set(pin, false);
  for (int i = 0; i < 5 && !low; i++) {   // never leave a "button" held down
    vTaskDelay(pdMS_TO_TICKS(10));
    low = expander.allLow();
  }
  return low;
}

static bool pressSeq(Channel ch, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    if (abortReq) return false;
    if (!pulse(ch)) {
      Serial.println("! I2C write failed, sequence aborted");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(gapMs));
  }
  return true;
}

static uint8_t homePresses() {
  const uint16_t n = (uint16_t)tempLoOffset + tempSteps + TEMP_HOME_MARGIN;
  return n > MAX_TEMP_STEPS ? MAX_TEMP_STEPS : (uint8_t)n;
}

static uint8_t stepsFromLo(int16_t f) {
  const int minF = tempMinF, maxF = tempMaxF;
  f = constrain(f, minF, maxF);
  const long span = maxF - minF;
  const long n = span > 0 ? ((long)(f - minF) * tempSteps + span / 2) / span : 0;
  return (uint8_t)(tempLoOffset + n);
}

static bool runSetTemp(int16_t target) {
  if (target == TEMP_HI) return pressSeq(Channel::TempUp, homePresses());
  // Absolute: slam to LO (dial clamps), then count up from there.
  if (!pressSeq(Channel::TempDown, homePresses())) return false;
  if (target == TEMP_LO) return true;
  vTaskDelay(pdMS_TO_TICKS(TEMP_SETTLE_MS));
  return pressSeq(Channel::TempUp, stepsFromLo(target));
}

static void outputTask(void *) {
  Command c;
  for (;;) {
    if (xQueueReceive(cmdQueue, &c, portMAX_DELAY) != pdTRUE) continue;
    abortReq = false;
    bool ok;
    if (c.kind == Kind::Press) {
      ok = pressSeq(c.ch, c.count);
      Serial.printf("%s %s x%u %s\n", ok ? "<" : "!", info(c.ch).name, c.count,
                    ok ? "done" : "FAILED/aborted");
    } else {
      ok = runSetTemp(c.targetF);
      Serial.printf("%s temp %s\n", ok ? "<" : "!", ok ? "done" : "FAILED/aborted");
    }
  }
}

// ---- bench buttons ---------------------------------------------------------

struct Button {
  uint8_t pin;
  Channel ch;
  bool stable;          // debounced level (HIGH = released)
  bool lastRaw;
  uint32_t changedAt;
};

static Button buttons[] = {
    {pins::BTN_TEMP_UP,       Channel::TempUp,        true, true, 0},
    {pins::BTN_TEMP_DOWN,     Channel::TempDown,      true, true, 0},
    {pins::BTN_SEAT_DRIVER,   Channel::SeatDriver,    true, true, 0},
    {pins::BTN_SEAT_PASS,     Channel::SeatPassenger, true, true, 0},
    {pins::BTN_FRONT_DEFROST, Channel::FrontDefrost,  true, true, 0},
    {pins::BTN_REAR_DEFROST,  Channel::RearDefrost,   true, true, 0},
};

static void pollButtons() {
  const uint32_t now = millis();
  for (auto &b : buttons) {
    const bool raw = digitalRead(b.pin);
    if (raw != b.lastRaw) {
      b.lastRaw = raw;
      b.changedAt = now;
    } else if (raw != b.stable && now - b.changedAt >= DEBOUNCE_MS) {
      b.stable = raw;
      if (!raw) enqueuePress(b.ch, 1);   // act on press, not release
    }
  }
}

// ---- serial console --------------------------------------------------------

static float readBatteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(pins::VBAT_SENSE);
  const float pinV = (mv / 16.0f) / 1000.0f;
  return pinV * (VBAT_R1_OHMS + VBAT_R2_OHMS) / VBAT_R2_OHMS;
}

static void printStatus() {
  Serial.printf("expander: %s  press=%u ms  gap=%u ms  queued=%u\n",
                expander.ready() ? "ok" : "NOT FOUND", pressMs, gapMs,
                (unsigned)uxQueueMessagesWaiting(cmdQueue));
  Serial.printf("temp cal: %dF..%dF in %u steps, LO offset %u, home presses %u\n", tempMinF,
                tempMaxF, tempSteps, tempLoOffset, homePresses());
}

static void printHelp() {
  Serial.println(
      "commands:\n"
      "  temp <F>|lo|hi   absolute: home to LO, then count up (hi = press up until clamped)\n"
      "  up [n] / down [n]  relative temp steps\n"
      "  seatd [n] / seatp [n]    seat heater button presses\n"
      "  deff [n] / defr [n]      front / rear defrost button presses\n"
      "  tempcal <minF> <maxF> <steps> [loOffset]   calibrate dial (saved)\n"
      "  press <ms> / gap <ms>    pulse width / gap (saved)\n"
      "  stop             abort current sequence and clear the queue\n"
      "  vbat / status / help");
}

static void handleLine(String line) {
  line.trim();
  if (line.isEmpty()) return;

  String tok[5];
  int n = 0;
  while (n < 5 && line.length()) {
    const int sp = line.indexOf(' ');
    if (sp < 0) {
      tok[n++] = line;
      break;
    }
    tok[n++] = line.substring(0, sp);
    line = line.substring(sp + 1);
    line.trim();
  }
  tok[0].toLowerCase();
  const String &cmd = tok[0];
  const long arg = n > 1 ? tok[1].toInt() : 1;
  const uint8_t cnt = (uint8_t)constrain(arg, 1, 255);

  if (cmd == "up")         enqueuePress(Channel::TempUp, cnt);
  else if (cmd == "down")  enqueuePress(Channel::TempDown, cnt);
  else if (cmd == "seatd") enqueuePress(Channel::SeatDriver, cnt);
  else if (cmd == "seatp") enqueuePress(Channel::SeatPassenger, cnt);
  else if (cmd == "deff")  enqueuePress(Channel::FrontDefrost, cnt);
  else if (cmd == "defr")  enqueuePress(Channel::RearDefrost, cnt);
  else if (cmd == "temp") {
    String t = n > 1 ? tok[1] : "";
    t.toLowerCase();
    if (t == "lo") enqueueTemp(TEMP_LO);
    else if (t == "hi") enqueueTemp(TEMP_HI);
    else if (t.toInt() > 0) enqueueTemp((int16_t)t.toInt());
    else Serial.println("usage: temp <F>|lo|hi");
  } else if (cmd == "tempcal") {
    const long lo = n > 1 ? tok[1].toInt() : 0;
    const long hi = n > 2 ? tok[2].toInt() : 0;
    const long steps = n > 3 ? tok[3].toInt() : 0;
    const long off = n > 4 ? tok[4].toInt() : tempLoOffset;
    if (lo <= 0 || hi <= lo || steps <= 0 || steps > 100 || off < 0 || off > 10) {
      Serial.println("usage: tempcal <minF> <maxF> <steps minF->maxF> [loOffset]");
    } else {
      tempMinF = lo;
      tempMaxF = hi;
      tempSteps = steps;
      tempLoOffset = off;
      prefs.putShort("tmin", tempMinF);
      prefs.putShort("tmax", tempMaxF);
      prefs.putUChar("tsteps", tempSteps);
      prefs.putUChar("tlo", tempLoOffset);
      printStatus();
    }
  } else if (cmd == "press") {
    pressMs = constrain(arg, MIN_PRESS_MS, MAX_PRESS_MS);
    prefs.putUShort("press", pressMs);
    Serial.printf("press = %u ms\n", pressMs);
  } else if (cmd == "gap") {
    gapMs = constrain(arg, MIN_GAP_MS, MAX_GAP_MS);
    prefs.putUShort("gap", gapMs);
    Serial.printf("gap = %u ms\n", gapMs);
  } else if (cmd == "stop") {
    abortReq = true;
    xQueueReset(cmdQueue);
    Serial.println("stopped");
  } else if (cmd == "vbat") {
    Serial.printf("vbat = %.2f V (meaningless unless the divider is wired)\n", readBatteryVolts());
  } else if (cmd == "status") {
    printStatus();
  } else {
    printHelp();
  }
}

static void pollSerial() {
  static String line;
  while (Serial.available()) {
    const char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      handleLine(line);
      line = "";
    } else if (line.length() < 64) {
      line += ch;
    }
  }
}

// ---- setup / loop ----------------------------------------------------------

void setup() {
  for (auto &b : buttons) pinMode(b.pin, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(pins::VBAT_SENSE, ADC_11db);

  Serial.begin(115200);
  delay(300);

  prefs.begin("climate", false);
  pressMs = constrain(prefs.getUShort("press", DEFAULT_PRESS_MS), MIN_PRESS_MS, MAX_PRESS_MS);
  gapMs = constrain(prefs.getUShort("gap", DEFAULT_GAP_MS), MIN_GAP_MS, MAX_GAP_MS);
  tempMinF = prefs.getShort("tmin", DEFAULT_TEMP_MIN_F);
  tempMaxF = prefs.getShort("tmax", DEFAULT_TEMP_MAX_F);
  tempSteps = prefs.getUChar("tsteps", DEFAULT_TEMP_STEPS);
  tempLoOffset = prefs.getUChar("tlo", DEFAULT_TEMP_LO_OFFSET);

  Wire.begin(pins::I2C_SDA, pins::I2C_SCL, 400000);
  if (!expander.begin(Wire, pins::MCP_ADDR)) {
    Serial.printf("! MCP23017 not found at 0x%02X: outputs disabled\n", pins::MCP_ADDR);
  }

  cmdQueue = xQueueCreate(16, sizeof(Command));
  xTaskCreatePinnedToCore(outputTask, "output", 4096, nullptr, 3, nullptr, 1);

  Serial.println("\nCX-5 climate bench firmware ready");
  printStatus();
  printHelp();
}

void loop() {
  pollButtons();
  pollSerial();
  delay(5);
}
