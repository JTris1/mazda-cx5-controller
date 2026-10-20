#pragma once
// Copy this file to src/secrets.h (which is untracked) and fill in your values.
// Never commit secrets.h.

// Phone hotspot for the prototype. The ESP32-S3 is 2.4 GHz only: on an iPhone turn on
// "Maximize Compatibility" for the hotspot.
constexpr char WIFI_SSID[] = "your-hotspot-name";
constexpr char WIFI_PASSWORD[] = "your-hotspot-password";

// --- WireGuard tunnel to the home server (see docs/wireguard-server.md) -----------
// Leave WG_ENABLED false until the server side is set up.
constexpr bool WG_ENABLED = false;
constexpr char WG_LOCAL_IP[] = "10.8.0.2";                 // this device's address inside the tunnel
constexpr char WG_PRIVATE_KEY[] = "base64-private-key";    // from the server's peer_esp32 folder
constexpr char WG_SERVER_PUBLIC_KEY[] = "base64-server-public-key";
constexpr char WG_ENDPOINT[] = "wg.example.com";           // DDNS hostname or public IP of the home server
constexpr uint16_t WG_PORT = 51820;
