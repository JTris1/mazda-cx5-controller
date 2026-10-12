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
* Phase 2 (SIM7670G SMS wake from deep sleep, signed commands, MQTT telemetry): **not
  started on purpose.** The user wants hardware proven first. Do not start modem work
  until asked.
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

* Wake path: SMS to the SIM, modem RI (GPIO 6) pulls low, ESP32 wakes from deep sleep.
* **Command security: signed-only**, Ed25519, strictly increasing counter in NVS, rate
  limit, one SMS segment. HA holds the private key, the car only the public key. No
  encryption layer. MQTT is outbound telemetry only. Details in `docs/design-notes.md`.
* Awake policy: stay awake while engine running (GWR / ignition input), sleep within
  2 minutes after it stops. 12 V sense divider for a low-battery lockout (~12.0 V at rest).
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
* Choose carrier and SMS sender (candidates: Telnyx, Hologram, Twilio Super SIM);
  confirm inbound SMS and Cat-1 bands. Prices unverified.
* Confirm the board charger/NTC situation if an 18650 is ever reconsidered.

## Conventions

* No secrets in the repo: use an untracked `src/secrets.h`.
* Commit only when the user asks.
