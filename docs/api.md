# Controller API v1

HTTPS only, on the ESP32's WireGuard tunnel address (port 443). Nothing listens on the Wi-Fi
interface. The certificate is self-signed: pin its SHA-256 fingerprint (printed by
`tools/gen_cert.sh` and by the `api` console command).

## Signing a request

1. `GET /api/v1/challenge` (unauthenticated) returns `{"nonce": "<32 hex>", "expires_in": 30}`.
   A nonce is good for **one** request and expires after 30 s.
2. Build the string (joined with `\n`, no trailing newline):
   ```
   CX5-V1
   <nonce>
   <key id>
   <METHOD>
   <path, exactly as requested, no query string>
   <hex sha256 of the raw request body; of the empty string for a GET>
   ```
3. Sign it with ECDSA P-256 / SHA-256. Send the signature as base64 of the 64 raw bytes `r || s`
   (not DER: this is what WebCrypto produces; in Python convert with `decode_dss_signature`).
4. Send these headers with the request:
   `X-CX5-Key: <key id>`, `X-CX5-Nonce: <nonce>`, `X-CX5-Signature: <signature>`.

Public keys are enrolled as base64 of the 65-byte uncompressed point (`04 || X || Y`).
`tools/cx5_client.py` is a working reference implementation (`sign()` is the whole recipe).

## Endpoints

| Method and path | Body | Role |
|---|---|---|
| `GET /api/v1/challenge` | none | none |
| `GET /api/v1/state` | none | control |
| `POST /api/v1/heater` | `{"seat":"driver"\|"passenger","level":"off"\|"low"\|"medium"\|"high"}` | control |
| `POST /api/v1/defrost` | `{"zone":"front"\|"rear","action":"on"\|"off"}` | control |
| `POST /api/v1/temperature` | `{"value": 60..84}` | control |
| `POST /api/v1/fan` | `{"level": 1..fan_steps}` (absolute) or `{"action":"up"\|"down","steps":1..}` (relative) | control |
| `POST /api/v1/assume` | any of `{"temperature":72\|null,"heater":{"driver":"off","passenger":"low"},"defrost":{"front":false,"rear":true},"fan":3\|null}` | control |
| `POST /api/v1/power` | `{"action":"toggle"}` | control |
| `POST /api/v1/remote-start` | `{}` | control |
| `GET /api/v1/jobs/<id>` | none | control |
| `GET /api/v1/admin/keys` | none | admin |
| `POST /api/v1/admin/keys` | `{"id":"...","role":"control"\|"admin","public_key":"<base64>"}` | admin |
| `DELETE /api/v1/admin/keys/<id>` | none | admin |
| `GET /api/v1/admin/info` | none | admin |
| `GET /api/v1/admin/log` | none (recent sign-in attempts, newest first) | admin |
| `GET /api/v1/admin/logs` | none (`current`, `previous`: the device log as text, `reset_reason`) | admin |
| `GET /api/v1/admin/settings` | none | admin |
| `POST /api/v1/admin/settings` | any subset of the settings fields below | admin |
| `POST /api/v1/admin/remote-start-enabled` | `{"enabled": true\|false}` | admin |
| `POST /api/v1/admin/reboot` | `{}` (replies `202`, restarts about a second later) | admin |
| `GET /api/v1/admin/firmware` | none (update status) | admin |
| `POST /api/v1/admin/firmware` | `{"size":N,"sha256":"<hex>","version":"..."}` | admin |
| `PUT /api/v1/admin/firmware/<token>` | the raw firmware image | token from the POST |
| `GET /api/v1/setup` | none | none |
| `POST /api/v1/setup/first-key` | `{"id":"...","public_key":"<base64>"}` | none, only while no key exists |
| `GET /admin` | none (the admin web page, see `admin-page.md`) | none |

An `admin` key can also use every `control` endpoint.

### Keys

`GET /api/v1/admin/keys` returns `id`, `role`, `fingerprint` and the (public) `public_key` of each key. Adding a key
with an existing id replaces it, which is how a role is changed; you cannot change or delete your own key.

### Settings

`GET /api/v1/admin/settings` returns, and `POST` accepts any subset of: `hold_ms`, `gap_ms`, `temp_min_f`,
`temp_max_f`, `clamp_extra`, `fan_steps`, `fan_home_extra`, `seat_heater_stages`, `rs_pulses`, `rs_pulse_ms`,
`rs_gap_ms`, `rs_confirm_ms` (whole numbers) and `reset_on_engine_stop` (boolean). Nothing is changed if any value
is unknown, mistyped or out of range (`400` with a `reason`), and changes are refused with `503 busy` while a
command is running. They are saved to flash. Tracked temperature or fan values that no longer fit the new limits
become `null`.

### First key (`setup`)

While the device holds **no** keys, `POST /api/v1/setup/first-key` enrolls an `admin` key without a signature
(anything that can reach the tunnel can do this, so do it right after flashing). It answers `409 setup_closed` as
soon as any key exists, and is limited to 5 attempts a minute. `keywipe yes` on the serial console reopens it.
`GET /api/v1/setup` tells the admin page which situation it is in (`{"setup_open": bool, "firmware": "..."}`).

### Firmware update

1. `POST /api/v1/admin/firmware` (signed, admin) declares `size`, `sha256` and `version` and returns a one-time
   `upload_token` valid for 10 minutes. The signature covers the declared hash.
2. `PUT /api/v1/admin/firmware/<token>` streams the image into the spare slot. The token is consumed by the first
   attempt. The upload must match the declared size and SHA-256 and pass the ESP-IDF image checks, otherwise it is
   discarded (`400 upload_failed` with a `reason`) and nothing changes.
3. `POST /api/v1/admin/reboot` runs it.

The start is refused (`409 refused` with a `reason`) while a command is running or the engine is running, and
commands are refused (`503 busy`) during the upload. The new image boots on probation: it is confirmed only after
cellular, the tunnel and the API have been healthy together for 3 minutes. If it restarts before that, or is still
not healthy 20 minutes after booting, the bootloader returns to the previous image. `pending_verify` in
`GET /api/v1/admin/firmware` shows a probation in progress.

### Tracked state and `assume`

The climate panel cannot report its own state, so the device remembers what it **last commanded** (stored in
flash, so it survives a reboot) and reports it under `climate` in `state`. Temperature and fan use absolute
commands: the device presses down to the lowest value first, then up to the target ("homing"), so they are
correct regardless of what the panel showed before. Seat heaters and defrost are counted from the tracked state,
so they can drift if someone uses the physical buttons; `POST /api/v1/assume` corrects the tracked values without
pressing anything. When the engine stops, the tracked heater and defrost state is reset to off (setting
`resetOnEngineStop`, default on).

### `GET /api/v1/state` response

```json
{
  "firmware": "0.2.0", "uptime_s": 1234, "cellular": true, "tunnel": true, "expander_ok": true,
  "engine_running": false, "engine_age_s": 310,
  "remote_start": {"enabled": true, "state": "idle|starting|confirmed|failed|refused", "detail": "...", "age_s": 42},
  "busy": {"running": null, "queued": 0},
  "climate": {"tracked": true, "temperature_f": 72, "heater": {"driver": "off", "passenger": "low"},
              "defrost": {"front": false, "rear": false}, "fan": 3},
  "limits": {"temp_min_f": 60, "temp_max_f": 84, "fan_steps": 7}
}
```

`temperature_f` and `fan` are `null` until they have been set. `busy.running` is the id of the job being executed.

`POST /api/v1/power` currently answers `503 {"error":"disabled"}`: the climate power channel is switched off
in firmware (`CLIMATE_POWER_ENABLED` in `src/config.h`).

Actions are queued and run one at a time: a successful `POST` returns `202 {"job_id": n}`, and
`GET /api/v1/jobs/<n>` reports `queued`, `running` or `done` (with `ok` and `result`).

## Status codes

| Code | Meaning |
|---|---|
| 202 | action queued |
| 400 | malformed JSON, invalid arguments, or a query string was sent |
| 401 | not authorized. The same answer for every cause: unknown key, bad or used nonce, bad signature, stale challenge |
| 403 | the key's role is not allowed here, or remote start was refused by policy (`reason` says why) |
| 404 | unknown path or job |
| 409 | key store full, deleting or changing your own key, setup already closed, or a firmware update that cannot start now |
| 413 | body larger than 512 bytes (not applied to the firmware upload) |
| 429 | too many failures or challenges from this address (blocked for 60 s) |
| 503 | the expander is missing or the action queue is full |

The reason behind a 401 is only visible on the device (`authlog` console command).

## Remote start policy

Network-requested starts are refused until remote start is enabled (`rsenable on` on the serial console, or the
admin page / `POST /api/v1/admin/remote-start-enabled`), and then
limited to one per 60 s, 3 per hour and 10 per day. The request must also use a challenge that is
at most 15 s old. The serial `start` command is not affected.

## Device log and the link watchdog

Every line the device prints is also kept in a 6 KB ring buffer in RTC memory, which survives a crash, watchdog or
brownout restart (not a full power loss). `GET /api/v1/admin/logs` returns this run (`current`) and the run before
the last restart (`previous`). The admin page shows both.

The link watchdog repairs the network without anyone watching: tunnel without a valid session for 3 minutes
restarts the tunnel (and again every 5 minutes while it stays down, because the WireGuard library gives up after about 90 s and never retries by itself, which is what a router outage looks like), 8 minutes restarts the cellular link, 20 minutes restarts the device (and 15 minutes with no
cellular link at all restarts the device). Restarts are held back while a command runs or firmware is written, and each
consecutive watchdog restart waits longer (20, 40, 80, then 120 minutes). `GET /api/v1/admin/info` has a `link`
object with the current state, and the `net` console command prints it.
