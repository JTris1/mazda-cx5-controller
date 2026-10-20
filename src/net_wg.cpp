#include <WireGuard-ESP32.h>
#include "net_wg.h"
#include "log.h"
#include "net_time.h"
#include "net_wifi.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing src/secrets.h: copy src/secrets.example.h to src/secrets.h and fill it in."
#endif

static WireGuard wg;
static bool started = false;
static uint32_t startedAt = 0;

bool wgStarted() {
  return started;
}

void wgPoll() {
  if (!WG_ENABLED) return;

  if (started && !wifiConnected()) {
    // The underlying network went away: stop the tunnel so it can come up cleanly later.
    wg.end();
    started = false;
    Log(WARN, "WireGuard: stopped (Wi-Fi lost)");
    return;
  }

  if (!started && wifiConnected() && timeSynced()) {
    Logf(INFO, "WireGuard: starting tunnel to %s:%u (may take a few seconds)", WG_ENDPOINT, WG_PORT);
    IPAddress localIp;
    if (!localIp.fromString(WG_LOCAL_IP)) {
      Logf(ERROR, "WireGuard: bad WG_LOCAL_IP '%s'", WG_LOCAL_IP);
      return;
    }
    // begin() resolves the endpoint (up to 5 tries, 2 s apart) before returning.
    if (wg.begin(localIp, WG_PRIVATE_KEY, WG_ENDPOINT, WG_SERVER_PUBLIC_KEY, WG_PORT)) {
      started = true;
      startedAt = millis();
      Logf(INFO, "WireGuard: started, tunnel address %s", WG_LOCAL_IP);
    } else {
      Log(ERROR, "WireGuard: begin() failed (endpoint lookup or keys), will retry");
      delay(5000);
    }
  }
}

void wgPrintStatus() {
  if (!WG_ENABLED) {
    Log(INFO, "WireGuard: disabled (WG_ENABLED is false in secrets.h)");
    return;
  }
  Logf(INFO, "WireGuard: %s, tunnel %s, endpoint %s:%u", started ? "started" : "not started",
       WG_LOCAL_IP, WG_ENDPOINT, WG_PORT);
  if (started) {
    Logf(INFO, "Up for %lu s. Check 'wg show' on the server for the handshake.",
         (unsigned long)((millis() - startedAt) / 1000));
  } else if (!wifiConnected()) {
    Log(INFO, "Waiting for Wi-Fi");
  } else if (!timeSynced()) {
    Log(INFO, "Waiting for the clock to sync (WireGuard needs real time)");
  }
}
