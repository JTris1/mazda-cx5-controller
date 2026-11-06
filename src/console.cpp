#include <Arduino.h>
#include "console.h"
#include "actions.h"
#include "api.h"
#include "auth.h"
#include "climate.h"
#include "config.h"
#include "engine.h"
#include "link_watch.h"
#include "log.h"
#include "net_wg.h"
#include "net_cell.h"
#include "outputs.h"
#include "remote_start.h"
#include "settings.h"
#include "util.h"

// --- Shared helpers ---------------------------------------------------------------
static bool requireExpander() {
  if (!expanderOk) {
    Log(ERROR, "Expander is missing. Refused");
    return false;
  }
  return true;
}

// Waits for a queued action to finish. The worker task prints the details, so the console
// stays synchronous: the prompt only comes back once the buttons have been pressed.
static void finish(Submit result, uint32_t id) {
  if (result != Submit::Ok) {
    Logf(result == Submit::NoExpander ? ERROR : WARN, "%s", submitText(result));
    return;
  }
  JobInfo info;
  if (!actionsWait(id, 120000, info)) Log(ERROR, "Timed out waiting for the action");
}

// Optional repeat count in argv[1] (default 1, range 1-50). Prints the usage line on error.
static bool parseCount(const char* name, int argc, char** argv, int& n) {
  long v = 1;
  if (argc >= 2 && (!parseInt(argv[1], v) || v < 1 || v > 50)) {
    Logf(WARN, "Usage: %s [1-50]", name);
    return false;
  }
  n = (int)v;
  return true;
}

static void printHelp(int argc, char** argv);  // defined after the command table

// --- Settings commands ------------------------------------------------------------
static void cmdHold(int argc, char** argv) {
  long v;
  if (argc < 2 || !parseInt(argv[1], v) || v < 10 || v > (long)HOLD_MS_MAX) {
    Logf(WARN, "Usage: hold <10-%lu>", (unsigned long)HOLD_MS_MAX);
    return;
  }
  cfg.holdMs = v;
  settingsSave();
  Logf(INFO, "hold = %ld ms", v);
}

static void cmdGap(int argc, char** argv) {
  long v;
  if (argc < 2 || !parseInt(argv[1], v) || v < 0 || v > 5000) {
    Log(WARN, "Usage: gap <0-5000>");
    return;
  }
  cfg.gapMs = v;
  settingsSave();
  Logf(INFO, "gap = %ld ms", v);
}

static void cmdRange(int argc, char** argv) {
  long lo, hi;
  if (argc < 3 || !parseInt(argv[1], lo) || !parseInt(argv[2], hi) || lo >= hi) {
    Log(WARN, "Usage: range <min> <max>   (min must be below max)");
    return;
  }
  cfg.tempMinF = lo;
  cfg.tempMaxF = hi;
  settingsSave();
  Logf(INFO, "temp range = %ld-%ld F", lo, hi);
}

static void cmdClamp(int argc, char** argv) {
  long v;
  if (argc < 2 || !parseInt(argv[1], v) || v < 0 || v > 10) {
    Log(WARN, "Usage: clamp <0-10>");
    return;
  }
  cfg.clampExtra = v;
  settingsSave();
  Logf(INFO, "clampExtra = %ld", v);
}

// --- Climate commands -------------------------------------------------------------
static void cmdUp(int argc, char** argv) {
  int n;
  if (!requireExpander() || !parseCount("up", argc, argv, n)) return;
  uint32_t id;
  finish(actionTempStep(true, n, &id), id);
}

static void cmdDown(int argc, char** argv) {
  int n;
  if (!requireExpander() || !parseCount("down", argc, argv, n)) return;
  uint32_t id;
  finish(actionTempStep(false, n, &id), id);
}

static void cmdTemp(int argc, char** argv) {
  if (!requireExpander()) return;
  long target;
  if (argc < 2 || !parseInt(argv[1], target) || target < cfg.tempMinF || target > cfg.tempMaxF) {
    Logf(WARN, "Usage: temp <%d-%d>", cfg.tempMinF, cfg.tempMaxF);
    return;
  }
  uint32_t id;
  finish(actionSetTemperature((int)target, &id), id);
}

// heater <driver|passenger> <high|medium|low|off>   ("on" is the same as "high")
static void cmdHeater(int argc, char** argv) {
  if (!requireExpander()) return;
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
  uint32_t id;
  finish(actionSetHeater(driver ? SEAT_DRIVER : SEAT_PASSENGER, target, &id), id);
}

// defrost <front|rear> <on|off>
static void cmdDefrost(int argc, char** argv) {
  if (!requireExpander()) return;
  bool front = (argc >= 3 && strcasecmp(argv[1], "front") == 0);
  bool rear = (argc >= 3 && strcasecmp(argv[1], "rear") == 0);
  bool on = (argc >= 3 && strcasecmp(argv[2], "on") == 0);
  bool off = (argc >= 3 && strcasecmp(argv[2], "off") == 0);

  if (!(front || rear) || !(on || off)) {
    Log(WARN, "Usage: defrost <front|rear> <on|off>");
    return;
  }
  uint32_t id;
  finish(actionSetDefrost(front ? DEFROST_FRONT : DEFROST_REAR, on, &id), id);
}

// fan <up|down> [n]
static void cmdFan(int argc, char** argv) {
  if (argc < 2 || (strcasecmp(argv[1], "up") != 0 && strcasecmp(argv[1], "down") != 0)) {
    Log(WARN, "Usage: fan <up|down> [1-50]");
    return;
  }
  bool up = (strcasecmp(argv[1], "up") == 0);
  int n;
  // parseCount reads the optional count from argv[1], so pass the shifted arguments.
  if (!requireExpander() || !parseCount(up ? "fan up" : "fan down", argc - 1, argv + 1, n)) return;
  uint32_t id;
  finish(actionFanStep(up, n, &id), id);
}

// fanset <level>: absolute fan speed
static void cmdFanSet(int argc, char** argv) {
  long level;
  if (argc < 2 || !parseInt(argv[1], level) || level < 1 || level > cfg.fanSteps) {
    Logf(WARN, "Usage: fanset <1-%d>", cfg.fanSteps);
    return;
  }
  uint32_t id;
  finish(actionSetFan((int)level, &id), id);
}

// fancfg <speeds> <extra>: how many fan speeds the car has, and extra down presses when homing
static void cmdFanCfg(int argc, char** argv) {
  long steps, extra;
  if (argc < 3 || !parseInt(argv[1], steps) || !parseInt(argv[2], extra) || steps < 2 || steps > FAN_STEPS_LIMIT ||
      extra < 0 || extra > 5) {
    Logf(WARN, "Usage: fancfg <speeds 2-%d> <extra down presses 0-5>", FAN_STEPS_LIMIT);
    return;
  }
  cfg.fanSteps = steps;
  cfg.fanHomeExtra = extra;
  settingsSave();
  Logf(INFO, "fan: %ld speeds, %ld extra down presses when homing", steps, extra);
}

// autoreset <on|off>: assume heaters and defrost are off when the engine stops
static void cmdAutoReset(int argc, char** argv) {
  if (argc < 2 || (strcasecmp(argv[1], "on") != 0 && strcasecmp(argv[1], "off") != 0)) {
    Logf(WARN, "Usage: autoreset <on|off>   (now %s)", cfg.resetOnEngineStop ? "on" : "off");
    return;
  }
  cfg.resetOnEngineStop = strcasecmp(argv[1], "on") == 0;
  settingsSave();
  Logf(INFO, "Reset tracked heater/defrost state when the engine stops: %s", cfg.resetOnEngineStop ? "on" : "off");
}

static void cmdSettings(int argc, char** argv) {
  settingsPrint();
}

static void cmdPower(int argc, char** argv) {
  if (!requireExpander()) return;
  uint32_t id;
  finish(actionPower(&id), id);
}

// --- Remote start commands ----------------------------------------------------------
static void cmdStart(int argc, char** argv) {
  uint32_t id;
  finish(actionRemoteStart(&id), id);
}

// rswait <seconds>
static void cmdRsWait(int argc, char** argv) {
  long v;
  if (argc < 2 || !parseInt(argv[1], v) || v < 1 || v > 120) {
    Log(WARN, "Usage: rswait <1-120 seconds>");
    return;
  }
  cfg.rsConfirmMs = (uint32_t)v * 1000;
  settingsSave();
  Logf(INFO, "Remote start confirm wait = %ld s", v);
}

// rscfg <pulseMs> <gapMs> <pulses>
static void cmdRsCfg(int argc, char** argv) {
  long pulse, gap, count;
  if (argc < 4 || !parseInt(argv[1], pulse) || !parseInt(argv[2], gap) || !parseInt(argv[3], count) ||
      pulse < 20 || pulse > (long)RS_PULSE_MS_MAX || gap < 0 || gap > 5000 || count < 1 || count > 3) {
    Logf(WARN, "Usage: rscfg <pulse 20-%lu> <gap 0-5000> <pulses 1-3>", (unsigned long)RS_PULSE_MS_MAX);
    return;
  }
  cfg.rsPulseMs = pulse;
  cfg.rsGapMs = gap;
  cfg.rsPulses = count;
  settingsSave();
  Logf(INFO, "Remote start: %ld pulse(s), %ld ms on, %ld ms gap", count, pulse, gap);
}

static void cmdEngine(int argc, char** argv) {
  Logf(INFO, "Engine %s (%lu s since last change, raw pin %s)",
       engineRunning ? "RUNNING" : "stopped",
       (unsigned long)((millis() - engineChangedAt) / 1000),
       digitalRead(ENGINE_RUNNING_PIN) == LOW ? "LOW" : "HIGH");
}

static void cmdNet(int argc, char** argv) {
  cellPrintStatus();
  linkWatchPrintStatus();
}

// wg           show tunnel status
// wg restart   stop the tunnel and let it start again (a new handshake attempt)
static void cmdI2c(int argc, char** argv) {
  outputsPrintStatus();
}

static void cmdApi(int argc, char** argv) {
  apiPrintStatus();
}

static void cmdWg(int argc, char** argv) {
  if (argc >= 2 && strcasecmp(argv[1], "restart") == 0) {
    wgRestart();
    return;
  }
  wgPrintStatus();
}

// --- Authentication commands -------------------------------------------------------------
// keyadd <id> <control|admin> <base64 public key>
static void cmdKeyAdd(int argc, char** argv) {
  Role role = Role::Control;
  bool roleOk = false;
  if (argc >= 4 && strcasecmp(argv[2], "admin") == 0) { role = Role::Admin; roleOk = true; }
  if (argc >= 4 && strcasecmp(argv[2], "control") == 0) { role = Role::Control; roleOk = true; }
  if (!roleOk) {
    Log(WARN, "Usage: keyadd <id> <control|admin> <base64 public key (65 bytes, 04||X||Y)>");
    return;
  }
  uint8_t pub[65];
  if (!authParsePublicKey(argv[3], pub)) {
    Log(ERROR, "Not a valid P-256 public key (expect base64 of the 65-byte uncompressed point)");
    return;
  }
  switch (authKeyAdd(argv[1], role, pub)) {
    case KeyAdd::Ok: {
      char fp[17];
      authFingerprint(pub, fp);
      Logf(INFO, "Key '%s' enrolled as %s (fingerprint %s)", argv[1], roleName(role), fp);
      break;
    }
    case KeyAdd::BadId:   Log(ERROR, "Key id must be 1-15 characters: letters, digits, - or _"); break;
    case KeyAdd::BadRole: Log(ERROR, "Bad role"); break;
    case KeyAdd::BadKey:  Log(ERROR, "Public key rejected"); break;
    case KeyAdd::Full:    Logf(ERROR, "Key store is full (%d keys). Remove one first", AUTH_MAX_KEYS); break;
  }
}

static void cmdKeyList(int argc, char** argv) {
  int n = authKeyCount();
  if (n == 0) {
    Log(INFO, "No keys enrolled. Remote commands cannot be authorized yet");
    return;
  }
  KeyInfo k;
  for (int i = 0; i < n && authKeyAt(i, k); i++) {
    char fp[17];
    authFingerprint(k.pub, fp);
    Logf(INFO, "  %-15s %-8s fingerprint %s", k.id, roleName(k.role), fp);
  }
}

static void cmdKeyDel(int argc, char** argv) {
  if (argc < 2) {
    Log(WARN, "Usage: keydel <id>");
    return;
  }
  if (authKeyRemove(argv[1])) {
    Logf(INFO, "Key '%s' removed", argv[1]);
  } else {
    Logf(WARN, "No key named '%s'", argv[1]);
  }
}

static void cmdKeyWipe(int argc, char** argv) {
  if (argc < 2 || strcasecmp(argv[1], "yes") != 0) {
    Log(WARN, "This removes every enrolled key. Run 'keywipe yes' to confirm");
    return;
  }
  authKeyWipe();
  Log(INFO, "All keys removed");
}

static void cmdAuthLog(int argc, char** argv) {
  authPrintLog();
}

static void cmdAuthTest(int argc, char** argv) {
  authSelfTest();
}

// rsenable [on|off]   (without an argument it shows the current state)
static void cmdRsEnable(int argc, char** argv) {
  if (argc >= 2 && strcasecmp(argv[1], "on") == 0) {
    setRemoteStartEnabled(true);
  } else if (argc >= 2 && strcasecmp(argv[1], "off") == 0) {
    setRemoteStartEnabled(false);
  } else if (argc >= 2) {
    Log(WARN, "Usage: rsenable [on|off]");
    return;
  }
  Logf(INFO, "Remote start over the network is %s (the serial 'start' command always works)",
       remoteStartEnabled() ? "ENABLED" : "disabled");
}

// --- Bench test ------------------------------------------------------------------
// Holds ONE expander output ON for several seconds so it can be measured with a
// multimeter. This deliberately exceeds the normal 500 ms hold cap, so it is limited
// to BENCH_MAX_S, any keypress aborts it, and the pin is always driven LOW afterwards.
// The remote-start trigger is intentionally not included.
struct BenchOutput {
  const char* name;
  uint8_t pin;
};

static const BenchOutput benchOutputs[] = {
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

// bench start <seconds> confirm
// Holds the remote-start trigger output ON for a few seconds so it can be measured. Unlike the
// climate buttons this wire goes to the iDatastart module, and pulling it to ground is what starts
// the engine, so it is capped at 10 s and needs the word "confirm" as a deliberate second step.
static void benchStart(int argc, char** argv) {
  long seconds;
  if (argc < 3 || !parseInt(argv[2], seconds) || seconds < 1 || seconds > 10) {
    Log(WARN, "Usage: bench start <1-10 s> confirm");
    return;
  }
  if (argc < 4 || strcasecmp(argv[3], "confirm") != 0) {
    Log(WARN, "This pulls the start-trigger output to ground. DISCONNECT the wire from the module (M3-06) first,");
    Log(WARN, "then repeat: bench start <seconds> confirm");
    return;
  }
  if (!actionsLock(2000)) {
    Log(WARN, "Busy: an action is still running, try again");
    return;
  }
  Logf(INFO, "Bench: start trigger ON (GPIO %d HIGH) for %ld s. Measure now. Press any key to stop early.", RS_TRIGGER_PIN, seconds);
  delay(50);
  while (Serial.available()) Serial.read();
  digitalWrite(RS_TRIGGER_PIN, HIGH);
  uint32_t end = millis() + (uint32_t)seconds * 1000;
  while (millis() < end && !Serial.available()) delay(10);
  digitalWrite(RS_TRIGGER_PIN, LOW);
  actionsUnlock();
  while (Serial.available()) Serial.read();
  Log(INFO, "Bench: start trigger OFF");
}

// bench <up|down|driver|passenger|front|rear|fanup|fandown|power> <seconds>
static void cmdBench(int argc, char** argv) {
  if (argc >= 2 && strcasecmp(argv[1], "start") == 0) {
    benchStart(argc, argv);
    return;
  }
  if (!requireExpander()) return;
  const BenchOutput* out = nullptr;
  long seconds;
  if (argc >= 3) {
    for (const BenchOutput& o : benchOutputs) {
      if (strcasecmp(argv[1], o.name) == 0) out = &o;
    }
  }
  if (out && out->pin == CLIMATE_POWER_PIN && !CLIMATE_POWER_ENABLED) {
    Log(WARN, "The climate power button is disabled (CLIMATE_POWER_ENABLED in config.h)");
    return;
  }
  if (!out || !parseInt(argv[2], seconds) || seconds < 1 || seconds > (long)BENCH_MAX_S) {
    Logf(WARN, "Usage: bench <up|down|driver|passenger|front|rear|fanup|fandown|power> <1-%lu s>   or: bench start <1-10 s> confirm", (unsigned long)BENCH_MAX_S);
    return;
  }

  Logf(INFO, "Bench: %s ON for %ld s. Measure now. Press any key to stop early.", out->name, seconds);
  // Throw away leftover input first (the "\n" of the Enter that started this command),
  // otherwise it would count as the "any key" that stops the test immediately.
  delay(50);
  while (Serial.available()) Serial.read();

  // Take exclusive access so no queued action can press buttons during the measurement.
  if (!actionsLock(2000)) {
    Log(WARN, "Busy: an action is still running, try again");
    return;
  }
  mcp.digitalWrite(out->pin, HIGH);
  uint32_t end = millis() + (uint32_t)seconds * 1000;
  while (millis() < end && !Serial.available()) {
    delay(10);
  }
  mcp.digitalWrite(out->pin, LOW);
  actionsUnlock();
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

static const Command commands[] = {
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
  {"fanset", "fanset <level>",                "set the fan to an absolute speed (homes down, then up)", cmdFanSet},
  {"fancfg", "fancfg <speeds> <extra>",       "set how many fan speeds the car has",          cmdFanCfg},
  {"autoreset", "autoreset <on|off>",         "assume heaters/defrost off when the engine stops", cmdAutoReset},
  {"settings", "settings",                    "show all saved settings",                      cmdSettings},
  {"power",  "power",                         "press the climate power button once (currently DISABLED in config.h)", cmdPower},
  {"rswait", "rswait <sec>",                  "set how long start waits to confirm the engine", cmdRsWait},
  {"bench",  "bench <name> <sec>",            "hold one output ON for metering (also: bench start)",     cmdBench},
  {"net",    "net",                           "show cellular connection and clock status",       cmdNet},
  {"wg",     "wg [restart]",                  "show WireGuard tunnel status, or restart it",                 cmdWg},
  {"keyadd", "keyadd <id> <role> <key>",      "enroll a client public key (role control|admin)", cmdKeyAdd},
  {"keylist", "keylist",                      "list enrolled keys",                           cmdKeyList},
  {"keydel", "keydel <id>",                   "remove an enrolled key",                       cmdKeyDel},
  {"keywipe", "keywipe yes",                  "remove every enrolled key",                    cmdKeyWipe},
  {"authlog", "authlog",                      "show recent authentication attempts",          cmdAuthLog},
  {"authtest", "authtest",                    "run the built-in authentication self-test",    cmdAuthTest},
  {"rsenable", "rsenable [on|off]",           "allow or forbid network-requested remote start", cmdRsEnable},
  {"i2c",    "i2c",                           "show the expander's registers (read-only)",   cmdI2c},
  {"api",    "api",                           "show HTTPS API status and certificate fingerprint", cmdApi},
  {"engine", "engine",                        "show engine running status (GWR input)",       cmdEngine},
  {"start",  "start",                         "send the remote-start trigger pulse(s)",       cmdStart},
  {"rscfg",  "rscfg <ms> <gap> <n>",          "set trigger pulse width, gap and pulse count", cmdRsCfg},
};
static constexpr size_t COMMAND_COUNT = sizeof(commands) / sizeof(commands[0]);

static void printHelp(int argc, char** argv) {
  Serial.println("--- Commands ---");
  for (size_t i = 0; i < COMMAND_COUNT; i++) {
    Serial.printf("  %-20s %s\n", commands[i].Usage, commands[i].help);
  }
}

// --- Serial line reader ------------------------------------------------------------
static void printPrompt() {
  Serial.print("> ");
}

static void runLine(char* line) {
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

void consoleBegin() {
  printHelp(0, nullptr);
  printPrompt();
}

void consolePoll() {
  static char line[200];   // long enough for "keyadd <id> <role> <88-character public key>"
  static size_t len = 0;
  static bool overflow = false;  // the line did not fit: reject it instead of running a cut-off command
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
      if (overflow) {
        Logf(ERROR, "Line too long (limit %u characters): ignored", (unsigned)(sizeof(line) - 1));
      } else {
        runLine(line);
      }
      len = 0;
      overflow = false;
      printPrompt();  // ready for the next command
    } else if (c == '\b' || c == 0x7F) {
      if (len > 0) {
        len--;
        Serial.print("\b \b");  // step back, blank the character, step back
      }
    } else if (c < 0x20 || c > 0x7E) {
      // Ignore other control and non-ASCII bytes, e.g. the escape sequences an arrow key sends.
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
      Serial.write(c);  // echo what was actually stored
    } else {
      overflow = true;
    }
  }
}
