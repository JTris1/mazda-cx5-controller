# CLAUDE.md: CX-5 climate remote (ESP32-S3 + SIM7670G)

Everything a fresh session (on a new machine, with no memory of earlier chats) needs to continue. Read it all before
changing anything. Details that live in code or other docs are pointed to, not repeated.

## Goal

Cellular remote start and climate pre-conditioning for a 2022 Mazda CX-5, independent of Mazda's subscription. An
ESP32-S3 drives PC817 optocouplers that "press" the climate control unit (CCU) switch pads, and triggers an
iDatastart HCXG0 remote-start module. Budget: under $10/month for connectivity. The device is **installed in the
user's car, unattended**, so reliability and remote diagnosis matter more than features.

## How to work with this user

* Commit and push **only when asked**. Never `git add -A` blindly: `.vscode/extensions.json` is modified locally and
  stays out of commits. Check `git status` and secrets before pushing (see "Secrets").
* Never paste, echo or commit private keys, `src/secrets.h`, `src/tls_cert.h`, or `~/.cx5/*.pem`. If the user pastes
  one, tell them to rotate it.
* They like explanations of *why* something broke and what a fix does, short answers, and recommendations rather
  than option lists. They said early on not to dump huge amounts of code at once. They read logs they paste
  (boot logs, admin-page Device log lines); ask for those when debugging.
* Be honest about what is **verified on hardware** and what is only built or tested on the simulator. Several
  earlier claims turned out wrong (for example the data-use estimate) and were corrected; do not state unmeasured
  numbers as facts.
* Do not remove or rewrite their own README wording (the AI-assistance notice, the Build Log placeholder).

## Repositories and what is not in git

* GitHub: `https://github.com/JTris1/mazda-cx5-controller` (branch `main`). This project folder is that repo.
* **Not in git, must be carried over by hand** (a new machine cannot rebuild these from the repo):
  * `src/secrets.h`: APN, WireGuard private key, server public key, endpoint, tunnel IP. Template:
    `src/secrets.example.h`. Losing it means a new WireGuard key pair and a router peer update.
  * `src/tls_cert.h`: the device's TLS certificate and private key. **Home Assistant pins its SHA-256
    fingerprint**, so regenerating it (`tools/gen_cert.sh`) changes the fingerprint and every client (Home Assistant
    config entry, `CX5_FINGERPRINT` env var) must be updated. Any new firmware built without the original file will
    not match the pinned fingerprint and clients will refuse it.
  * `~/.cx5/*.pem`: the user's own client private keys made by `tools/cx5_client.py keygen`. Home Assistant stores
    its own key inside its config; the admin page key lives non-extractable in the user's browser (IndexedDB) and
    cannot be copied, so after a browser change just create a new one and enroll it (an existing admin does that).
  * KiCad work: `~/Documents/mazda-cx5-controller/hardware/` on the old machine, **never committed**. Ask the user
    whether to commit it to the repo or back it up.
* The old machine also had a second clone of the same GitHub repo at `~/Documents/mazda-cx5-controller` (it only held
  `hardware/` and an older `CLAUDE.md`). Ignore it and never push from it.
* Local-only branches that were never pushed: `gps` (the abandoned CMUX experiment) and `cellular` (an ancestor of
  `main`, nothing in it that `main` lacks). Nothing needs to be rescued.

## Setting up a new machine (Linux)

1. `git clone https://github.com/JTris1/mazda-cx5-controller.git`, then copy `src/secrets.h` and `src/tls_cert.h` in.
2. PlatformIO needs **Python 3.10-3.13** (pioarduino's installer refuses others). Example:
   `python3 -m venv ~/.platformio/penv && ~/.platformio/penv/bin/pip install platformio`, then `pio run` in the
   project (the first build downloads the platform and toolchain, about 1 GB; `platformio.ini` pins the pioarduino
   release URL on purpose). If the system Python is too old: `pip install uv`, `uv python install 3.12`.
3. Serial access for flashing: add the user to the `dialout` group (log out and in). `pio run -t upload`; if it
   fails, hold BOOT, tap RESET, release BOOT. The board's USB-C goes straight to the ESP32-S3's native USB, so the
   serial console is `pio device monitor` at 115200.
4. Tests need **Python 3.14** (Home Assistant 2026): `uv python install 3.14`, create a venv, `pip install -r
   homeassistant/requirements_test.txt`, run `python -m pytest` inside `homeassistant/` (74 pass, about 15 s).
   `python tests/serve_mock.py` serves the admin page against the simulator at http://127.0.0.1:8765/admin.
5. Build check: `pio run` should end with SUCCESS (about 965 KB of a 6.5 MB slot).

## Network and deployment facts

* Cellular: Telnyx IoT SIM, APN `data00.telnyx`, LTE (T-Mobile). Data costs about $0.0078/MB.
* The ESP32 dials a **WireGuard** tunnel to the user's **OpenWrt router** (the hub). ESP32 tunnel IP: `192.168.10.2`.
  Home Assistant and the user's laptop reach the API through the router. The ESP32's firewall rule forbids it from
  opening connections to other peers, so Home Assistant **polls**.
  `docs/wireguard-server.md` was written for a Docker hub (addresses 10.8.0.x) and still mentions Wi-Fi in places; the
  real hub is the router, so treat that doc as the concept, not the config.
* The WireGuard library keepalive is 10 s. It gives up after about 90 s of failed handshakes and never retries by
  itself (this caused a multi-day outage after a router firmware upgrade; the link watchdog now handles it).
* Device HTTPS server: `https://192.168.10.2/` on the tunnel address only (requests arriving on another interface
  are closed). TLS 1.2 is forced on clients (TLS 1.3 was slow and unreliable on the ESP32). Self-signed certificate,
  clients pin the SHA-256 fingerprint. Two connection slots only: clients must serialize calls; the server closes
  idle connections after about 5 s.

## Architecture (all in `src/`)

| File | Role |
|---|---|
| `main.cpp` | `setup()` / `loop()` only; loop calls the pollers, enables a 60 s hardware watchdog on the loop task |
| `config.h`, `config.cpp`, `settings.*` | pins, limits, `Settings` struct (`cfg`), persisted to NVS (blob namespace `settings`, version 1: **changing the struct requires bumping `SETTINGS_VERSION`, which resets saved settings**) |
| `outputs.*` | MCP23017 access and button presses (hold cap 500 ms, retry-to-low on I2C error) |
| `climate.*`, `vehicle_state.*` | climate commands (absolute temperature and fan by "homing": press down to the minimum plus extra, then up) and the tracked state (NVS namespace `vstate`) |
| `actions.*` | the only code that touches outputs: a FreeRTOS job queue and one worker; console and API both submit here; jobs have ids and results |
| `engine.*`, `remote_start.*` | engine-running input (GPIO 39, low = running, debounced), start trigger (GPIO 40) with confirmation and policy (cooldown 60 s, 3/h, 10/day, nonce at most 15 s old, disabled until enabled) |
| `net_cell.*` | modem power-up/recovery (PWRKEY, "+++" escape), AT sanity queries, PPP via the Arduino `PPP` class; retry with back-off |
| `net_time.*`, `net_wg.*` | NTP (WireGuard needs the clock), WireGuard start/stop and `wgTunnelUp()` |
| `link_watch.*` | link watchdog (below) |
| `auth.*` | key store (8 keys, NVS), one-time nonces, ECDSA P-256 verify, roles, per-address rate limits, auth log |
| `api.cpp`, `api_internal.h`, `admin_api.cpp` | HTTPS server on the IDF `esp_https_server`; control endpoints, admin endpoints, first-key setup, the admin page, log and OTA endpoints |
| `ota.*` | streamed OTA with signed manifest, SHA-256, rollback on probation |
| `log.*`, `flash_log.*` | logging, RTC-memory ring, flash log (below) |
| `console.*` | serial console (`help` lists commands) |
| `lib/WireGuard-ESP32` | vendored library with local patches (see its `PATCHES.md`) |

Full protocol and endpoint list: **`docs/api.md`** (read it before touching the API). Signed string:
`"CX5-V1\n<nonce>\n<key_id>\n<METHOD>\n<path>\n<hex sha256(body)>"`, ECDSA P-256 / SHA-256, signature = base64 of raw
r||s; headers `X-CX5-Key`, `X-CX5-Nonce`, `X-CX5-Signature`. Public keys: base64 of the 65-byte uncompressed point.
Roles: `control` and `admin`. Commands return `202 {job_id}` and the client polls `GET /api/v1/jobs/<id>`.

### Reliability features (written and built, not yet proven in the car)

* **Link watchdog** (`link_watch.cpp`): tunnel without a valid session for 3 min restarts the tunnel (again every
  5 min while down), 8 min restarts the cellular link, 20 min restarts the device; 15 min with no cellular restarts
  the device. Consecutive watchdog restarts wait longer (20/40/80/120 min). A restart held back for a running job or an
  OTA upload goes ahead after 30 more minutes.
* **Hang watchdog:** loop task registered with the task watchdog, 60 s, panics and restarts.
* **Two logs.** An RTC-memory ring (6 KB, `RTC_NOINIT`) survives crash/watchdog/software restart but **not power loss**.
  A flash log (`flash_log.cpp`, LittleFS on the `spiffs` partition, two 96 KB files) keeps warnings/errors, network
  events, a `HB` heartbeat every 15 min and a `BOOT` line (reset reason) at each start, and survives power loss.
  Both are readable at `GET /api/v1/admin/logs` and in the admin page's Device log (option "Saved" is the default).
* **How to read an outage:** heartbeats stop and then a `BOOT reset=power-on` line = dead or unpowered. Heartbeats
  continue with `tunnel=DOWN` = alive but unreachable (network). A `watchdog (the firmware hung)` BOOT = a hang.

### Home Assistant integration (`homeassistant/custom_components/cx5_controller`)

* `api.py` is HA-independent (asyncio + `cryptography`): signs requests, checks the pinned fingerprint before sending,
  TLS 1.2, one connection reused for a few seconds (challenge and request share it).
* `coordinator.py`: polls `state` every 60 s idle, 15 s while the engine runs; commands are per-control with a 1.5 s
  trailing debounce for temperature and fan, **pending / confirm / revert** behavior, one shared job poller (the device
  allows 30 challenges per minute per address), persistent notifications and events
  (`cx5_controller_command_failed`, `cx5_controller_remote_start`). Remote start is a **button** plus a status sensor.
* Entities: climate (temperature 60-84 F, fan modes 1-7; shows a placeholder 72 F target with
  `temperature_known: false` until a temperature has been set, because HA hides the controls when the target is null),
  two heater selects, two defrost switches (assumed state), remote start button and status sensor, reset-tracked button,
  engine / connectivity / remote-start-enabled binary sensors, uptime, firmware and last-command sensors, service
  `cx5_controller.resync`.
* Config flow: host, port, certificate fingerprint, key id, and a private key that may be **left empty** (HA then
  generates one and shows its public key to enroll as `control`). Docs: `docs/home-assistant.md`.
* Polling over cellular: each poll opens a new TLS connection, so idle polling is an **unmeasured estimate of 150-250
  MB/month** (about $1-2). A configurable idle interval was proposed and not built. Measure with `wg show` counters.
* The simulator `tests/mock_device.py` mirrors the firmware's API rules; when changing an endpoint, change firmware,
  simulator, tests and `docs/api.md` together.

## Hardware (decided)

* Board: **LilyGO T-SIM7670G-S3 Standard** (Cat-1). 18650 removed and not used (cold charging, hot cabin).
  PlatformIO board `esp32-s3-devkitc-1`, QIO flash (the board's PSRAM is not octal), 16 MB, `default_16MB.csv`
  (two 6.5 MB OTA slots, a `spiffs` partition used by LittleFS, a coredump partition).
* Output expander: MCP23017 (Waveshare board), I2C SDA GPIO 3, SCL GPIO 2, **address 0x27**.
* Outputs: PC817 per channel (LED side: expander pin -> 220R, 10k pulldown to GND; collector to the CCU 5 V pad,
  emitter to CCU ground). Do not put anything but these on the expander. Expander pins: temp up GPB0, temp down GPA7,
  driver heater GPA6, passenger heater GPA5, front defrost GPA4, rear defrost GPA3, fan up GPA2, fan down GPA1,
  power button GPA0 (**disabled in firmware**: `CLIMATE_POWER_ENABLED=false`, its panel line misbehaved).
* Native GPIOs: remote-start trigger 40 (ground pulse through a PC817, default two pulses of 300 ms), engine-running
  input 39. Never reuse: `4/5` modem UART, `6` RI, `7` DTR, `46` PWRKEY, `8` battery ADC, `18` solar ADC,
  `42` power-save, `2/3` I2C, `10-13` SD, `0/1/17/45/48` GPS, `19/20` USB, `26-37` flash/PSRAM. Source:
  LilyGo-Modem-Series `utilities.h`, `*_S3_STAN` block.
* Power: a cheap 12 V to 5 V buck (about 10 mA no-load) for the prototype, fused plus TVS, short tests only. The PCB
  needs a low-Iq automotive buck (tens of uA, at least 2 A, at least 40 V load-dump rating, reverse protection).
* PCB plan: a carrier board with the LilyGO module on headers (swappable), buck, TVS and fuse, MCP23017, PC817
  channels with pull-downs, 12 V sense divider (GPIO 9), ignition and running inputs, iDatastart trigger.
* Connector to the CCU housing: GX16-8 solder-cup (no crimper available).

### CCU switches (measured by the user)

Temperature dial = DPDT spring-return jog switch, 5 V pull-up, active low. Top row: pin 2 = common ground, pin 1 =
decrease, pin 3 = increase. Seat and defrost buttons are simple push buttons. Only pins 1-3 are used. No state
feedback exists (the panel talks CAN only, no LIN), which is why state is tracked. Seat heaters cycle
off -> high -> medium -> low -> off; the fan has 7 speeds (the user's number), but the temperature range defaults (60-84 F in 24
steps) and the press timing (hold/gap 150 ms) are **unmeasured on the car**.

## iDatastart HCXG0 (ADS guide 20241125; more in `docs/idatastart.md`)

* Start trigger: M3-06 WHITE/BLUE external RS trigger input, ground pulse, **disabled by default**. Enable option
  1-16, use the double-pulse setting. Pulse width unspecified.
* Engine running: the module's POC1 (M2-08) programmed to **F24 "GND when engine running"** (what is wired). The
  alternative is M2-06 "ground while running". Key-on: a POC set to F31. Run time via option 1-03 (3 to 35 min).
* Needs brake, hood and engine-sense inputs wired for remote start to be allowed.

## Decisions and lessons (so they are not re-litigated)

* **No MQTT, no Wi-Fi, no light sleep.** The modem (about 4-8 mA connected-idle) is the baseline; the ESP32 stays awake.
* **GPS and live signal strength were tried and abandoned.** The modem takes no AT commands while PPP carries data, and
  the CMUX mode that would allow it fails on this modem (`esp_modem_set_mode` returns ESP_FAIL on CMUX_MANUAL).
  LilyGO's own examples read GPS NMEA over the same UART (the GPIO 48/45 pins are for an external GPS module), so
  there is no side door. If ever wanted: drop the data link on demand (engine stop or a "Locate" button), read GNSS
  in the existing pre-PPP AT phase (`logModemInfo` in `net_cell.cpp` already runs AT commands before each connect),
  then reconnect; about a minute offline, and GNSS draws power only during that window.
* The **Arduino core auto-confirms a new OTA image at startup** unless `verifyRollbackLater()` returns true; `ota.cpp`
  overrides it so rollback works. The new image is confirmed only after cellular, tunnel and API are healthy 3 minutes.
* The ESP-IDF 5 lwIP core needs `LOCK_TCPIP_CORE()` around netif/udp calls from application tasks (patched in the
  WireGuard library). The IDF server's dual-stack socket shows IPv4 clients as IPv4-mapped IPv6 (`v4Of` in `api.cpp`).
* NVS reads of a missing key log scary errors unless guarded with `isKey()`.
* After a firmware crash with PPP open, the modem stays in data mode and ignores AT; the code escapes with "+++" and,
  failing that, power-cycles it with PWRKEY (100 ms pulse on, about 3 s to switch off).
* `tools/embed_web.py` (a PlatformIO pre-script) gzips `web/admin.html` into the untracked `src/admin_page.gen.h`.
* The admin page serializes its API calls because the device has two connection slots.

## Status and open problems

* **Working in the car:** climate presses, remote start with engine confirmation, cellular, WireGuard, signed API,
  Home Assistant integration.
* **Written, built, not verified on the device:** admin page, tracked state persistence, OTA, link watchdog, hang
  watchdog, flash log. The last flashed build may predate some of these; after reflashing, wait for the first `HB` line
  in the Saved log to confirm the flash log works.
* **OTA problem (open):** an upload from the admin page ended with "Failed: not receiving". An unsigned time compare in
  the stall check was fixed and the log now says why an update was inactive, but this is unconfirmed. The browser's
  progress bar hits 100% long before the device finishes (the modem link is about 10 KB/s). Ask for the Device log
  lines from the first `OTA:` entry. Because the fix lives in the new firmware, **the next update that fixes it must be
  a USB flash**.
* **Outages:** the device was unreachable for days twice. First: a router firmware upgrade (explained, watchdog now
  retries the tunnel). Second: 4 days, **cause unknown**; the old log was lost when the user unplugged the device. The
  flash log exists to answer this next time.
* Unresolved: an ESP32 reboot at engine start (likely supply dip or ground loop); the reset reason is logged, stored in
  the flash log and shown on the admin page.
* Not started: battery-voltage lockout (12 V sense divider on GPIO 9), CAN read-only telemetry, custom carrier PCB,
  faster modem UART for OTA (`AT+IPR=921600`, then `PPP.begin` with the new baud).

## Open items

* Flash the current build over USB and verify on the car: `state` fields, fan homing counts, persistence across a
  reboot, engine-stop reset of heaters/defrost, the admin page, the Saved log, an OTA of a new build and of a
  deliberately bad one (rollback).
* Measure on the car: pulse/gap timing, dial range, whether the first press only wakes the display, seat/defrost
  cycle and reset behavior.
* Measure real data use (`wg show` counters) and decide whether to make the Home Assistant idle poll interval a setting.
* Measure the car-side current draw.
* Commit or back up the KiCad `hardware/` folder (ask the user).
* Update `docs/wireguard-server.md` to the OpenWrt reality (and drop its Wi-Fi remarks) if the user wants.
* Optional recon: get the CCU connector wiring diagram, back-probe for CAN/LIN, passively log CAN at the OBD port
  (listen-only) while using the buttons and Mazda's app. The earlier claim that CAN injection cannot work is unverified.

## Conventions

* No secrets in the repo; secrets files are listed in `.gitignore`. Verify before every push (`git ls-files | grep -iE
  "secret|tls_cert|\.pem"` should show only `secrets.example.h`).
* Commit only when the user asks. Commit messages end with the attribution line the harness provides.
* Keep `docs/api.md`, the simulator and the tests in step with firmware API changes.
