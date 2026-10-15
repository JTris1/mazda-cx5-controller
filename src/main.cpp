#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <stdarg.h>

// --- Pins (native ESP32-S3 GPIO) -------------------------------------------
// TODO: confirm these against your board's header before wiring.
constexpr int PIN_I2C_SDA = 8;
constexpr int PIN_I2C_SCL = 9;

// --- MCP23017 output channels (expander pin numbers 0-15) ------------------
constexpr uint8_t MCP_ADDR = 0x27;

constexpr uint8_t TEMP_UP_PIN = 0;
constexpr uint8_t TEMP_DOWN_PIN = 1;
constexpr uint8_t DRIVER_SEAT_HEATER_PIN = 2;
constexpr uint8_t PASSENGER_SEAT_HEATER_PIN = 3;
constexpr uint8_t DEFROST_PIN = 4;

Adafruit_MCP23X17 mcp;

bool expanderOk = false;

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

struct Settings {
  uint32_t holdMs = 150;      // how long a button is "held"
  uint32_t gapMs = 150;       // pause between presses
  int tempMinF = 60;          // dial value when clamped at LO (unmeasured default)
  int tempMaxF = 84;          // dial value when clamped at HI (unmeasured default)
  int clampExtra = 2;         // extra down presses beyond the range, to be sure of LO
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

void cmdStatus(int argc, char** argv) {
  Logf(INFO, "expander: %s", expanderOk ? "ok" : "MISSING");
  Logf(INFO, "hold=%lu ms  gap=%lu ms", (unsigned long)cfg.holdMs, (unsigned long)cfg.gapMs);
  Logf(INFO, "temp range=%d-%d F  clampExtra=%d", cfg.tempMinF, cfg.tempMaxF, cfg.clampExtra);
}

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
// presses. One cycle is stages+1 presses (off, 1, 2, ... stages, back to off).
int driverHeaterStage = 0;
int passengerHeaterStage = 0;

// Press the button as many times as needed to land on targetStage
// (0 = off, cfg.seatHeaterStages = highest).
void setSeatHeater(uint8_t pin, int& stage, int targetStage) {
  int cycle = cfg.seatHeaterStages + 1;
  int presses = (targetStage - stage + cycle) % cycle;
  Logf(INFO, "Seat heater: stage %d -> %d (%d presses)", stage, targetStage, presses);
  pressButton(pin, presses);
  stage = targetStage;
}

// heater <driver|passenger> <on|off>
void cmdHeater(int argc, char** argv) {
  if (!expanderOk) {
    Log(ERROR, "Expander is missing. Refused");
    return;
  }
  bool driver = (argc >= 3 && strcasecmp(argv[1], "driver") == 0);
  bool passenger = (argc >= 3 && strcasecmp(argv[1], "passenger") == 0);
  bool on = (argc >= 3 && strcasecmp(argv[2], "on") == 0);
  bool off = (argc >= 3 && strcasecmp(argv[2], "off") == 0);
  if (!(driver || passenger) || !(on || off)) {
    Log(WARN, "Usage: heater <driver|passenger> <on|off>");
    return;
  }

  int target = on ? cfg.seatHeaterStages : 0;
  if (driver) {
    setSeatHeater(DRIVER_SEAT_HEATER_PIN, driverHeaterStage, target);
  } else {
    setSeatHeater(PASSENGER_SEAT_HEATER_PIN, passengerHeaterStage, target);
  }
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
  {"status", "status",                        "show expander state and current settings",     cmdStatus},
  {"hold",   "hold <ms>",                     "set button hold time (10-500 ms)",             cmdHold},
  {"gap",    "gap <ms>",                      "set pause between presses (0-5000 ms)",        cmdGap},
  {"range",  "range <min> <max>",             "set the dial's LO/HI temperature in F",       cmdRange},
  {"clamp",  "clamp <n>",                     "extra down presses used to reach LO (0-10)",   cmdClamp},
  {"up",     "up [n]",                        "press temp up n times (default 1)",            cmdUp},
  {"down",   "down [n]",                      "press temp down n times (default 1)",          cmdDown},
  {"temp",   "temp <F>",                      "set absolute temperature (clamp LO, count up)", cmdTemp},
  {"heater", "heater <seat> <on|off>", "seat heater: driver or passenger, on or off", cmdHeater},
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
  Serial.begin(115200);
  delay(2000);  // give USB-CDC a moment so early prints are not lost
  Log(INFO, "Booting...");

  // Initialize the I2C communication
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  // Initialize the expander
  expanderOk = mcp.begin_I2C(MCP_ADDR, &Wire);

  if (expanderOk) {
    // Outputs start LOW so no optocoupler is ever driven at boot.
    for (uint8_t pin : {TEMP_UP_PIN, TEMP_DOWN_PIN}) {
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
  pollSerial();
}
