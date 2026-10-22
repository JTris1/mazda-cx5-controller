# Local patches

Vendored from https://github.com/ciniml/WireGuard-ESP32-Arduino (library WireGuard-ESP32 0.1.5, license in LICENSE).

* `src/wireguardif.c`, `src/wireguardif.h`: the library looked up the Wi-Fi interface with
  `tcpip_adapter_get_netif(TCPIP_ADAPTER_IF_STA, ...)`, which does not exist in ESP-IDF 5
  (Arduino core 3.x) and would be the wrong interface for a cellular modem anyway. It now uses an
  interface chosen by the caller through `wireguardif_set_underlying_netif()`, which must be called
  before `WireGuard.begin()`. `wireguardif_init()` fails if none was set.
* `src/wireguard.h`: added the missing declaration of `handshake_destroy()` (the newer GCC in Arduino core 3.x
  treats the implicit declaration as an error).
* `src/WireGuard.cpp`: `begin()` and `end()` call lwIP netif and UDP functions directly. ESP-IDF 5 builds lwIP
  with core locking, which asserts ("Required to lock TCPIP core functionality") unless the caller holds the
  lock. They now take `LOCK_TCPIP_CORE()` around those calls, but not around the DNS lookup, which would
  deadlock (the lookup needs the lwIP thread to receive the reply).
