# CLAUDE.md: CX-5 climate remote (ESP32-S3 + SIM7670G)

Project state and decisions, so a fresh session (for example on another machine) can
continue without the original conversation.

## Goal

Cellular remote start and climate pre-conditioning for a 2022 Mazda CX-5, independent
of Mazda's subscription. An ESP32-S3 drives PC817 optocouplers that "press" the
climate control unit (CCU) switch pads, and triggers an iDatastart HCXG0 remote-start
module. Budget: under $10/month for connectivity. Prototype first, custom PCB later.

## Status

* Phase 1 (bench test firmware, local buttons + serial console): written, **builds**,
  not yet run on hardware. Test plan in `docs/bench-test.md`.
* Phase 2 (SIM7670G always-connected MQTT with ESP32 light sleep, signed commands,
  telemetry): **not started on purpose.** The user wants hardware proven first. Do not
  start modem work until asked.
* Phase 3 (iDatastart trigger and running-status inputs, battery lockout): not started.
* Future: CAN bus read-only telemetry; custom carrier PCB.

## Hardware (decided)

* Board: **LilyGO T-SIM7670G-S3 Standard** (Cat-1, not Cat-M). 18650 removed and not
  used (cold charging, hot cabin, unknown charger/NTC).
* Output expander: MCP23017 on I2C (SDA GPIO 3, SCL GPIO 2, address 0x20).
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
free: `40, 41, 47`. Source: LilyGo-Modem-Series `utilities.h` (`*_S3_STAN` block).

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

* **Connectivity: option A, always connected.** The modem registers and holds the MQTT
  connection (keepalive 60-120 s, TLS, last-will for presence, QoS 1, retained commands);
  the ESP32-S3 sits in **light sleep** and wakes on modem UART/RI activity. No SMS in the
  normal path: the user found per-SMS pricing too high (some plans include only ~250
  lifetime). Deep sleep is not required: the modem (about 4-8 mA connected-idle) is the
  baseline, not the ESP32. Estimated about 5 mA from the car battery with a good buck.
  Rejected: SMS wake (cost), timer polling (latency or data), voice-call ring (IoT SIMs
  rarely have voice). Optional later: scheduled RTC wake around the usual departure
  time, home Wi-Fi when parked.
* SMS is only an emergency wake, if at all: when resting battery is below ~12.0 V the
  device drops to timer-only wake.
* **Command security: signed-only**, Ed25519, strictly increasing counter in NVS, rate
  limit. HA holds the private key, the car only the public key. Commands travel inside
  MQTT payloads over TLS with a broker ACL limited to the car's topics. No encryption
  layer. Details in `docs/design-notes.md`.
* Data budget: about 150 B/min keepalive (about 6.5 MB/month) plus telemetry; Telnyx at
  $0.0078/MB makes data pennies. Enforce a **daily byte budget in NVS**, exponential
  reconnect backoff, batch telemetry, and stream CAN only while driving (24/7 at 1 Hz
  would be about $4.40/month).
* Awake policy: stay fully awake while engine running (GWR / ignition input); otherwise
  light sleep with the connection held. 12 V sense divider for a low-battery lockout
  (~12.0 V at rest).
* Temperature is **absolute**: press down enough to clamp at LO, then count up.
  Calibrated on the car with `tempcal`. Defaults (60-84 F in 24 steps) are unmeasured.
* FreeRTOS: output pulsing on core 1, networking on core 0.
* Fail-safe rules (keep): outputs never held (500 ms cap, retry-to-low on I2C error),
  refuse commands if the expander is missing, queue is cleared by `stop`.

## Build

Developed under WSL2 with PlatformIO (venv `~/.venvs/pio`). Keep a copy on the Linux
filesystem for speed:

```bash
rsync -a --delete platformio.ini src ~/esp32-mazda-controller/
cd ~/esp32-mazda-controller && ~/.venvs/pio/bin/pio run
```

Flash from the host OS with `esptool` (see README), because `usbipd` and the S3's
native USB re-enumeration are unreliable. On macOS/Linux, `pio run -t upload` works
directly.

## Open items

* Verify GPIO 9, 14, 21 are broken out on the user's headers.
* Measure on the car: pulse/gap timing, dial range, whether the first press only wakes
  the display, seat/defrost cycle and reset behavior.
* Choose carrier (candidates: Telnyx at $0.0078/MB and about $2/SIM/month, Hologram,
  Twilio Super SIM); confirm Cat-1 bands for the user's module variant, data-session
  idle timeouts and billing increments. Inbound SMS is no longer required.
* **First hardware test for phase 2:** with the modem holding an MQTT connection (AT
  MQTT stack or TinyGSM socket) and the ESP32 in light sleep, confirm an incoming
  message raises RI (GPIO 6) or UART activity, and measure real idle current.
* Pick a broker reachable from the car (VPS Mosquitto or hosted broker); the car and HA
  both connect outbound. Tailscale/WireGuard on the ESP32 was dropped as too complex.
* Network recon (optional, for a broader product): get the CCU connector wiring diagram,
  back-probe for CAN/LIN, and passively log CAN at the OBD port (listen-only) while using
  the buttons and Mazda's app remote start. Keep the Mazda subscription until captured.
  Firmware dumping was judged low value. The earlier claim that CAN injection cannot
  work is unverified.
* Confirm the board charger/NTC situation if an 18650 is ever reconsidered.

## Conventions

* No secrets in the repo: use an untracked `src/secrets.h`.
* Commit only when the user asks.
