# Home Assistant integration

`homeassistant/custom_components/cx5_controller/` talks to the controller's signed HTTPS API. It needs
Home Assistant to be able to reach the controller's tunnel address (see `wireguard-server.md`: Home Assistant
must be a WireGuard peer or sit behind the router that is the hub). The controller never connects out to
Home Assistant, so Home Assistant polls.

## Install

1. Copy `homeassistant/custom_components/cx5_controller` to `/config/custom_components/` on the Home Assistant
   machine and restart Home Assistant.
2. Settings > Devices & services > Add integration > "CX-5 Climate Remote". Enter the tunnel address, port and key id,
   and the certificate fingerprint. Open the controller's admin page (`/admin`): the "Connect Home Assistant" box
   shows all three with Copy buttons.
3. **Leave the private key empty.** Home Assistant creates its own key (it never leaves Home Assistant) and shows
   you its public key. On the admin page, under "Add a key", enter the same key id, role **control** (not admin:
   Home Assistant stores the key unencrypted) and paste the public key, then press Submit in Home Assistant.
   If you would rather make the key yourself (`python3 tools/cx5_client.py keygen homeassistant`), paste the
   contents of `~/.cx5/homeassistant.pem` into the private key field instead.

## What you get

| Entity | Notes |
|---|---|
| Button **Remote start** | An action, never a toggle. Unavailable while a start is in progress, the engine is running, remote start is disabled on the device, or the device is offline. |
| Sensor **Remote start status** | `idle`, `starting`, `confirmed`, `failed`, `refused` (with `detail`). Shows `starting` the moment you press. |
| Climate | Target temperature 60-84 F and fan speed 1-7. Absolute targets, see below. |
| Selects | Driver / passenger seat heater: off, low, medium, high. |
| Switches | Front / rear defrost (assumed state). |
| Binary sensors | Engine running, connectivity, remote start enabled. |
| Sensors | Last command, uptime, firmware. |
| Button **Reset tracked state** (diagnostic) | See "Resync". |

## How commands behave

The car cannot report heater, defrost, temperature or fan state, so everything shown for those is the
**tracked state**: what the controller last pressed. Entities carry `assumed: true`.

* Commands are slow (a temperature change takes 10-15 s, a remote start 3-10 s). Nothing waits on a request:
  the controller queues the work and Home Assistant follows the job.
* **Pending:** the moment you change something, the entity shows your target with `pending: true`. When the job
  finishes the entity shows the controller's value. If the job failed, the entity returns to the last confirmed
  value (`last_confirmed` attribute) and you get a persistent notification and a `cx5_controller_command_failed` event.
* **Temperature and fan are debounced.** Tapping + five times sends one absolute command after 1.5 s of quiet. If
  you change the target again while a run is in progress, the newer target runs right after, not in parallel.
* **Remote start** fires `cx5_controller_remote_start` with `result: confirmed | failed | refused` and a `detail`.
  Failures and refusals also create a notification. Use a confirmation dialog on the button
  (see `homeassistant/dashboard.example.yaml`).
* The controller enforces its own limits (cooldown 60 s, 3 per hour, 10 per day); the integration cannot lift them.
* If the controller is offline, its entities become unavailable and commands are rejected immediately with a clear
  error rather than queued.

## Resync

If someone uses the physical climate buttons, the tracked state is wrong. Either call the service (nothing is pressed):

```yaml
action: cx5_controller.resync
data:
  temperature: 70
  fan: 3
  driver_heater: low
  front_defrost: true
```

or press **Reset tracked state**: heaters and defrost become off, temperature and fan become unknown (the next
change re-homes them). When the engine stops the controller also resets heaters and defrost to off (setting
`resetOnEngineStop`).

## Polling and data use

Every 60 s while idle (**estimate, not measured: roughly 150-250 MB a month**, because every poll opens a new TLS connection, and the handshake with the certificate is most of the traffic; check `wg show` transfer counters on the router), every 15 s while the engine runs, and about every 3 s only while a
command is being followed. The controller allows 30 challenges per minute per address, which this stays under.

## Tests

```bash
python3 -m venv .venv && .venv/bin/pip install -r homeassistant/requirements_test.txt
cd homeassistant && ../.venv/bin/python -m pytest
```

They run against `tests/mock_device.py`, a TLS server that verifies signatures like the firmware does.
