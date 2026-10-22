#include <Network.h>
#include <WireGuard-ESP32.h>
#include <lwip/netif.h>
#include "net_wg.h"
#include "log.h"
#include "net_time.h"
#include "net_cell.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing src/secrets.h: copy src/secrets.example.h to src/secrets.h and fill it in."
#endif

// Added to the vendored WireGuard library (see lib/WireGuard-ESP32/PATCHES.md).
extern "C" void wireguardif_set_underlying_netif(struct netif* underlying);

static WireGuard wg;
static bool started = false;
static uint32_t startedAt = 0;
static uint32_t nextTry = 0;   // do not retry a failed start before this time (millis)

bool wgStarted() {
  return started;
}

void wgRestart() {
  if (started) {
    wg.end();
    started = false;
  }
  Log(INFO, "WireGuard: restarting");
}

void wgPoll() {
  if (!WG_ENABLED) return;

  if (started && !cellConnected()) {
    // The underlying network went away: stop the tunnel so it can come up cleanly later.
    wg.end();
    started = false;
    Log(WARN, "WireGuard: stopped (cellular link lost)");
    return;
  }

  if (!started && cellConnected() && timeSynced() && (int32_t)(millis() - nextTry) >= 0) {
    Logf(INFO, "WireGuard: starting tunnel to %s:%u (may take a few seconds)", WG_ENDPOINT, WG_PORT);
    // Show what the endpoint name resolves to from this network (the library resolves it itself).
    IPAddress endpointIp;
    if (Network.hostByName(WG_ENDPOINT, endpointIp)) {
      Logf(INFO, "WireGuard: %s resolves to %s", WG_ENDPOINT, endpointIp.toString().c_str());
    } else {
      Logf(ERROR, "WireGuard: cannot resolve %s from this network, retrying in 30 s", WG_ENDPOINT);
      nextTry = millis() + 30000;
      return;
    }
    IPAddress localIp;
    if (!localIp.fromString(WG_LOCAL_IP)) {
      Logf(ERROR, "WireGuard: bad WG_LOCAL_IP '%s'", WG_LOCAL_IP);
      return;
    }
    // Tell the library which interface carries the encrypted packets: the cellular PPP link.
    struct netif* underlying = cellLwipNetif();
    if (!underlying) {
      Log(ERROR, "WireGuard: no cellular interface yet, retrying in 30 s");
      nextTry = millis() + 30000;
      return;
    }
    wireguardif_set_underlying_netif(underlying);
    // begin() resolves the endpoint (up to 5 tries, 2 s apart) before returning.
    if (wg.begin(localIp, WG_PRIVATE_KEY, WG_ENDPOINT, WG_SERVER_PUBLIC_KEY, WG_PORT)) {
      started = true;
      startedAt = millis();
      Logf(INFO, "WireGuard: started, tunnel address %s", WG_LOCAL_IP);
    } else {
      Log(ERROR, "WireGuard: begin() failed (endpoint lookup or keys), retrying in 30 s");
      nextTry = millis() + 30000;
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
  } else if (!cellConnected()) {
    Log(INFO, "Waiting for the cellular link");
  } else if (!timeSynced()) {
    Log(INFO, "Waiting for the clock to sync (WireGuard needs real time)");
  }
}
