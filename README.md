# CX-5 climate remote: phase 1 (bench test)

Local-only firmware: six bench buttons and a USB serial console drive six PC817
optocouplers through an MCP23017 I2C expander. No modem, no MQTT yet. The
`enqueuePress()` / `enqueueTemp()` entry points are what the SMS/MQTT layer will call
in phase 2.

## Wiring (per channel)

```
MCP23017 pin ──220R──► PC817 pin1 (LED anode)
             └─10k──► GND            (keeps the opto off while the expander resets)
GND ◄─────────────── PC817 pin2 (LED cathode)
CCU 5V pad ◄───────── PC817 pin4 (collector)
CCU GND pad ◄──────── PC817 pin3 (emitter)   (temp up + down share the dial's centre pin)
```

MCP23017: VDD to 3V3, VSS to GND, RESET to 3V3, A0/A1/A2 to GND (address 0x20),
SDA to GPIO 3, SCL to GPIO 2 (the board's I2C pins; 4.7k pull-ups to 3V3 if your
breakout lacks them).

| Function         | Pin            | CCU pad                          |
|------------------|----------------|----------------------------------|
| Temp up          | MCP GPA0       | dial switch pin 3 (right)        |
| Temp down        | MCP GPA1       | dial switch pin 1 (left)         |
| Seat driver      | MCP GPA2       | seat button pad (TBD)            |
| Seat passenger   | MCP GPA3       | seat button pad (TBD)            |
| Front defrost    | MCP GPA4       | defrost button pad (TBD)         |
| Rear defrost     | MCP GPA5       | defrost button pad (TBD)         |
| Button temp up   | GPIO 15        | to GND                           |
| Button temp down | GPIO 16        | to GND                           |
| Button seat drv  | GPIO 14        | to GND                           |
| Button seat pas  | GPIO 21        | to GND                           |
| Button front def | GPIO 38        | to GND                           |
| Button rear def  | GPIO 39        | to GND                           |
| 12 V sense       | GPIO 9         | 220k/47k divider + 100 nF        |

Pins are for the T-SIM7670G-S3 **Standard** board. Never use 4, 5, 6, 7, 46 (modem),
8 (battery ADC) or 42 (power-save). Confirm 9, 14, 21 are on your headers; change them in
`src/pins.h` if not. Still free: GPIO 40, 41, 47 and MCP GPA6-7 / GPB0-7.

On day one do not connect the CCU: put LEDs or a multimeter on the opto outputs first.

## Build and flash from WSL2

```bash
python3 -m venv ~/.venvs/pio && ~/.venvs/pio/bin/pip install platformio
cd ~/esp32-mazda-controller && ~/.venvs/pio/bin/pio run
```

Build in WSL (keep a copy on the Linux filesystem, not `/mnt/c`, for speed).
The S3's native USB re-enumerates when it resets, which makes `usbipd` attachments
flaky. The reliable route is to flash from Windows:

```powershell
# one-time, Windows:  pip install esptool
# copy bootloader.bin, partitions.bin, firmware.bin from .pio/build/tsim7670g-s3/
# and boot_app0.bin from ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/
esptool.py --chip esp32s3 --port COM5 write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

(`pio run -t upload` works too once the board is attached with
`usbipd attach --wsl --auto-attach --busid <id>`. If it fails mid-way, hold BOOT, tap
RESET, release BOOT to force download mode.)

## Console

| Command | Effect |
|---|---|
| `temp 72` / `temp lo` / `temp hi` | absolute: press down enough to clamp at LO, then count up (`hi` presses up until clamped) |
| `up [n]`, `down [n]` | relative temperature steps |
| `seatd [n]`, `seatp [n]` | seat heater button presses |
| `deff [n]`, `defr [n]` | front / rear defrost button presses |
| `tempcal <minF> <maxF> <steps> [loOffset]` | calibrate the dial, saved to flash |
| `press <ms>`, `gap <ms>` | pulse width and gap, saved to flash |
| `stop` | abort the running sequence and clear the queue |
| `vbat`, `status`, `help` | |

## Things to measure on the car

1. Smallest `press` that reliably registers (start at 80 ms).
2. Smallest `gap` that never drops a click.
3. Dial range: with `temp lo`, press `up` and count to the first and last displayed
   temperature, then enter it with `tempcal`. Check whether LO (and HI) are separate
   positions beyond the numeric range (that's `loOffset`).
4. Does the first press on a sleeping CCU only wake the display? If so we need a dummy
   press before counting.
5. What one seat-heater press does (cycle order, whether it wraps to off) and whether
   seat and defrost state reset at ignition. There is no state feedback until CAN.
