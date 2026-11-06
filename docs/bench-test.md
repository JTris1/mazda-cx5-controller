# Bench test plan (phase 1)

Goal: prove the optocoupler channels, timing and absolute-temperature logic before any
SIM work and before touching the CCU. Modem code is deliberately out of scope.

## Parts

* LilyGO T-SIM7670G-S3 Standard (18650 removed), USB cable
* MCP23017 breakout, breadboard
* 6x PC817, 6x 220R (LED side), 6x 10k pulldown (expander pin to GND)
* 6x momentary push buttons (bench inputs)
* 6x LED + 1k, 5 V source (USB 5 V pin is fine) as a stand-in "CCU pad" per channel
* Multimeter; a logic analyzer or scope is nice for pulse width but not required

## Stage 0: boot

1. Flash the build, open the serial console at 115200.
2. Expect `expander: ok` and the help text. If it says `NOT FOUND`: check SDA = GPIO 3,
   SCL = GPIO 2, address pins to GND, RESET to 3V3, pull-ups, and power.
3. Pull SDA off the expander and reset: expect `refusing command` on every command.

## Stage 1: channels

For each channel wire: 5 V -> 1k -> LED -> PC817 collector (pin 4), emitter (pin 3) -> GND.

| Command | Expect |
|---|---|
| `up 3` | channel 0 LED blinks 3 times, ~80 ms on / ~150 ms off |
| `down 3` | channel 1 only |
| `seatd`, `seatp`, `deff`, `defr` | one blink on the matching channel only |
| `up 99` | clamped to 60 blinks (use `stop` to abort) |
| `stop` mid-sequence | blinking ceases within one press, queue cleared |

Pass criteria: no channel ever stays lit; no cross-talk between channels.

## Stage 2: glitch and fail-safe

1. Reset the board 20 times with the LED loads connected: no LED may flash on reset.
2. Power-cycle the board with the USB cable: same.
3. During `up 40`, pull SDA: the sequence aborts and prints an I2C failure; on reconnect
   all LEDs are off.

## Stage 3: buttons

Each bench button queues exactly one press of its channel. A held button must not
repeat. Contact bounce must not double-fire (try pressing quickly 20 times).

## Stage 4: absolute temperature (still on the LED stand-in)

1. `tempcal 60 84 24`, then `temp 72`: count blinks on temp-down (homing, `homePresses`)
   then temp-up (expect 12).
2. `temp lo` = homing only; `temp hi` = homePresses on temp-up only.
3. Reboot: `status` still shows the saved calibration.

## Stage 5: on the car (ignition on, engine off, parked, garage)

Before connecting, with the CCU powered and a multimeter:

* Confirm the idle voltage on each pad is about 5 V and which pad is the common ground
  (re-check the dial: pin 2 = ground, pin 1 = down, pin 3 = up).
* Confirm opto polarity: collector to the 5 V pad, emitter to the ground pad.

Then, using 30 AWG wire, one channel at a time:

1. `up 1` / `down 1`: does the setpoint move by exactly one step? Tune `press` and
   `gap` (smallest values that never drop a click, then add 30 % margin).
2. Does the first press after the display sleeps only wake it? If so we add a wake press.
3. Measure the dial: after `temp lo`, press `up` and note the first and last displayed
   values and the number of presses; enter them with `tempcal`.
4. Seat and defrost: one press = what? Does the state reset after ignition off/on?
5. Record results in this file.

## Results log

| Date | Item | Result |
|---|---|---|
| | | |
