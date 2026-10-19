#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <stdarg.h>

// --- Pins (native ESP32-S3 GPIO) -------------------------------------------
constexpr int PIN_I2C_SDA = 8;
constexpr int PIN_I2C_SCL = 9;

// Remote-start trigger: drives a PC817 that pulls the iDatastart trigger wire to
// ground. Native GPIO on purpose, not the expander, so it stays independent of I2C.
// TODO: confirm this pin is free and broken out on your board.
constexpr int RS_TRIGGER_PIN = 14;

// Engine-running input: the iDatastart GWR output (M2-06 BLUE/WHITE) grounds this wire
// while the engine runs and leaves it floating otherwise, so we use the internal
// pull-up and read LOW as "running". Add a ~1k series resistor in the wiring.
// TODO: confirm this pin is free and broken out on your board.
constexpr int ENGINE_RUNNING_PIN = 21;

// --- MCP23017 output channels (expander pin numbers 0-15) ------------------
constexpr uint8_t MCP_ADDR = 0x27;

constexpr uint8_t TEMP_UP_PIN = 0;
constexpr uint8_t TEMP_DOWN_PIN = 1;
constexpr uint8_t DRIVER_SEAT_HEATER_PIN = 2;
constexpr uint8_t PASSENGER_SEAT_HEATER_PIN = 3;
constexpr uint8_t FRONT_DEFROST_PIN = 4;
constexpr uint8_t REAR_DEFROST_PIN = 5;
constexpr uint8_t FAN_UP_PIN = 6;        // GPA6
constexpr uint8_t FAN_DOWN_PIN = 7;      // GPA7
constexpr uint8_t CLIMATE_POWER_PIN = 8; // GPB0 (library pins 8-15 are bank B)

Adafruit_MCP23X17 mcp;

bool expanderOk = false;

// Engine state (updated by pollEngine, see "Engine running status")
bool engineRunning = false;
bool engineCandidate = false;       // most recent raw reading
uint32_t engineCandidateSince = 0;  // when the raw reading last changed
uint32_t engineChangedAt = 0;       // when engineRunning last changed

enum LogTypes {INFO, DEBUG, WARN, ERROR};
void Log(LogTypes type, const char* message) {
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
}

// printf-style wrapper around Log (formats into a small stack buffer, no heap).
void Logf(LogTypes type, const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Log(type, buf);
}

// --- Runtime settings (changeable from the serial console) ------------------
constexpr uint32_t HOLD_MS_MAX = 500;  // fail-safe: an output is never held longer
constexpr uint32_t RS_PULSE_MS_MAX = 2000;

struct Settings {
  uint32_t holdMs = 150;      // how long a button is "held"
  uint32_t gapMs = 150;       // pause between presses
  int tempMinF = 60;          // dial value when clamped at LO (unmeasured default)
  int tempMaxF = 84;          // dial value when clamped at HI (unmeasured default)
  int clampExtra = 2;         // extra down presses beyond the range, to be sure of LO
  uint32_t rsPulseMs = 300;   // remote-start trigger pulse width (module spec unknown, tune it)
  uint32_t rsGapMs = 300;     // gap between pulses when sending more than one
  int rsPulses = 2;           // 2 = the module's "double pulse" setting (1-3)
  uint32_t rsConfirmMs = 20000;  // how long to wait for the engine to report running
  int seatHeaterStages = 3;    // number of stages on the seat heater button (ex: 3 means the button can be pressed 3 times before looping around to 0)
};
Settings cfg;

// --- Button driver ------------------------------------------------------------
// Blocking for now: one press = HIGH for holdMs, LOW, then wait gapMs.
void pressButton(uint8_t pin) {
  mcp.digitalWrite(pin, HIGH);
  delay(cfg.holdMs);
  mcp.digitalWrite(pin, LOW);
  delay(cfg.gapMs);
}

void pressButton(uint8_t pin, int times) {
  for (int i = 0; i < times; i++) pressButton(pin);
}

void pressButton(uint8_t pin, int times, void (*actionFunction)()) {
  for (int i = 0; i < times; i++)
  {
    pressButton(pin);
    actionFunction();
  }
}

// --- Command handlers -----------------------------------------------------------
// Parse a whole-number argument; false if it is missing or not a number.
bool parseInt(const char* text, long& out) {
  if (!text || !*text) return false;
  char* end;
  out = strtol(text, &end, 10);
  return *end == '\0';
}

void printHelp(int argc, char** argv);  // defined after the command table

void cmdHold(int argc, char** argv) {
  long v;
  if (argc < 2 || !parseInt(argv[1], v) || v < 10 || v > (long)HOLD_MS_MAX) {
    Logf(WARN, "Usage: hold <10-%lu>", (unsigned long)HOLD_MS_MAX);
    return;
  }
  cfg.holdMs = v;
  Logf(INFO, "hold = %ld ms", v);
}

void cmdGap(int argc, char** argv) {
  long v;
  if (argc < 2 || !parseInt(argv[1], v) || v < 0 || v > 5000) {
    Log(WARN, "Usage: gap <0-5000>");
    return;
  }
  cfg.gapMs = v;
  Logf(INFO, "gap = %ld ms", v);
}

void cmdRange(int argc, char** argv) {
  long lo, hi;
  if (argc < 3 || !parseInt(argv[1], lo) || !parseInt(argv[2], hi) || lo >= hi) {
    Log(WARN, "Usage: range <min> <max>   (min must be below max)");
    return;
  }
  cfg.tempMinF = lo;
  cfg.tempMaxF = hi;
  Logf(INFO, "temp range = %ld-%ld F", lo, hi);
}

void cmdClamp(int argc, char** argv) {
  long v;
  if (argc < 2 || !parseInt(argv[1], v) || v < 0 || v > 10) {
    Log(WARN, "Usage: clamp <0-10>");
    return;
  }
  cfg.clampExtra = v;
  Logf(INFO, "clampExtra = %ld", v);
}

// Shared by "up" and "down": press the given pin n times (default 1).
void pressCommand(const char* name, uint8_t pin, int argc, char** argv) {
  if (!expanderOk) {
    Log(ERROR, "Expander is missing. Refused");
    return;
  }
  long n = 1;
  if (argc >= 2 && (!parseInt(argv[1], n) || n < 1 || n > 50)) {
    Logf(WARN, "Usage: %s [1-50]", name);
    return;
  }
  Logf(INFO, "%s x%ld", name, n);
  pressButton(pin, (int)n);
}

void cmdUp(int argc, char** argv) { pressCommand("up", TEMP_UP_PIN, argc, argv); }
void cmdDown(int argc, char** argv) { pressCommand("down", TEMP_DOWN_PIN, argc, argv); }

// Absolute temperature: press down enough to clamp at LO, then count up.
void cmdTemp(int argc, char** argv) {
  if (!expanderOk) {
    Log(ERROR, "Expander is missing. Refused");
    return;
  }
  long target;
  if (argc < 2 || !parseInt(argv[1], target) || target < cfg.tempMinF || target > cfg.tempMaxF) {
    Logf(WARN, "Usage: temp <%d-%d>", cfg.tempMinF, cfg.tempMaxF);
    return;
  }
  int downs = (cfg.tempMaxF - cfg.tempMinF) + cfg.clampExtra;
  int ups = target - cfg.tempMinF;
  Logf(INFO, "Temp %ld F: %d down, then %d up", target, downs, ups);
  pressButton(TEMP_DOWN_PIN, downs);
  pressButton(TEMP_UP_PIN, ups);
  Logf(INFO, "Temp set to %d", target);
}


// Seat heater state is tracked in software: the car gives no feedback, so we count
// presses since "off". Each press steps down one level and the cycle is stages+1 presses:
//   press 1 = high, press 2 = medium, press 3 = low, press 4 = off (back to the start).
// So the stage number is both the press count and the level: 0 off, 1 high, 2 medium, 3 low.
int driverHeaterStage = 0;
int passengerHeaterStage = 0;

// Press the button as many times as needed to land on targetStage.
void setSeatHeater(uint8_t pin, int& stage, int targetStage) {
  int cycle = cfg.seatHeaterStages + 1;
  int presses = (targetStage - stage + cycle) % cycle;
  Logf(INFO, "Seat heater: stage %d -> %d (%d presses)", stage, targetStage, presses);
  pressButton(pin, presses);
  stage = targetStage;
}

// heater <driver|passenger> <high|medium|low|off>   ("on" is the same as "high")
void cmdHeater(int argc, char** argv) {
  if (!expanderOk) {
    Log(ERROR, "Expander is missing. Refused");
    return;
  }
  bool driver = (argc >= 3 && strcasecmp(argv[1], "driver") == 0);
  bool passenger = (argc >= 3 && strcasecmp(argv[1], "passenger") == 0);

  int target = -1;
  if (argc >= 3) {
    if (strcasecmp(argv[2], "off") == 0) target = 0;
    else if (strcasecmp(argv[2], "high") == 0 || strcasecmp(argv[2], "on") == 0) target = 1;
    else if (strcasecmp(argv[2], "medium") == 0) target = 2;
    else if (strcasecmp(argv[2], "low") == 0) target = 3;
  }
  if (!(driver || passenger) || target < 0 || target > cfg.seatHeaterStages) {
    Log(WARN, "Usage: heater <driver|passenger> <high|medium|low|off>");
    return;
  }

  if (driver) {
    setSeatHeater(DRIVER_SEAT_HEATER_PIN, driverHeaterStage, target);
  } else {
    setSeatHeater(PASSENGER_SEAT_HEATER_PIN, passengerHeaterStage, target);
  }
}

// Defrost button control
// Each defrost button toggles, and the car gives no feedback, so we track the state.
bool frontDefrostOn = false;
bool rearDefrostOn = false;

// Press the button only if the tracked state differs from what was asked for.
void setDefrost(const char* name, uint8_t pin, bool& state, bool wantOn) {
  const char* word = wantOn ? "on" : "off";
  if (state == wantOn) {
    Logf(INFO, "%s defrost is already %s.", name, word);
    return;
  }
  pressButton(pin);
  state = wantOn;
  Logf(INFO, "%s defrost turned %s", name, word);
}

// defrost <front|rear> <on|off>
void cmdDefrost(int argc, char** argv) {
  if (!expanderOk) {
    Log(ERROR, "Expander is missing. Refused");
    return;
  }
  bool front = (argc >= 3 && strcasecmp(argv[1], "front") == 0);
  bool rear = (argc >= 3 && strcasecmp(argv[1], "rear") == 0);
  bool on = (argc >= 3 && strcasecmp(argv[2], "on") == 0);
  bool off = (argc >= 3 && strcasecmp(argv[2], "off") == 0);

  if (!(front || rear) || !(on || off)) {
    Log(WARN, "Usage: defrost <front|rear> <on|off>");
    return;
  }

  if (front) {
    setDefrost("Front", FRONT_DEFROST_PIN, frontDefrostOn, on);
  } else {
    setDefrost("Rear", REAR_DEFROST_PIN, rearDefrostOn, on);
  }
}

// Fan speed: plain up/down buttons, same idea as the temperature dial.
// fan <up|down> [n]
void cmdFan(int argc, char** argv) {
  if (argc < 2 || (strcasecmp(argv[1], "up") != 0 && strcasecmp(argv[1], "down") != 0)) {
    Log(WARN, "Usage: fan <up|down> [1-50]");
    return;
  }
  bool up = (strcasecmp(argv[1], "up") == 0);
  // pressCommand expects the optional count as argv[1], so pass the shifted arguments.
  pressCommand(up ? "fan up" : "fan down", up ? FAN_UP_PIN : FAN_DOWN_PIN, argc - 1, argv + 1);
}

// Climate power button: a momentary switch, one press toggles the climate system.
// No state is tracked here; once CAN/LIN feedback exists the real state can be read.
void cmdPower(int argc, char** argv) {
  if (!expanderOk) {
    Log(ERROR, "Expander is missing. Refused");
    return;
  }
  Log(INFO, "Climate power button pressed");
  pressButton(CLIMATE_POWER_PIN);
}

// --- Remote start trigger ---------------------------------------------------------
// HIGH turns the optocoupler on, which pulls the module's trigger wire to ground.
void pollEngine();  // defined in "Engine running status" below

// Returns true only once the engine input confirms the engine is running.
bool sendRemoteStart() {
  // The module may treat another pulse as a stop request, so never pulse a running engine.
  if (engineRunning) {
    Log(WARN, "Engine is already running, not sending a start pulse");
    return true;
  }

  Logf(INFO, "Remote start: %d pulse(s), %lu ms on, %lu ms gap", cfg.rsPulses,
       (unsigned long)cfg.rsPulseMs, (unsigned long)cfg.rsGapMs);
  for (int i = 0; i < cfg.rsPulses; i++) {
    digitalWrite(RS_TRIGGER_PIN, HIGH);
    delay(cfg.rsPulseMs);
    digitalWrite(RS_TRIGGER_PIN, LOW);
    if (i < cfg.rsPulses - 1) delay(cfg.rsGapMs);
  }
  Logf(INFO, "Trigger sent, waiting up to %lu s for the engine...",
       (unsigned long)(cfg.rsConfirmMs / 1000));

  // Wait for the debounced engine input to go true, or give up.
  uint32_t startedAt = millis();
  while (millis() - startedAt < cfg.rsConfirmMs) {
    pollEngine();
    if (engineRunning) {
      Logf(INFO, "Remote start CONFIRMED after %lu ms", (unsigned long)(millis() - startedAt));
      return true;
    }
    delay(10);
  }
  Logf(ERROR, "Remote start NOT confirmed within %lu s. Check the module's parking-light error code.",
       (unsigned long)(cfg.rsConfirmMs / 1000));
  return false;
}

void cmdStart(int argc, char** argv) {
  sendRemoteStart();
}

// rswait <seconds>
void cmdRsWait(int argc, char** argv) {
  long v;
  if (argc < 2 || !parseInt(argv[1], v) || v < 1 || v > 120) {
    Log(WARN, "Usage: rswait <1-120 seconds>");
    return;
  }
  cfg.rsConfirmMs = (uint32_t)v * 1000;
  Logf(INFO, "Remote start confirm wait = %ld s", v);
}

// rscfg <pulseMs> <gapMs> <pulses>
void cmdRsCfg(int argc, char** argv) {
  long pulse, gap, count;
  if (argc < 4 || !parseInt(argv[1], pulse) || !parseInt(argv[2], gap) || !parseInt(argv[3], count) ||
      pulse < 20 || pulse > (long)RS_PULSE_MS_MAX || gap < 0 || gap > 5000 || count < 1 || count > 3) {
    Logf(WARN, "Usage: rscfg <pulse 20-%lu> <gap 0-5000> <pulses 1-3>", (unsigned long)RS_PULSE_MS_MAX);
    return;
  }
  cfg.rsPulseMs = pulse;
  cfg.rsGapMs = gap;
  cfg.rsPulses = count;
  Logf(INFO, "Remote start: %ld pulse(s), %ld ms on, %ld ms gap", count, pulse, gap);
}

// --- Engine running status ----------------------------------------------------------
constexpr uint32_t ENGINE_DEBOUNCE_MS = 200;  // reading must be stable this long to count


// GWR pulls the wire to ground while running, so LOW means running.
bool readEngineRaw() {
  return digitalRead(ENGINE_RUNNING_PIN) == LOW;
}

// Call every loop. Debounces the input and logs each change.
void pollEngine() {
  bool raw = readEngineRaw();
  uint32_t now = millis();
  if (raw != engineCandidate) {
    engineCandidate = raw;
    engineCandidateSince = now;
  }
  if (engineCandidate != engineRunning && now - engineCandidateSince >= ENGINE_DEBOUNCE_MS) {
    engineRunning = engineCandidate;
    engineChangedAt = now;
    Log(INFO, engineRunning ? "Engine running" : "Engine stopped");
  }
}

void cmdEngine(int argc, char** argv) {
  Logf(INFO, "Engine %s (%lu s since last change, raw pin %s)",
       engineRunning ? "RUNNING" : "stopped",
       (unsigned long)((millis() - engineChangedAt) / 1000),
       digitalRead(ENGINE_RUNNING_PIN) == LOW ? "LOW" : "HIGH");
}

// --- Bench test ------------------------------------------------------------------
// Holds ONE expander output ON for several seconds so it can be measured with a
// multimeter. This deliberately exceeds the normal 500 ms hold cap, so it is limited
// to BENCH_MAX_S, any keypress aborts it, and the pin is always driven LOW afterwards.
// The remote-start trigger is intentionally not included.
constexpr uint32_t BENCH_MAX_S = 30;

struct BenchOutput {
  const char* name;
  uint8_t pin;
};

const BenchOutput benchOutputs[] = {
  {"up", TEMP_UP_PIN},
  {"down", TEMP_DOWN_PIN},
  {"driver", DRIVER_SEAT_HEATER_PIN},
  {"passenger", PASSENGER_SEAT_HEATER_PIN},
  {"front", FRONT_DEFROST_PIN},
  {"rear", REAR_DEFROST_PIN},
  {"fanup", FAN_UP_PIN},
  {"fandown", FAN_DOWN_PIN},
  {"power", CLIMATE_POWER_PIN},
};

// bench <up|down|driver|passenger|front|rear> <seconds>
void cmdBench(int argc, char** argv) {
  if (!expanderOk) {
    Log(ERROR, "Expander is missing. Refused");
    return;
  }
  const BenchOutput* out = nullptr;
  long seconds;
  if (argc >= 3) {
    for (const BenchOutput& o : benchOutputs) {
      if (strcasecmp(argv[1], o.name) == 0) out = &o;
    }
  }
  if (!out || !parseInt(argv[2], seconds) || seconds < 1 || seconds > (long)BENCH_MAX_S) {
    Logf(WARN, "Usage: bench <up|down|driver|passenger|front|rear|fanup|fandown|power> <1-%lu s>", (unsigned long)BENCH_MAX_S);
    return;
  }

  Logf(INFO, "Bench: %s ON for %ld s. Measure now. Press any key to stop early.", out->name, seconds);
  // Throw away leftover input first (the "\n" of the Enter that started this command),
  // otherwise it would count as the "any key" that stops the test immediately.
  delay(50);
  while (Serial.available()) Serial.read();

  mcp.digitalWrite(out->pin, HIGH);
  uint32_t end = millis() + (uint32_t)seconds * 1000;
  while (millis() < end && !Serial.available()) {
    delay(10);
  }
  mcp.digitalWrite(out->pin, LOW);
  while (Serial.available()) Serial.read();  // discard the key that stopped it
  Log(INFO, "Bench: output OFF");
}

// --- Command table ---------------------------------------------------------------
// Add a row here and it shows up in "help" and at boot automatically.
struct Command {
  const char* name;
  const char* Usage;
  const char* help;
  void (*handler)(int argc, char** argv);
};

const Command commands[] = {
  {"help",   "help",                          "list all commands",                            printHelp},
  {"hold",   "hold <ms>",                     "set button hold time (10-500 ms)",             cmdHold},
  {"gap",    "gap <ms>",                      "set pause between presses (0-5000 ms)",        cmdGap},
  {"range",  "range <min> <max>",             "set the dial's LO/HI temperature in F",       cmdRange},
  {"clamp",  "clamp <n>",                     "extra down presses used to reach LO (0-10)",   cmdClamp},
  {"up",     "up [n]",                        "press temp up n times (default 1)",            cmdUp},
  {"down",   "down [n]",                      "press temp down n times (default 1)",          cmdDown},
  {"temp",   "temp <F>",                      "set absolute temperature (clamp LO, count up)", cmdTemp},
  {"heater", "heater <seat> <level>", "seat heater: driver|passenger, high|medium|low|off", cmdHeater},
  {"defrost", "defrost <front|rear> <on|off>", "defrost: front or rear, on or off", cmdDefrost},
  {"fan",    "fan <up|down> [n]",             "press fan speed up or down n times",           cmdFan},
  {"power",  "power",                         "press the climate power button once (toggles)", cmdPower},
  {"rswait", "rswait <sec>",                  "set how long start waits to confirm the engine", cmdRsWait},
  {"bench",  "bench <name> <sec>",            "hold one output ON for metering (1-30 s)",     cmdBench},
  {"engine", "engine",                        "show engine running status (GWR input)",       cmdEngine},
  {"start",  "start",                         "send the remote-start trigger pulse(s)",       cmdStart},
  {"rscfg",  "rscfg <ms> <gap> <n>",          "set trigger pulse width, gap and pulse count", cmdRsCfg},
};
constexpr size_t COMMAND_COUNT = sizeof(commands) / sizeof(commands[0]);

void printHelp(int argc, char** argv) {
  Serial.println("--- Commands ---");
  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    Serial.printf("  %-20s %s\n", commands[i].Usage, commands[i].help);
  }
}

// --- Serial line reader ------------------------------------------------------------
void printPrompt() {
  Serial.print("> ");
}

void runLine(char* line) {
  char* argv[4];
  int argc = 0;
  for (char* tok = strtok(line, " \t"); tok && argc < 4; tok = strtok(nullptr, " \t")) {
    argv[argc++] = tok;
  }
  if (argc == 0) return;

  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    if (strcasecmp(argv[0], commands[i].name) == 0) {
      commands[i].handler(argc, argv);
      return;
    }
  }
  Logf(WARN, "Unknown command '%s' (type help)", argv[0]);
}

void pollSerial() {
  static char line[64];
  static size_t len = 0;
  static char prev = 0;  // previous byte, to treat "\r\n" as a single Enter
  while (Serial.available()) {
    char c = Serial.read();
    bool secondOfCrLf = (c == '\n' && prev == '\r');
    prev = c;
    if (secondOfCrLf) {
      continue;
    }
    if (c == '\r' || c == '\n') {
      Serial.println();  // echo the Enter key
      line[len] = '\0';
      len = 0;
      runLine(line);
      printPrompt();  // ready for the next command
    } else if (c == '\b' || c == 0x7F) {
      if (len > 0) {
        len--;
        Serial.print("\b \b");  // step back, blank the character, step back
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
      Serial.write(c);  // echo what was actually stored
    }
  }
}

void setup() {
  // Safe state first: the trigger output must be LOW before anything else runs.
  pinMode(RS_TRIGGER_PIN, OUTPUT);
  digitalWrite(RS_TRIGGER_PIN, LOW);

  // Engine input: floats high via the pull-up, module grounds it while running.
  pinMode(ENGINE_RUNNING_PIN, INPUT_PULLUP);
  engineRunning = engineCandidate = readEngineRaw();

  Serial.begin(115200);
  delay(2000);  // give USB-CDC a moment so early prints are not lost
  Log(INFO, "Booting...");

  // Initialize the I2C communication
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  // Initialize the expander
  expanderOk = mcp.begin_I2C(MCP_ADDR, &Wire);

  if (expanderOk) {
    // Outputs start LOW so no optocoupler is ever driven at boot.
    for (uint8_t pin : {
      TEMP_UP_PIN, 
      TEMP_DOWN_PIN, 
      DRIVER_SEAT_HEATER_PIN, 
      PASSENGER_SEAT_HEATER_PIN, 
      FRONT_DEFROST_PIN, 
      REAR_DEFROST_PIN,
      FAN_UP_PIN,
      FAN_DOWN_PIN,
      CLIMATE_POWER_PIN
    }) {
      mcp.pinMode(pin, OUTPUT);
      mcp.digitalWrite(pin, LOW);
    }
    Log(INFO, "MCP23017 intialized.");
  } else {
    Log(ERROR, "MCP23017 not found. Outputs have been disabled.");
  }

  printHelp(0, nullptr);
  printPrompt();
}

void loop() {
  pollEngine();
  pollSerial();
}
