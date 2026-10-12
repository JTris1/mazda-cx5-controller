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

## Connectivity and power architecture (decided: option A)

The device stays registered on the network and holds one MQTT connection through the
modem; the ESP32-S3 sleeps (light sleep) and wakes on modem activity. No SMS in the
normal path, because per-SMS pricing on IoT SIMs was too high for 2-3 starts a day.

| Component | State | Approx. draw at the 5 V rail (estimates, measure on hardware) |
|---|---|---|
| ESP32-S3 | awake, no Wi-Fi | ~40 mA |
| ESP32-S3 | light sleep | ~0.3 mA chip, ~1 mA with board overhead |
| ESP32-S3 | deep sleep | ~0.01 mA chip, ~1 mA with board overhead |
| SIM7670G | registered, idle, MQTT keepalive | ~4-8 mA |

Total about 6-10 mA at 5 V, about 5 mA from the car battery with a good buck (about
1.7 Ah over two weeks, around 4 % of the battery). Deep sleep buys almost nothing here.

Alternatives considered and rejected as primary:

* SMS wake with deep sleep: SMS cost; kept only as an emergency wake.
* Timer polling: a 15-minute poll is about 20 MB/month and too slow for "start now".
* Scheduled RTC wake before the usual departure: good later optimization.
* Home Wi-Fi when parked, cellular when away: possible later hybrid.
* Voice-call ring wake: IoT SIMs usually have no voice service.

Risks and mitigations:

* Carrier NAT drops idle TCP: keepalive 60-120 s, last-will so HA sees "offline", QoS 1
  and retained commands so nothing is lost across reconnects.
* Reconnect loops: exponential backoff and a **daily byte budget** in NVS. A TLS connect
  every 10 s would be about 50 MB/day.
* Battery margin: 12 V sense; below ~12.0 V at rest, timer-only wake; report voltage in
  every status message.
* Broker must be reachable from the car (VPS or hosted broker); both car and HA connect
  outbound.
* Unverified until tested: that an incoming MQTT message makes the SIM7670G raise RI or
  UART activity while the ESP32 is in light sleep. Test this first.

Data: about 0.2 KB per 100-byte publish, about 150 B/min keepalive, about 5-8 KB per TLS
reconnect. Baseline is a few MB/month (cents at $0.0078/MB). Streaming CAN at 1 Hz
around the clock would be about $4.40/month, so stream only while driving and batch.

## Command security (phase 2)

Commands travel in MQTT payloads over TLS, but the transport is not trusted: security
is done at the application layer. Every command message is:

```
v1 | counter (u32) | command | args | Ed25519 signature over all of the above
```

Compact binary or base64 in the payload; no SMS size limit applies now.

* **Authenticity:** Home Assistant holds the Ed25519 private key; the car holds only the
  public key. Stealing the device does not let anyone sign commands.
* **Replay protection:** strictly increasing counter, last value kept in NVS. A replayed
  or old message is dropped before anything is touched.
* **Rate limit:** max N accepted start commands per hour, enforced on the device.
* **Broker access:** TLS, per-client credentials, and an ACL limiting the car to its own
  command and status topics. A compromised broker still cannot forge signed commands.
* **Blast radius:** commands can only start the engine, adjust temperature and seats.
  Doors stay locked, and the module's own brake/hood shutdown still applies.
* **Confidentiality (optional):** wrap the signed message in an X25519 sealed box to the
  car's public key. Not required for safety; the user chose signed-only.

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
