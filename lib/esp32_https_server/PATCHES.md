# Local patches

Vendored from https://github.com/fhessel/esp32_https_server at commit
de1876cf6fe717cf236ad6603a97e88f22e38d62 (MIT license, see LICENSE).

* `src/HTTPConnection.hpp` / `src/HTTPConnection.cpp`: the WebSocket handshake used the
  ESP32-only `hwcrypto/sha.h` and `esp_sha()`. Replaced with mbedTLS `mbedtls_sha1_ret()` so the
  library builds for the ESP32-S3. WebSockets are not used by this project.
