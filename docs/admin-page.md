# Admin page

`https://<tunnel address>/admin`, served by the controller itself and reachable only over the WireGuard tunnel.
The page is public on the tunnel and holds no secrets: everything it does is a signed API call made with a key
that lives in your browser.

## Opening it

The controller's certificate is self-signed, so the browser warns once. Compare the fingerprint it shows with the
one in the controller's boot log (or `tools/gen_cert.sh`) before accepting. The page needs WebCrypto and
IndexedDB, which browsers only allow on https pages.

## Your browser's key

* **First run, controller has no keys:** the page offers "Create the first admin key". It is enrolled without a
  signature, which means anything on the tunnel could claim a blank controller, so do this right after flashing.
  Once any key exists this is closed (the console command `keywipe yes` reopens it).
* **Controller already has keys:** create a key in the browser, copy its public key, and have an admin enroll it
  (`python3 tools/cx5_client.py key-add <id> admin <public key>`, or another browser's admin page).
* The private key is generated non-extractable (it cannot be read out, not even by this page) and stored in the
  browser's IndexedDB. Clearing site data, or "Remove this browser's key", deletes it; enroll a new one.

## Sections

| Section | What it does |
|---|---|
| Device | Firmware, running slot, uptime, last restart reason (power-on, crash, brownout, watchdog), memory, links |
| Keys | List, add, change role, revoke. You cannot revoke or demote the key you are signed in with. 8 keys at most |
| Settings | Button timing, temperature range, fan speeds, remote start pulses, reset-on-engine-stop. Validated on the controller; refused while a command runs |
| Remote start | Enable or disable network remote start |
| Device log | What the controller printed, current run and the run before the last restart, filterable to warnings and errors |
| Sign-in log | The last 32 attempts: key, address and why it was accepted or refused |
| Firmware update | Pick `firmware.bin`, check it, upload, then restart into it (see below) |
| Restart | Restarts the controller |

## Updating the firmware

1. `pio run`, then choose `.pio/build/<env>/firmware.bin`.
2. The page checks it is an ESP32 image, shows its version and hash, and asks you to confirm. Over the cellular
   link the upload takes minutes (about 10 KB/s on the modem's UART at 115200 baud; a faster UART is a future
   improvement). The car keeps working during the upload; commands are refused only while the flash is being written.
3. After "Uploaded and verified", press **Restart into the new firmware**.
4. The new firmware runs on probation (a banner says so). It is confirmed after 3 minutes with cellular, tunnel and
   API up; otherwise the controller returns to the previous firmware by itself.

Refused while a command runs or the engine is running.

## Developing the page

`web/admin.html` is one self-contained file. `tools/embed_web.py` gzips it into `src/admin_page.gen.h` on every
build (the generated header is not committed). To try the page without a car:

```bash
cd homeassistant && python3 tests/serve_mock.py     # http://127.0.0.1:8765/admin, plain http on localhost is a secure context
```
