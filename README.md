# ESP32 Climate Remote for the Mazda CX-5

> **NOTE:** This project (and most of this README) was developed heavily with AI assistance (Claude Code). If you have a problem with AI, I totally understand. Feel free to use this as a reference and build your own version, or stop reading here. 
> 
> I know a good bit of programming, but I have very little experience with embedded platforms and am frankly out of practice. I would expect bugs and AI nonsense if you decide to use this. Once I actually get around to building the hardware side of this project, I plan to review the code and try to weed out any weird AI quirks that may exist.
>
> Also, I know this README kinda blows because Claude wrote it. I made some edits, but it is still kind of difficult to follow.

Remote control for a car's climate controls, built around an ESP32-S3 and a cellular
modem. The goal is to warm up (or cool down) the cabin before you get in, without paying
a monthly subscription to the manufacturer.

> **Status: early prototype.** The bench-test firmware is written and compiles, but it
> has not yet been tested in a car. Cellular connectivity and the remote-start trigger
> are planned but not built. See [Roadmap](#roadmap).

## What it does

The firmware presses the buttons on your car's climate control panel for you. A small
microcontroller drives optocouplers that connect directly to the panel's switch pads, so
the panel sees the same signal it would see from a finger on the button.

It can currently:

- Raise or lower the driver's temperature setting, one step at a time
- Set an exact temperature (for example `temp 72`)
- Press the driver and passenger seat heater buttons
- Press the front and rear defrost buttons
- Accept commands from physical buttons on a breadboard or from a USB serial console

Planned (not yet implemented): trigger a remote-start module, receive commands over a cellular connection with MQTT and Home Assistant, implement CAN bus reading which can be broadcast over an on-demand wifi network, and report battery voltage and status.

## Why this approach

Mazda keeps their climate settings on an internal network. I couldn't find a way to send update signals through CAN or LIN to change the values. Simulating a button press is the only way I could figure out how to make this work, and it is probably the easiest route anyways. The downside is that you must physically wire into the climate panel, which means soldering fine wires to its circuit board.

The target vehicle is a 2022 Mazda CX-5 Turbo Signature with heated and ventilated seats and automatic climate control. Other vehicles may work if their buttons are simple switches, but you will have to work out the wiring
yourself.

## How it works

```
 Command source                 ESP32-S3                    Climate panel
 --------------                 --------                    -------------
 Serial console  ----\                                 /--> Temperature up/down
 Bench buttons   -----+--> command queue --> I2C       |--> Seat heater (driver)
 (later) MQTT    ----/      (FreeRTOS task)   expander -+--> Seat heater (passenger)
                                              + opto-   |--> Front defrost
                                              couplers  \--> Rear defrost
```

Each button is "pressed" by briefly turning on an optocoupler (a PC817) that connects
the button's two pads together. An I2C expander (MCP23017) provides the output pins, so
adding more buttons later does not use up the ESP32's GPIOs.

Because the car does not report its current setpoint (as far as I currently know), temperature is set by pressing
down enough times to reach the lowest setting, then pressing up the right number of
times. This takes a few seconds and needs a one-time calibration (see below).

## What you need

### Hardware

| Item | Notes |
|---|---|
| LilyGO T-SIM7670G-S3 **Standard** board | ESP32-S3 with a cellular modem. The modem is not used yet. Remove the 18650 battery holder. |
| MCP23017 I2C expander breakout | Provides the output pins |
| 6 x PC817 optocouplers | One per button, or two 4-channel PC847 chips |
| 6 x 220 ohm and 6 x 10 kohm resistors | LED resistors and pull-downs |
| 6 x momentary push buttons | Bench testing only |
| 30 AWG wire | Thin and flexible so you do not lift pads off the panel |

For a car installation you will also need:

| Item | Notes |
|---|---|
| 12 V to 5 V buck converter | Choose one with very low idle current (under 1 mA) if the car will sit for days. A cheap module can draw 10 mA and slowly drain the battery. |
| Fuse (about 1A) and TVS diode | Protects against shorts and voltage spikes |
| Connector for the panel housing (optional) | A GX16 8-pin solder-type connector works without special crimping tools. |

### Software

- [PlatformIO](https://platformio.org/) (command line or the VS Code extension)
- Git

## Building the hardware

> Always start on the bench with LEDs instead of the car. Only connect to the climate
> panel once the bench tests below pass.

### 1. Wire one output channel

Each channel looks like this:

```
Expander pin --[220 ohm]--> PC817 pin 1 (LED anode)
Expander pin --[10 kohm]--> GND        (keeps the output off while powering up)
PC817 pin 2 (LED cathode) --> GND
PC817 pin 4 (collector)   --> panel pad (the pad that sits at 5 V)
PC817 pin 3 (emitter)     --> panel ground pad
```

For bench testing, replace the panel pads with a 5 V source, a 1 kohm resistor, and an
LED, so you can see each press.

### 2. Connect the expander

| MCP23017 | Connects to |
|---|---|
| VDD / RESET | 3.3 V |
| VSS | GND |
| A0, A1, A2 | GND (address 0x20) |
| SDA | GPIO 3 |
| SCL | GPIO 2 |

### 3. Assign the channels

| Function | Expander pin | Bench button GPIO |
|---|---|---|
| Temperature up | GPA0 | 15 |
| Temperature down | GPA1 | 16 |
| Driver seat heater | GPA2 | 14 |
| Passenger seat heater | GPA3 | 21 |
| Front defrost | GPA4 | 38 |
| Rear defrost | GPA5 | 39 |

Wire each bench button between its GPIO and GND. An optional 12 V battery sense uses
GPIO 9 through a 220 kohm / 47 kohm divider and a 100 nF capacitor.

These pins are for the Standard version of the LilyGO board. Do not use GPIOs 4, 5, 6,
7, 8, 42 or 46: they are connected to the modem and other on-board parts. Check that
GPIOs 9, 14 and 21 are brought out on your board's headers, or change them in
[`src/pins.h`](src/pins.h).

### 4. Connect to the climate panel (later)

The temperature dial on the CX-5 is a small spring-return switch with three pads in a
row on the top side. With a multimeter, confirm which pad is ground and which pads drop
to 0 V when pressed. The other buttons are simple two-pad push buttons. Connect one
optocoupler per button: collector to the pad that sits at 5 V, emitter to the ground pad.

Pad voltages and layouts differ between models and model years. Measure before you
connect anything.

## Building the software

PlatformIO runs on Windows, macOS and Linux. The project has been built on Linux,
and a Linux or macOS machine is the easiest place to build it.

```bash
git clone <this-repository-url>
cd esp32-mazda-controller
pio run                  # build
pio run -t upload        # flash the board over USB
pio device monitor       # open the serial console at 115200 baud
```

If the upload fails, put the board into download mode: hold BOOT, tap RESET, then
release BOOT, and try again.

### Using the serial console

| Command | What it does |
|---|---|
| `temp 72` | Set an absolute temperature in degrees F |
| `temp lo` / `temp hi` | Go to the lowest or highest setting |
| `up [n]` / `down [n]` | Step the temperature up or down n times |
| `seatd [n]` / `seatp [n]` | Press the driver or passenger seat heater button |
| `deff [n]` / `defr [n]` | Press the front or rear defrost button |
| `press <ms>` / `gap <ms>` | Set how long each press lasts and the pause between presses |
| `tempcal <minF> <maxF> <steps> [loOffset]` | Calibrate the temperature range |
| `stop` | Abort whatever is running |
| `status` / `help` | Show settings or the command list |

### Calibrating temperature

The defaults (60 to 84 F in 24 steps) are only a starting guess. In the car:

1. Run `temp lo`, then `up` one press at a time and note the first and last
   temperatures shown and how many presses it takes.
2. Enter them with `tempcal <min> <max> <steps>`. If the lowest setting is a separate
   "LO" position below the first number, pass the number of extra presses as `loOffset`.
3. Test with `temp 72` and adjust.

Calibration is saved on the board and survives a restart.

## Testing

A step-by-step test plan lives in [`docs/bench-test.md`](docs/bench-test.md). It starts
with verifying the board boots and finds the expander, then each channel using LEDs,
reset glitch checks, the buttons, absolute temperature, and finally the car.

## Roadmap

1. **Bench and in-car testing** of the current firmware (in progress)
2. **Cellular connection:** the modem stays connected and the ESP32 sleeps lightly, so
   commands arrive over MQTT within seconds without SMS charges
3. **Signed commands:** every message is digitally signed so nobody else can control
   the car
4. **Remote start trigger** for an aftermarket module, and engine-running detection
5. **Home Assistant integration** with buttons and status
6. **Battery protection:** report battery voltage and refuse commands when it is low
7. **Vehicle data over CAN** for temperature feedback and telemetry
8. **Climate data over LIN** (maybe, not sure if this is even possible yet)
8. **A custom PCB** that carries the LilyGO board, so no breadboard is needed

## Project layout

```
src/main.cpp      Firmware: command queue, buttons, serial console
src/mcp23017.h    Small driver for the I2C expander
src/pins.h        Pin assignments
src/config.h      Timing and safety limits
docs/             Bench test plan and design notes
platformio.ini    Build configuration
```

Design decisions, the security model, and the reasoning behind the connectivity choices
are written up in [`docs/design-notes.md`](docs/design-notes.md).

## Contributing

Issues and suggestions are welcome, especially wiring notes and test results for other
vehicles. If you try this on another car, please record what you measured.

## License

No license has been chosen yet.
