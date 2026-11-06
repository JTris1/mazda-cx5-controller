#include <HardwareSerial.h>
#include <Network.h>
#include <PPP.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>   // esp_netif_get_netif_impl()
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "net_cell.h"
#include "config.h"
#include "log.h"
#include "net_time.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing src/secrets.h: copy src/secrets.example.h to src/secrets.h and fill it in."
#endif

enum class CellState : uint8_t { Starting, PoweringModem, Registering, Connected, Retrying };

static volatile CellState state = CellState::Starting;
static volatile uint32_t connectedAt = 0;
static volatile uint32_t lastFailureAt = 0;
static char lastFailure[48] = "";
static volatile bool restartRequested = false;

void cellRequestRestart() {
  restartRequested = true;
}

static constexpr uint32_t RETRY_MIN_MS = 10000;
static constexpr uint32_t RETRY_MAX_MS = 120000;
static constexpr uint32_t ATTACH_TIMEOUT_MS = 90000;     // registering on the network can take a while
static constexpr uint32_t CONNECT_TIMEOUT_MS = 60000;    // PPP negotiation after the data call starts

static const char* stateName(CellState s) {
  switch (s) {
    case CellState::Starting:      return "starting";
    case CellState::PoweringModem: return "powering the modem";
    case CellState::Registering:   return "registering / connecting";
    case CellState::Connected:     return "connected";
    case CellState::Retrying:      return "waiting to retry";
  }
  return "?";
}

static void fail(const char* why) {
  strncpy(lastFailure, why, sizeof(lastFailure) - 1);
  lastFailureAt = millis();
  Logf(WARN, "Cellular: %s", why);
}

// --- modem power -----------------------------------------------------------------------
// Says "AT" on the modem's UART and waits for "OK". Uses Serial1 only briefly: the PPP driver
// needs the same UART afterwards, so it is released again.
static bool modemResponds(uint32_t timeoutMs) {
  Serial1.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  uint32_t start = millis();
  bool ok = false;
  String reply;
  while (!ok && millis() - start < timeoutMs) {
    while (Serial1.available()) Serial1.read();
    Serial1.print("AT\r\n");
    uint32_t t = millis();
    reply = "";
    while (millis() - t < 300) {
      while (Serial1.available()) reply += (char)Serial1.read();
      if (reply.indexOf("OK") >= 0) { ok = true; break; }
      delay(10);
    }
  }
  Serial1.end();
  return ok;
}

// Asks the modem a few questions with plain AT commands and logs the answers. This is the first
// thing to look at when the link does not come up: SIM present, signal, network registration and APN.
// (Only possible before PPP starts: in PPP data mode the UART carries IP packets, not commands.)
static void logModemInfo() {
  static const char* commands[] = {"ATI", "AT+CPIN?", "AT+CSQ", "AT+CEREG?", "AT+CREG?", "AT+COPS?", "AT+CGATT?", "AT+CGDCONT?"};
  Serial1.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  for (const char* cmd : commands) {
    while (Serial1.available()) Serial1.read();
    Serial1.print(cmd);
    Serial1.print("\r\n");
    String reply;
    uint32_t t = millis();
    while (millis() - t < 2500) {
      while (Serial1.available()) reply += (char)Serial1.read();
      if (reply.indexOf("OK") >= 0 || reply.indexOf("ERROR") >= 0) break;
      delay(10);
    }
    reply.replace("\r", " ");
    reply.replace("\n", " ");
    reply.trim();
    while (reply.indexOf("  ") >= 0) reply.replace("  ", " ");
    Logf(INFO, "Modem: %s -> %s", cmd, reply.length() ? reply.c_str() : "(no answer)");
  }
  Serial1.end();
}

// PWRKEY idles LOW. A short HIGH pulse (100 ms) switches the modem ON; it has no effect while the
// modem is on. Switching OFF needs the pin held HIGH for about 3 s (LilyGO's reference values).
static void pwrkeyPulse(uint32_t highMs) {
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(highMs);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
}

// If the ESP32 resets while a PPP session is open (a crash, a reflash), the modem stays in data
// mode and ignores AT commands. The standard escape is "+++" with a second of silence on each side.
static bool escapeDataMode() {
  Serial1.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  while (Serial1.available()) Serial1.read();
  delay(1200);
  Serial1.print("+++");
  delay(1200);
  Serial1.end();
  return modemResponds(3000);
}

// Gets the modem to answer AT, trying the gentlest recovery first.
static bool ensureModemOn() {
  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);   // keep the modem awake

  if (modemResponds(2000)) {
    Log(INFO, "Cellular: modem is already on");
    return true;
  }
  Log(INFO, "Cellular: modem is silent, trying to leave data mode");
  if (escapeDataMode()) {
    Log(INFO, "Cellular: modem recovered");
    return true;
  }
  Log(INFO, "Cellular: switching the modem on");
  pwrkeyPulse(100);
  delay(3000);                         // boot time
  if (modemResponds(20000)) return true;

  Log(WARN, "Cellular: still silent, power cycling the modem");
  pwrkeyPulse(3000);                   // switch off
  delay(8000);
  pwrkeyPulse(100);                    // switch on
  delay(3000);
  return modemResponds(25000);
}

// --- connection task ----------------------------------------------------------------------
static void cellTask(void*) {
  uint32_t retryMs = RETRY_MIN_MS;
  for (;;) {
    state = CellState::PoweringModem;
    bool up = false;
    if (!ensureModemOn()) {
      fail("modem does not answer (check power and antenna)");
    } else {
      state = CellState::Registering;
      logModemInfo();
      Logf(INFO, "Cellular: connecting, APN '%s'", CELL_APN);
      PPP.setApn(CELL_APN);
      if (CELL_SIM_PIN[0]) PPP.setPin(CELL_SIM_PIN);
      PPP.setPins(MODEM_TX_PIN, MODEM_RX_PIN);
      // The SIM7670G speaks the same AT dialect as the SIM7600 that the driver supports.
      if (!PPP.begin(PPP_MODEM_SIM7600, 1, MODEM_BAUD)) {
        fail("PPP did not start (SIM, signal or APN problem)");
      } else {
        // begin() only prepares the modem (SIM check) and leaves it in command mode. The data call,
        // which is what actually creates the PPP link, starts when the mode is switched to DATA.
        Log(INFO, "Cellular: modem ready, waiting for it to attach to the network");
        uint32_t start = millis();
        while (!PPP.attached() && millis() - start < ATTACH_TIMEOUT_MS) vTaskDelay(pdMS_TO_TICKS(500));
        if (!PPP.attached()) {
          fail("modem did not attach to the network");
        } else if (!PPP.mode(ESP_MODEM_MODE_DATA)) {
          fail("could not switch the modem to data mode");
        } else {
          Log(INFO, "Cellular: data call started, waiting for an IP address");
          start = millis();
          uint32_t lastNote = start;
          while (!PPP.hasIP() && millis() - start < CONNECT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(500));
            if (millis() - lastNote >= 15000) {
              lastNote = millis();
              Logf(INFO, "Cellular: still waiting for an IP address (%lu s)", (unsigned long)((millis() - start) / 1000));
            }
          }
          up = PPP.hasIP();
          if (!up) fail("no IP address from the network");
        }
      }
    }

    if (up) {
      retryMs = RETRY_MIN_MS;
      connectedAt = millis();
      state = CellState::Connected;
      Logf(INFO, "Cellular: connected, IP %s", PPP.localIP().toString().c_str());
      while (PPP.hasIP() && !restartRequested) vTaskDelay(pdMS_TO_TICKS(1000));
      fail(restartRequested ? "restart requested by the link watchdog" : "connection lost");
      restartRequested = false;
    }

    PPP.end();
    state = CellState::Retrying;
    Logf(INFO, "Cellular: retrying in %lu s", (unsigned long)(retryMs / 1000));
    vTaskDelay(pdMS_TO_TICKS(retryMs));
    retryMs = min(retryMs * 2, RETRY_MAX_MS);
  }
}

static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_PPP_START:        Log(INFO, "PPP event: start"); break;
    case ARDUINO_EVENT_PPP_CONNECTED:    Log(INFO, "PPP event: link up (negotiated)"); break;
    case ARDUINO_EVENT_PPP_GOT_IP:       Log(INFO, "PPP event: got an IP address"); break;
    case ARDUINO_EVENT_PPP_LOST_IP:      Log(WARN, "PPP event: lost the IP address"); break;
    case ARDUINO_EVENT_PPP_DISCONNECTED: Log(WARN, "PPP event: link down"); break;
    case ARDUINO_EVENT_PPP_STOP:         Log(INFO, "PPP event: stop"); break;
    default: break;
  }
}

void cellBegin() {
  Network.onEvent(onNetworkEvent);
  xTaskCreatePinnedToCore(cellTask, "cell", 8192, nullptr, 1, nullptr, 0);
}

bool cellConnected() {
  return state == CellState::Connected && PPP.hasIP();
}

struct netif* cellLwipNetif() {
  esp_netif_t* n = PPP.netif();
  return n ? (struct netif*)esp_netif_get_netif_impl(n) : nullptr;
}

void cellPrintStatus() {
  Logf(INFO, "Cellular: %s", stateName(state));
  if (cellConnected()) {
    Logf(INFO, "IP %s, gateway %s, DNS %s, connected for %lu s", PPP.localIP().toString().c_str(),
         PPP.gatewayIP().toString().c_str(), PPP.dnsIP().toString().c_str(),
         (unsigned long)((millis() - connectedAt) / 1000));
  } else if (lastFailure[0]) {
    Logf(INFO, "Last problem: %s (%lu s ago)", lastFailure, (unsigned long)((millis() - lastFailureAt) / 1000));
  }
  Logf(INFO, "Time: %s", timeNowString().c_str());
}
