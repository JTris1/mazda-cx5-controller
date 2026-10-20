#pragma once
// Copy this file to src/secrets.h (which is untracked) and fill in your values.
// Never commit secrets.h.

// Phone hotspot for the prototype. The ESP32-S3 is 2.4 GHz only: on an iPhone turn on
// "Maximize Compatibility" for the hotspot.
constexpr char WIFI_SSID[] = "your-hotspot-name";
constexpr char WIFI_PASSWORD[] = "your-hotspot-password";
