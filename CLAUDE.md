# CLAUDE.md: CX-5 climate remote (ESP32-S3 + SIM7670G)

Project state and decisions, so a fresh session (for example on another machine) can
continue without the original conversation.

## Goal

Cellular remote start and climate pre-conditioning for a 2022 Mazda CX-5, independent
of Mazda's subscription. An ESP32-S3 drives PC817 optocouplers that "press" the
climate control unit (CCU) switch pads, and triggers an iDatastart HCXG0 remote-start
module. Budget: under $10/month for connectivity. Prototype first, custom PCB later.

## Status

The firmware lives in a separate PlatformIO project: `~/Documents/PlatformIO/Projects/Mazda-CX-5-Controller`
(this folder holds the KiCad work in `hardware/`). Its `docs/` has `api.md`, `wireguard-server.md`,
`home-assistant.md` and `admin-page.md`.

* **Working on hardware:** serial console, climate outputs through the MCP23017, remote start (iDatastart
  trigger on GPIO 40, engine-running input on GPIO 39 from the module's POC1 programmed to F24), cellular (PPP
  over the SIM7670G, Telnyx APN `data00.telnyx`), WireGuard tunnel to the user's OpenWrt router (ESP32 tunnel
  IP 192.168.10.2), HTTPS API with ECDSA P-256 signed requests.
* **Written and built, not yet verified on hardware:** tracked climate state in flash, absolute fan, persistent
  settings, expanded `state`, `assume`, admin API, `/admin` page, OTA with rollback. The Python mock device
  (`homeassistant/tests/`) passes 70 tests and the admin page was driven in a browser against it.
* **Home Assistant integration** (`homeassistant/custom_components/cx5_controller`): written and tested against the
  mock, not yet installed on the user's Home Assistant OS box.
* Climate power button: disabled in firmware (`CLIMATE_POWER_ENABLED`); its panel line misbehaved.
* Unresolved: an ESP32 reboot at engine start (likely supply dip or ground loop; the reset reason is now logged
  at boot and shown on the admin page).
* Not started: battery-voltage lockout, CAN read-only telemetry, custom carrier PCB, faster modem UART for OTA.

## Hardware (decided)

* Board: **LilyGO T-SIM7670G-S3 Standard** (Cat-1, not Cat-M). 18650 removed and not
  used (cold charging, hot cabin, unknown charger/NTC).
* Output expander: MCP23017 on I2C (SDA GPIO 3, SCL GPIO 2, address 0x27 on the Waveshare board).
* Outputs: PC817 (LED side: expander pin -> 220R, 10k pulldown to GND; collector to the
  CCU 5 V pad, emitter to CCU ground). Do not put anything but these on the expander.
* Power: cheap 12 V -> 5 V buck for the prototype (10 mA no-load, bench and short tests
  only, fused + TVS). The PCB gets a low-Iq automotive buck.
* PCB plan: carrier board with the LilyGO module on headers (swappable).
* Connector to the CCU housing: GX16-8 solder-cup (no crimper available).

### Pin map (Standard board). Never reuse these

`4/5` modem UART, `6` RI, `7` DTR, `46` PWRKEY, `8` battery ADC, `18` solar ADC,
`42` power-save, `2/3` I2C, `10-13` SD, `0/1/17/45/48` GPS, `19/20` USB, `26-37`
flash/PSRAM. Free natives used: `15,16,14,21,38,39` buttons, `9` 12 V sense. Still
free: `41, 47` (40 = remote start trigger, 39 = engine-running input). Source: LilyGo-Modem-Series `utilities.h` (`*_S3_STAN` block).

### CCU switches (measured by the user)

Temperature dial = DPDT spring-return jog switch, 5 V pull-up, active low. Top row:
pin 2 = common ground, pin 1 = decrease, pin 3 = increase. Seat and defrost buttons are
simple push buttons. Only pins 1-3 are used. No state feedback exists until CAN.

## iDatastart HCXG0 (ADS guide 20241125)

* Start trigger: M3-06 WHITE/BLUE external RS trigger input, ground pulse, **disabled by
  default**. Enable option 1-16, use the double-pulse setting. Pulse width unspecified.
* Engine running: M2-06 BLUE/WHITE "ground while running" (250 mA, (-) output).
  Key-on: a POC set to F31. Run time via option 1-03 (3 to 35 min).
* Needs brake, hood and engine-sense inputs wired for remote start to be allowed.

## Architecture decisions

* **Connectivity: PPP over the modem, WireGuard, HTTPS.** MQTT and light sleep were dropped. The ESP32 brings up
  PPP on the SIM7670G (Arduino core 3.x, pioarduino), starts a WireGuard tunnel to the home router, and serves an
  HTTPS API **only on the tunnel address**. Home Assistant (and the admin page) reach it through the tunnel and
  poll, because the device's firewall forbids it from opening connections to other peers. Wi-Fi was removed.
* **Command security: per-device signed requests.** ECDSA P-256 over a one-time server nonce, `control` and
  `admin` roles, keys held as public keys on the device (up to 8), per-address failure blocking, remote start
  disabled until enabled, with its own cooldown and hourly/daily limits. Details in the firmware repo's
  `docs/api.md`. The first admin key can be enrolled from the admin page while the key store is empty (accepted
  risk, documented).
* **Tracked state.** The car reports nothing (CAN only, no LIN), so heaters, defrost, temperature and fan are the
  *last commanded* values, kept in flash, reset to off when the engine stops (setting), correctable with `assume`.
* Data budget: HA polling every 60 s idle opens a TLS connection each time: an unmeasured estimate of 150-250 MB/month (about $1-2 at Telnyx's $0.0078/MB). Measure with `wg show` transfer counters before trusting it.
* OTA: signed manifest, streamed to the spare slot, SHA-256 checked, boots on probation and rolls back unless
  cellular, tunnel and API stay healthy for 3 minutes. Modem UART is 115200 baud (about 10 KB/s), so an update takes
  minutes until the baud rate is raised.
* Temperature is **absolute**: press down enough to clamp at LO, then count up.
  Calibrated on the car with `tempcal`. Defaults (60-84 F in 24 steps) are unmeasured.
* FreeRTOS: output pulsing on core 1, networking on core 0.
* Fail-safe rules (keep): outputs never held (500 ms cap, retry-to-low on I2C error),
  refuse commands if the expander is missing, queue is cleared by `stop`.

## Build

In the firmware project: `~/.platformio/penv/bin/pio run` (macOS). `tools/embed_web.py` gzips `web/admin.html`
into `src/admin_page.gen.h` on every build. Secrets are untracked: `src/secrets.h` (WireGuard keys) and
`src/tls_cert.h` (run `tools/gen_cert.sh`). Python tests: `homeassistant/requirements_test.txt`, then
`python -m pytest` inside `homeassistant/`. On macOS/Linux `pio run -t upload` works; on WSL2 flash from the host
with `esptool`.

## Open items

* Verify GPIO 9, 14, 21 are broken out on the user's headers.
* Measure on the car: pulse/gap timing, dial range, whether the first press only wakes
  the display, seat/defrost cycle and reset behavior.
* Choose carrier (candidates: Telnyx at $0.0078/MB and about $2/SIM/month, Hologram,
  Twilio Super SIM); confirm Cat-1 bands for the user's module variant, data-session
  idle timeouts and billing increments. Inbound SMS is not used; Telnyx data is working.
* **Verify on hardware:** flash the current firmware and check `state` (tracked fields), fan homing counts,
  persistence across a reboot, engine-stop reset, the admin page, then an OTA of a new build and a deliberately
  bad one (rollback). Then install the Home Assistant integration.
* Measure the real current draw of the car-side install, and reduce it if needed (the always-connected modem is the
  baseline; light sleep was dropped).
* Raise the modem UART baud (`AT+IPR=921600`, then `PPP.begin` with the new baud) to speed up OTA.
* Network recon (optional, for a broader product): get the CCU connector wiring diagram,
  back-probe for CAN/LIN, and passively log CAN at the OBD port (listen-only) while using
  the buttons and Mazda's app remote start. Keep the Mazda subscription until captured.
  Firmware dumping was judged low value. The earlier claim that CAN injection cannot
  work is unverified.
* Confirm the board charger/NTC situation if an 18650 is ever reconsidered.

## Conventions

* No secrets in the repo: use an untracked `src/secrets.h`.
* Commit only when the user asks.
