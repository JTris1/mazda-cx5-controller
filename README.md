# ESP32 Climate Control for the Mazda CX-5

> [!WARNING]
> This project (and most of this README) was developed heavily with AI assistance (Claude Code). If you have a problem with AI, I totally understand. Feel free to use this as a reference and build your own version, or stop reading here. 
> 
> I know a good bit of programming, but I have very little experience with embedded platforms and am frankly out of practice. I would expect bugs and AI nonsense if you decide to use this. Once I actually get around to building the hardware side of this project, I plan to review the code and try to weed out any weird AI quirks that may exist.
>
> Also, I know this README kinda blows because Claude wrote it. I made some edits, but it is still kind of difficult to follow.

Remote control for the Mazda CX-5's climate controls and an aftermarket remote starter, built around an ESP32-S3
and a cellular modem. The goal is to warm up (or cool down) the cabin before you get in, without paying a monthly
subscription to the manufacturer. You control it from Home Assistant or from a small admin web page that the device
serves itself.

> [!NOTE]
> **Status: installed in a car and working, still a prototype.** Cellular, the VPN tunnel, the signed API, the
> climate buttons, remote start and the Home Assistant integration work. Some newer features (the self-repair
> watchdog, the flash log, over-the-air updates) are written and tested against a simulator but not yet proven on
> the real device. See [Status](#status) and the [Build Log](#build-log).

## What it does

- Presses the climate panel's buttons for you: temperature (an absolute value, 60-84 F), fan speed (1-7), driver and
  passenger seat heaters, front and rear defrost
- Triggers an iDatastart remote-start module and confirms that the engine actually started
- Lives on a cellular SIM (no Wi-Fi), reachable only through a WireGuard tunnel to your home network
- Accepts only cryptographically signed requests (per-device keys, so a stolen phone can be revoked)
- Home Assistant integration with a thermostat, fan, seat heater, defrost, remote-start button and status sensors
- A built-in admin page: manage keys, change settings, read logs, push firmware updates over the air
- Repairs itself when the network drops (restarts the tunnel, the cellular link, or the device) and keeps a log in
  flash that survives a power cut

## Why this approach

Mazda keeps their climate settings on an internal network. I couldn't find a way to send update signals through CAN or LIN to change the values. Simulating a button press is the only way I could figure out how to make this work, and it is probably the easiest route anyways. The downside is that you must physically wire into the climate panel, which means soldering fine wires to its circuit board.

Because the panel can't report its state, the device remembers what it **last commanded** and shows that. Temperature
and fan are set by pressing down to the lowest value and then up to the target, so they are always right. Seat
heaters and defrost can drift if someone uses the physical buttons; Home Assistant has a "resync" service for that.

The target vehicle is a 2022 Mazda CX-5 Turbo Signature with heated and ventilated seats and automatic climate control. Other vehicles may work if their buttons are simple switches, but you will have to work out the wiring yourself.

## How it works

```
 Home Assistant ----\                          (home network)
 Admin web page -----+--> WireGuard tunnel --> router ... internet ... cell tower
 Python client  ----/                                                       |
                                                                      SIM7670G modem
 ESP32-S3: HTTPS API (signed requests) -> job queue -> I2C expander -> PC817 optocouplers -> climate panel pads
                                       \-> trigger output ------------> remote-start module
                                        <- engine-running input <------ remote-start module
```

Each button is "pressed" by briefly turning on an optocoupler (a PC817) that connects the button's two pads together.
An I2C expander (MCP23017) provides the output pins. The ESP32's HTTPS server listens only on its WireGuard address.
Every request carries a signature over a one-time nonce (ECDSA P-256), checked against the public keys stored on the
device. The full protocol is in [`docs/api.md`](docs/api.md).

## What you need

### Hardware

| Item | Notes |
|---|---|
| LilyGO T-SIM7670G-S3 **Standard** board | ESP32-S3 with a cellular modem and GPS. Remove the 18650 battery holder. |
| Cellular SIM | Any IoT data SIM. I use Telnyx (APN `data00.telnyx`), about $2 a month plus data. |
| MCP23017 I2C expander breakout | I use a Waveshare board (address 0x27) |
| PC817 optocouplers | One per button (8 channels in use, 9 counting the disabled power button, see below), or PC847 4-channel chips |
| 220 ohm and 10 kohm resistors | LED resistors and pull-downs, one of each per channel |
| A computer or router that runs WireGuard | I use an OpenWrt router, see [`docs/wireguard-server.md`](docs/wireguard-server.md) |
| An iDatastart HCXG0 remote-start module (optional) | See [`docs/idatastart.md`](docs/idatastart.md) |
| 30 AWG wire | Thin and flexible so you do not lift pads off the panel |

For a car installation you will also need:

| Item | Notes |
|---|---|
| 12 V to 5 V buck converter | Choose one with very low idle current (under 1 mA) if the car will sit for days. A cheap module can draw 10 mA and slowly drain the battery. |
| Fuse (about 1A) and TVS diode | Protects against shorts and voltage spikes |
| Connector for the panel housing (optional) | A GX16 8-pin solder-type connector works without special crimping tools. |

### Software

- [PlatformIO](https://platformio.org/) (command line or the VS Code extension). Its ESP32 platform needs
  Python 3.10-3.13 on the machine that runs it.
- Git, OpenSSL (for the certificate script)
- Python 3.14 and `pip` if you want to run the Home Assistant tests (they use Home Assistant's own test package)

## Building the hardware

> Always start on the bench with LEDs instead of the car. Only connect to the climate
> panel once each output behaves.

### 1. Wire one output channel

Each channel looks like this:

```
Expander pin --[220 ohm]--> PC817 pin 1 (LED anode)
Expander pin --[10 kohm]--> GND        (keeps the output off while powering up)
PC817 pin 2 (LED cathode) --> GND
PC817 pin 4 (collector)   --> panel pad (the pad that sits at 5 V)
PC817 pin 3 (emitter)     --> panel ground pad
```

For bench testing, replace the panel pads with a 5 V source, a 1 kohm resistor, and an LED, so you can see each press.

### 2. Connect the expander

| MCP23017 | Connects to |
|---|---|
| VDD / RESET | 3.3 V |
| VSS | GND |
| A0, A1, A2 | per your board (the firmware expects address 0x27, set in `src/config.h`) |
| SDA | GPIO 3 |
| SCL | GPIO 2 |

### 3. Channels and other pins

| Function | Expander pin |
|---|---|
| Temperature up / down | GPB0 / GPA7 |
| Driver / passenger seat heater | GPA6 / GPA5 |
| Front / rear defrost | GPA4 / GPA3 |
| Fan speed up / down | GPA2 / GPA1 |
| Climate power button (disabled in firmware) | GPA0 |

| Function | ESP32 GPIO |
|---|---|
| Remote-start trigger output (to a PC817) | 40 |
| Engine-running input (module's "ground while running", with a series resistor) | 39 |

These pins are for the Standard version of the LilyGO board. Do not use GPIOs 4, 5, 6, 7, 8, 42 or 46: they are
connected to the modem and other on-board parts. All pins are defined in [`src/config.h`](src/config.h).

### 4. Connect to the climate panel (later)

The temperature dial on the CX-5 is a small spring-return switch with three pads in a row on the top side. With a
multimeter, confirm which pad is ground and which pads drop to 0 V when pressed. The other buttons are simple two-pad
push buttons. Connect one optocoupler per button: collector to the pad that sits at 5 V, emitter to the ground pad.

Pad voltages and layouts differ between models and model years. Measure before you connect anything.

## Building the software

PlatformIO runs on Windows, macOS and Linux.

```bash
git clone https://github.com/JTris1/mazda-cx5-controller.git
cd mazda-cx5-controller
```

### 1. Secrets (not in the repository)

Two files are deliberately untracked, because they contain keys:

1. `src/secrets.h`: copy [`src/secrets.example.h`](src/secrets.example.h) and fill in your APN, WireGuard keys and
   endpoint. Set up the WireGuard side first, see [`docs/wireguard-server.md`](docs/wireguard-server.md).
2. `src/tls_cert.h`: run `tools/gen_cert.sh` (needs OpenSSL). It prints the certificate's SHA-256 fingerprint, which
   Home Assistant pins. Generating a new one later changes the fingerprint, so keep the file.

### 2. Build and flash

```bash
pio run                  # build (also embeds web/admin.html)
pio run -t upload        # flash the board over USB
pio device monitor       # serial console at 115200 baud
```

If the upload fails, put the board into download mode: hold BOOT, tap RESET, then release BOOT, and try again.
On Linux you may need to add yourself to the `dialout` group to open the serial port.

After the first flash, later updates can be sent over the air from the admin page (slow over cellular, about
10 KB/s), but a USB flash is the safe choice for risky changes.

### 3. Enroll your first key

Over the tunnel, open `https://192.168.10.2/admin` (or your tunnel address), accept the certificate warning after
comparing the fingerprint, and choose "Create the first admin key". See [`docs/admin-page.md`](docs/admin-page.md).
Add a `control` key for Home Assistant, see [`docs/home-assistant.md`](docs/home-assistant.md).

### Using the serial console

Type `help` for the full list. The main commands:

| Command | What it does |
|---|---|
| `temp <F>` | Set an absolute temperature in degrees F (presses down to the lowest value, then up) |
| `up [n]` / `down [n]` | Step the temperature up or down n times |
| `heater <driver\|passenger> <high\|medium\|low\|off>` | Seat heater level |
| `defrost <front\|rear> <on\|off>` | Defrost |
| `fanset <level>` / `fan <up\|down> [n]` | Absolute fan speed, or step it |
| `hold <ms>`, `gap <ms>`, `range <min> <max>`, `clamp <n>`, `fancfg <speeds> <extra>` | Timing and range settings (saved) |
| `settings` | Show all saved settings |
| `start` / `rsenable [on\|off]` / `rscfg ...` / `rswait <sec>` | Remote start trigger, whether the network may use it, pulse settings |
| `engine` | Engine-running input status |
| `net` / `wg [restart]` / `api` | Cellular, tunnel and API status |
| `keylist` / `keyadd` / `keydel` / `keywipe yes` / `authlog` | Manage client keys |
| `bench <name> <sec>` | Hold one output on for metering |
| `i2c` | Show the expander's registers |

## Testing

- **On the bench:** the console commands above, with an LED per channel standing in for the panel.
- **Without the car:** a simulator of the device (`homeassistant/tests/mock_device.py`) speaks the same signed API,
  and the Home Assistant integration, the API client, the admin page and the update flow are tested against it:
  ```bash
  python3.14 -m venv .venv && .venv/bin/pip install -r homeassistant/requirements_test.txt
  cd homeassistant && ../.venv/bin/python -m pytest
  ```
  From `homeassistant/`, `python tests/serve_mock.py` serves the admin page against the simulator at
  http://127.0.0.1:8765/admin.

## Status

| Area | State |
|---|---|
| Climate buttons, remote start with engine confirmation | Working in the car |
| Cellular + WireGuard + signed HTTPS API | Working |
| Home Assistant integration | Working |
| Admin page, tracked state, OTA updates | Written and tested on the simulator; OTA needs more proof |
| Link watchdog, hang watchdog, flash log | Written and built, not yet verified in the car |
| Climate power button | Disabled (its panel line misbehaved) |
| GPS | Tried and abandoned, see `CLAUDE.md` |

## Roadmap

1. Prove the watchdog, flash log and over-the-air updates in the car
2. Find out why the device went offline for days (the flash log should show it next time)
3. Battery protection: report battery voltage and refuse commands when it is low
4. Vehicle data over CAN for temperature feedback and telemetry (read-only)
5. A custom PCB that carries the LilyGO board, so no breadboard is needed

## Project layout

```
src/                 Firmware (C++/Arduino): api, admin_api, auth, actions, climate, ota, link_watch, flash_log, ...
src/config.h         Pins, limits and the default settings
web/admin.html       The admin page (embedded into the firmware at build time)
homeassistant/       Home Assistant integration (custom_components/cx5_controller), tests and the device simulator
tools/               cx5_client.py (command-line client), gen_cert.sh, embed_web.py
docs/                api.md, home-assistant.md, admin-page.md, wireguard-server.md, idatastart.md
lib/WireGuard-ESP32  Vendored and patched WireGuard library (see its PATCHES.md)
platformio.ini       Build configuration
CLAUDE.md            Notes for the AI assistant and for restarting work on a new machine
```

## Contributing

Issues and suggestions are welcome, especially wiring notes and test results for other
vehicles. If you try this on another car, please record what you measured.

## Build Log
I will document my build process here. My goal is to take pictures of my progress, including what my prototypes look like, where I'm soldering to on the PCB, etc. The hardware comes in soon so I will begin building as soon as all the components come in.
