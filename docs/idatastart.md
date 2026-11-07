# iDatastart HCXG0 remote-start module: how the controller connects

The wiring below is what the firmware assumes (`src/config.h`: trigger on GPIO 40, engine-running input on GPIO 39).
The connectivity design that used to be in this file is obsolete (see `CLAUDE.md`).

## iDatastart HCXG0 (CMHCXG0) interface, from ADS master guide 20241125

| Need | Wire / option | Notes |
|---|---|---|
| Start trigger | M3-06 WHITE/BLUE, "External RS trigger input (-)" | Ground-pulse input, **disabled by default**. Enable with option 1-16: single, double or triple pulse. Use **double** pulse so noise can't start the engine. The guide gives no pulse width; start with 200 ms pulses 200 ms apart and tune. A PC817 collector to M3-06, emitter to chassis ground. |
| Engine running (remote start) | M2-06 BLUE/WHITE, "Ground while running (GWR)" | 250 mA (-) output. On 250 ms before ignition, stays on for the whole remote-start run, last to drop. Alternative: assign a POC output to F24 "GND when engine running". |
| Key-on / driving | any POC (M2-08..11) set to F31 "GND with vehicle ignition" | Needed if the ESP32 should stay awake while you drive. |
| Run time | option 1-03 | 3 to 35 minutes. When GWR drops, start the 2-minute sleep timer. |
| Telematic port | D3 (gray, 4-pin) | Used by the vendor's own telematics module; the guide doesn't document the protocol, so we don't use it. |

Reading a (-) output on a 3.3 V GPIO: 10 k pull-up to 3.3 V on the pin, 10 k in series
to the wire, plus a 3.3 V Zener or BAT54S clamp. Active LOW. Share ground with the module.

The module also needs its brake (M3-01), hood (M3-07) and engine-sense inputs wired for
remote start to be allowed at all.
