# Design notes (phases 2-3)

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

## Command security (phase 2)

SMS is unencrypted and the sender can be spoofed, so security is done at the application
layer. Every command message is:

```
v1 | counter (u32) | command | args | Ed25519 signature over all of the above
```

base64-encoded, which fits in one 160-char SMS.

* **Authenticity:** Home Assistant holds the Ed25519 private key; the car holds only the
  public key. Stealing the device does not let anyone sign commands.
* **Replay protection:** strictly increasing counter, last value kept in NVS. A replayed
  or old message is dropped before anything is touched.
* **Rate limit:** max N accepted start commands per hour, enforced on the device.
* **Sender allowlist:** only a convenience filter, never the security boundary.
* **Blast radius:** commands can only start the engine, adjust temperature and seats.
  Doors stay locked, and the module's own brake/hood shutdown still applies.
* **Confidentiality (optional):** wrap the signed message in an X25519 sealed box to the
  car's public key. Costs a second SMS segment. Not required for safety.

## Hardware decisions

* **Prototype power:** the cheap 12 V-to-5 V module (10 mA no-load) is accepted for bench
  and short in-car tests only. Fuse (~1 A) and TVS (e.g. SMBJ24A) on its input. Do not
  leave it connected to the car battery for weeks.
* **18650 cell:** removed. Not used as primary or backup power (cold charging, hot
  cabin, unknown charger/NTC on the board).
* **Future PCB:** carrier board for the LilyGO T-SIM7670G-S3 Standard on headers, not a
  bare-module design. Integrates a low-Iq automotive buck (tens of uA, >= 2 A, >= 40 V
  load-dump rating, reverse protection), TVS and fuse, MCP23017, PC817 channels with
  pull-downs, 12 V divider, ignition/running inputs and the iDatastart trigger.
