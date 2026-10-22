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
| `POST /api/v1/fan` | `{"action":"up"\|"down","steps":1..}` (steps optional) | control |
| `POST /api/v1/power` | `{"action":"toggle"}` | control |
| `POST /api/v1/remote-start` | `{}` | control |
| `GET /api/v1/jobs/<id>` | none | control |
| `GET /api/v1/admin/keys` | none | admin |
| `POST /api/v1/admin/keys` | `{"id":"...","role":"control"\|"admin","public_key":"<base64>"}` | admin |
| `DELETE /api/v1/admin/keys/<id>` | none | admin |

An `admin` key can also use every `control` endpoint.

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
| 409 | key store full, or an attempt to delete your own key |
| 413 | body larger than 512 bytes |
| 429 | too many failures or challenges from this address (blocked for 60 s) |
| 503 | the expander is missing or the action queue is full |

The reason behind a 401 is only visible on the device (`authlog` console command).

## Remote start policy

Network-requested starts are refused until `rsenable on` is run on the serial console, and then
limited to one per 60 s, 3 per hour and 10 per day. The request must also use a challenge that is
at most 15 s old. The serial `start` command is not affected.
