"""A software stand-in for the CX-5 controller, used by the tests and by the admin page tests.

It speaks the same signed HTTPS API as the firmware (docs/api.md) and follows the same rules: one-time
nonces that expire after 30 s, ECDSA P-256 verification, roles, per-address failure limits, jobs run one at
a time, tracked (not measured) climate state with the same homing and cycle arithmetic, and the same
remote start policy and states. Time is injectable and every press takes `press_seconds`, so tests are fast.
"""
from __future__ import annotations

import asyncio
import base64
import datetime
import hashlib
import json
import os
import secrets
import ssl
import tempfile
import time
from typing import Any, Callable

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
from cryptography.x509.oid import NameOID

NONCE_TTL = 30.0
START_MAX_NONCE_AGE = 15.0
HEATER_LEVELS = {"off": 0, "high": 1, "medium": 2, "low": 3}
HEATER_NAMES = {v: k for k, v in HEATER_LEVELS.items()}
STATUS_TEXT = {200: "OK", 201: "Created", 202: "Accepted", 400: "Bad Request", 401: "Unauthorized",
               403: "Forbidden", 404: "Not Found", 409: "Conflict", 413: "Payload Too Large",
               429: "Too Many Requests", 503: "Service Unavailable"}


def make_certificate() -> tuple[bytes, bytes, str]:
    """Self-signed P-256 certificate for 127.0.0.1. Returns (cert PEM, key PEM, sha256 fingerprint hex)."""
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "cx5-controller")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name).issuer_name(name).public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.SubjectAlternativeName([x509.IPAddress(__import__("ipaddress").ip_address("127.0.0.1"))]), False)
        .sign(key, hashes.SHA256())
    )
    cert_pem = cert.public_bytes(serialization.Encoding.PEM)
    key_pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    return cert_pem, key_pem, hashlib.sha256(cert.public_bytes(serialization.Encoding.DER)).hexdigest()


class MockDevice:
    def __init__(self, *, tls: bool = True, press_seconds: float = 0.002, start_seconds: float = 0.05, idle_timeout: float = 5.0,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self.tls = tls                      # False only for browser tests: http://localhost is a secure context
        self.press_seconds = press_seconds
        self.start_seconds = start_seconds
        self.idle_timeout = idle_timeout
        self.now = clock
        # configuration (settings)
        self.settings: dict[str, Any] = {
            "hold_ms": 150, "gap_ms": 150, "temp_min_f": 60, "temp_max_f": 84, "clamp_extra": 2, "rs_pulse_ms": 300,
            "rs_gap_ms": 300, "rs_pulses": 2, "rs_confirm_ms": 20000, "fan_steps": 7, "fan_home_extra": 1,
            "seat_heater_stages": 3, "reset_on_engine_stop": True}
        self.rebooted = 0
        self.firmware_version = "0.2.0"
        self.slot = "app0"
        self.pending_verify = False
        self.ota: dict[str, Any] = {"state": "idle", "token": None, "size": 0, "sha": "", "version": "", "received": 0, "armed": 0.0}
        self.ota_events: list[str] = []
        self.log_current = "10-31 08:00:01Z I Booting...\n10-31 08:00:09Z I Cellular: connected, IP 10.0.0.2\n"
        self.log_saved = ("10-27 03:00:00Z HB up=86400s heap=180000 min=150000 cell=up tunnel=up bad=0s wd_step=0 wd_restarts=0\n"
                           "10-27 03:15:00Z HB up=87300s heap=180000 min=150000 cell=up tunnel=DOWN bad=420s wd_step=2 wd_restarts=0\n"
                           "11-01 12:00:00Z BOOT reset=power-on (supply was cut) fw=0.2.0\n")
        self.log_previous = ("10-30 22:14:03Z W Link watchdog: tunnel down 180 s, restarting the tunnel\n"
                             "10-30 22:19:03Z W Link watchdog: tunnel down 480 s, restarting the cellular link\n"
                             "10-30 22:19:05Z E Cellular: modem did not attach to the network\n")
        self.page_path = os.path.join(os.path.dirname(__file__), "..", "..", "web", "admin.html")
        self.remote_start_enabled = True
        self.start_cooldown = 60.0
        self.challenge_limit = 1000   # firmware: 30 per minute per address; tests poll jobs far faster
        # tracked climate state (what the real device remembers; never read from the car)
        self.temperature: int | None = None
        self.heater = {"driver": 0, "passenger": 0}
        self.defrost = {"front": False, "rear": False}
        self.fan: int | None = None
        # engine and remote start
        self.engine_running = False
        self.engine_changed = self.now()
        self.engine_will_start = True
        self.remote_state, self.remote_detail, self.remote_since = "idle", "", self.now()
        self.start_times: list[float] = []
        # keys, nonces, failures
        self.keys: dict[str, dict[str, Any]] = {}
        self.nonces: dict[str, float] = {}
        self.failures: dict[str, list[float]] = {}
        self.blocked_until: dict[str, float] = {}
        self.challenges: dict[str, list[float]] = {}
        self.setup_attempts: dict[str, list[float]] = {}
        self.auth_log: list[tuple[float, str, str]] = []
        # jobs
        self.jobs: dict[int, dict[str, Any]] = {}
        self._next_job = 1
        self._queue: asyncio.Queue[int] = asyncio.Queue()
        self.running_job: int | None = None
        # instrumentation and fault injection
        self.calls: list[dict[str, Any]] = []        # every authenticated request that was accepted
        self.rejected: list[str] = []                 # reasons for every rejected request
        self.connections = 0
        self.log_press: list[str] = []                # which buttons were "pressed", in order
        self.fail_next: list[tuple[int, dict[str, Any]]] = []   # canned replies for the next signed requests
        self.close_after_challenge = False
        self.response_delay = 0.0
        self.uptime_start = self.now()
        # server
        self._server: asyncio.AbstractServer | None = None
        self._worker: asyncio.Task | None = None
        cert_pem, key_pem, self.fingerprint = make_certificate()
        self._tmp = tempfile.TemporaryDirectory()
        self._cert_file = os.path.join(self._tmp.name, "cert.pem")
        self._key_file = os.path.join(self._tmp.name, "key.pem")
        open(self._cert_file, "wb").write(cert_pem)
        open(self._key_file, "wb").write(key_pem)
        self.host, self.port = "127.0.0.1", 0

    # ---------------------------------------------------------------- lifecycle
    async def start(self) -> "MockDevice":
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self._cert_file, self._key_file)
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        self._server = await asyncio.start_server(self._handle, self.host, self.port, ssl=ctx if self.tls else None)
        self.port = self._server.sockets[0].getsockname()[1]
        self._worker = asyncio.create_task(self._work())
        return self

    async def stop(self) -> None:
        if self._worker:
            self._worker.cancel()
            await asyncio.gather(self._worker, return_exceptions=True)
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        self._tmp.cleanup()

    async def __aenter__(self) -> "MockDevice":
        return await self.start()

    async def __aexit__(self, *exc: object) -> None:
        await self.stop()

    # ---------------------------------------------------------------- helpers for tests
    def add_key(self, key_id: str, role: str, public_key_b64: str) -> None:
        self.keys[key_id] = {"role": role, "pub": base64.b64decode(public_key_b64)}

    def set_engine(self, running: bool) -> None:
        if running == self.engine_running:
            return
        self.engine_running = running
        self.engine_changed = self.now()
        if not running and self.settings["reset_on_engine_stop"]:
            self.heater = {"driver": 0, "passenger": 0}
            self.defrost = {"front": False, "rear": False}

    # ---------------------------------------------------------------- HTTP plumbing
    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.connections += 1
        peer = (writer.get_extra_info("peername") or ("0.0.0.0", 0))[0]
        try:
            while True:
                try:
                    line = await asyncio.wait_for(reader.readline(), self.idle_timeout)
                except asyncio.TimeoutError:
                    break
                if not line:
                    break
                method, target, _ = line.decode().strip().split(" ", 2)
                headers: dict[str, str] = {}
                while True:
                    h = await reader.readline()
                    if h in (b"\r\n", b"\n", b""):
                        break
                    k, _, v = h.decode().partition(":")
                    headers[k.strip().lower()] = v.strip()
                body = await reader.readexactly(int(headers.get("content-length", "0") or 0))
                if self.response_delay:
                    await asyncio.sleep(self.response_delay)
                status, data = self._route(method, target, headers, body, peer)
                extra = ""
                ctype = "application/json"
                if isinstance(data, tuple):                      # (bytes, content type, extra header lines)
                    payload, ctype, extra = data
                else:
                    payload = json.dumps(data).encode()
                close = headers.get("connection", "").lower() == "close"
                resp = (f"HTTP/1.1 {status} {STATUS_TEXT.get(status, 'Error')}\r\nContent-Type: {ctype}\r\n{extra}"
                        f"Content-Length: {len(payload)}\r\n" + ("Connection: close\r\n" if close else "") + "\r\n").encode() + payload
                writer.write(resp)
                await writer.drain()
                if close or (self.close_after_challenge and target == "/api/v1/challenge"):
                    break
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()

    # ---------------------------------------------------------------- authentication (mirrors auth.cpp)
    def _fail(self, peer: str) -> None:
        now = self.now()
        window = [t for t in self.failures.get(peer, []) if now - t < 60] + [now]
        self.failures[peer] = window
        if len(window) >= 5:
            self.blocked_until[peer] = now + 60
            self.failures[peer] = []

    def _authenticate(self, method: str, path: str, headers: dict[str, str], body: bytes, peer: str,
                      needed: str) -> tuple[int | None, dict[str, Any] | None, str, float]:
        """Returns (error status or None, error body, key id, nonce age)."""
        now = self.now()
        if self.blocked_until.get(peer, 0) > now:
            self.rejected.append("rate limited")
            return 429, {"error": "rate_limited"}, "", 0.0
        key_id = headers.get("x-cx5-key", "")
        nonce = headers.get("x-cx5-nonce", "")
        sig_b64 = headers.get("x-cx5-signature", "")
        issued = self.nonces.pop(nonce, None)       # consumed whatever happens next
        key = self.keys.get(key_id)
        reason = None
        age = 0.0
        if not key:
            reason = "unknown key"
        elif issued is None or now - issued > NONCE_TTL:
            reason = "bad or expired nonce"
        else:
            age = now - issued
            try:
                raw = base64.b64decode(sig_b64)
                assert len(raw) == 64
                data = "\n".join(["CX5-V1", nonce, key_id, method, path, hashlib.sha256(body).hexdigest()]).encode()
                pub = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), key["pub"])
                pub.verify(encode_dss_signature(int.from_bytes(raw[:32], "big"), int.from_bytes(raw[32:], "big")),
                           data, ec.ECDSA(hashes.SHA256()))
            except Exception:  # noqa: BLE001 - any failure is "bad signature"
                reason = "bad signature"
        if reason is None and needed == "admin" and key["role"] != "admin":
            self.rejected.append("role not allowed")
            self._fail(peer)
            return 403, {"error": "forbidden"}, key_id, age
        if reason:
            self.rejected.append(reason)
            self.auth_log.append((now, key_id, reason))
            self._fail(peer)
            return 401, {"error": "unauthorized"}, key_id, age
        self.failures.pop(peer, None)
        return None, None, key_id, age

    # ---------------------------------------------------------------- routing (mirrors api.cpp)
    def _route(self, method: str, path: str, headers: dict[str, str], body: bytes, peer: str) -> tuple[int, dict]:
        if path == "/api/v1/challenge" and method == "GET":
            now = self.now()
            recent = [t for t in self.challenges.get(peer, []) if now - t < 60]
            if self.blocked_until.get(peer, 0) > now or len(recent) >= self.challenge_limit:
                return 429, {"error": "rate_limited"}
            self.challenges[peer] = recent + [now]
            nonce = secrets.token_hex(16)
            self.nonces[nonce] = now
            return 200, {"nonce": nonce, "expires_in": 30}

        if path == "/admin" and method == "GET":
            with open(self.page_path, "rb") as f:
                return 200, (f.read(), "text/html; charset=utf-8",
                             "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                             "connect-src 'self'; img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n")
        if path == "/api/v1/setup" and method == "GET":
            return 200, {"setup_open": not self.keys, "firmware": self.firmware_version}
        if path == "/api/v1/setup/first-key" and method == "POST":
            return self._first_key(body, peer)
        if path.startswith("/api/v1/admin/firmware/") and method == "PUT":
            return self._ota_put(path.rsplit("/", 1)[1], body)

        if not path.startswith("/api/v1/"):          # the firmware answers other paths with 404 before any auth
            return 404, {"error": "not_found"}
        admin = path.startswith("/api/v1/admin/")
        status, err, key_id, age = self._authenticate(method, path, headers, body, peer, "admin" if admin else "control")
        if status:
            return status, err or {}
        self.calls.append({"method": method, "path": path, "body": body, "key": key_id})
        if self.fail_next:
            return self.fail_next.pop(0)
        try:
            doc = json.loads(body) if body else {}
        except ValueError:
            return 400, {"error": "invalid_json"}

        if path == "/api/v1/state" and method == "GET":
            return 200, self._state()
        if path.startswith("/api/v1/jobs/") and method == "GET":
            job = self.jobs.get(int(path.rsplit("/", 1)[1]) if path.rsplit("/", 1)[1].isdigit() else -1)
            if not job:
                return 404, {"error": "not_found"}
            out = {"id": job["id"], "status": job["status"]}
            if job["status"] == "done":
                out.update(ok=job["ok"], result=job["result"])
            return 200, out
        if admin:
            return self._admin(method, path, doc, key_id)
        if method == "POST":
            return self._post(path, doc, age)
        return 404, {"error": "not_found"}

    def _state(self) -> dict[str, Any]:
        now = self.now()
        return {
            "firmware": self.firmware_version, "uptime_s": int(now - self.uptime_start), "cellular": True, "tunnel": True,
            "expander_ok": True, "engine_running": self.engine_running, "engine_age_s": int(now - self.engine_changed),
            "remote_start": {"enabled": self.remote_start_enabled, "state": self.remote_state, "detail": self.remote_detail,
                             "age_s": 0 if self.remote_state == "idle" else int(now - self.remote_since)},
            "busy": {"running": self.running_job, "queued": self._queue.qsize()},
            "climate": {"tracked": True, "temperature_f": self.temperature,
                        "heater": {k: HEATER_NAMES[v] for k, v in self.heater.items()},
                        "defrost": dict(self.defrost), "fan": self.fan},
            "limits": {"temp_min_f": self.settings["temp_min_f"], "temp_max_f": self.settings["temp_max_f"], "fan_steps": self.settings["fan_steps"]},
        }

    def _submit(self, kind: str, **args: Any) -> tuple[int, dict]:
        job = {"id": self._next_job, "status": "queued", "ok": False, "result": "", "kind": kind, "args": args}
        self.jobs[job["id"]] = job
        self._next_job += 1
        self._queue.put_nowait(job["id"])
        return 202, {"job_id": job["id"], "status": "queued"}

    def _post(self, path: str, doc: dict[str, Any], nonce_age: float) -> tuple[int, dict]:
        bad = (400, {"error": "invalid_arguments"})
        if path == "/api/v1/heater":
            if doc.get("seat") not in self.heater or doc.get("level") not in HEATER_LEVELS:
                return bad
            return self._submit("heater", seat=doc["seat"], level=HEATER_LEVELS[doc["level"]])
        if path == "/api/v1/defrost":
            if doc.get("zone") not in self.defrost or doc.get("action") not in ("on", "off"):
                return bad
            return self._submit("defrost", zone=doc["zone"], on=doc["action"] == "on")
        if path == "/api/v1/temperature":
            v = doc.get("value")
            if not isinstance(v, int) or isinstance(v, bool) or not self.settings["temp_min_f"] <= v <= self.settings["temp_max_f"]:
                return bad
            return self._submit("temperature", value=v)
        if path == "/api/v1/fan":
            if isinstance(doc.get("level"), int):
                if not 1 <= doc["level"] <= self.settings["fan_steps"]:
                    return bad
                return self._submit("fan_level", level=doc["level"])
            if doc.get("action") not in ("up", "down") or not 1 <= doc.get("steps", 1) <= self.settings["fan_steps"]:
                return bad
            return self._submit("fan_step", up=doc["action"] == "up", steps=doc.get("steps", 1))
        if path == "/api/v1/power":
            return 503, {"error": "disabled"}
        if path == "/api/v1/assume":
            return self._assume(doc)
        if path == "/api/v1/remote-start":
            return self._remote_start(nonce_age)
        return 404, {"error": "not_found"}

    def _assume(self, doc: dict[str, Any]) -> tuple[int, dict]:
        bad = (400, {"error": "invalid_arguments"})
        t = self.temperature, dict(self.heater), dict(self.defrost), self.fan
        touched = False
        if "temperature" in doc:
            v = doc["temperature"]
            if v is not None and (not isinstance(v, int) or not self.settings["temp_min_f"] <= v <= self.settings["temp_max_f"]):
                return bad
            self.temperature, touched = v, True
        if "fan" in doc:
            v = doc["fan"]
            if v is not None and (not isinstance(v, int) or not 1 <= v <= self.settings["fan_steps"]):
                return bad
            self.fan, touched = v, True
        for seat, level in (doc.get("heater") or {}).items():
            if seat in self.heater and level in HEATER_LEVELS:
                self.heater[seat], touched = HEATER_LEVELS[level], True
            else:
                self.temperature, self.heater, self.defrost, self.fan = t
                return bad
        for zone, on in (doc.get("defrost") or {}).items():
            if zone in self.defrost and isinstance(on, bool):
                self.defrost[zone], touched = on, True
        if not touched:
            return 400, {"error": "nothing_to_assume"}
        return 200, {"assumed": True}

    def _remote_start(self, nonce_age: float) -> tuple[int, dict]:
        if nonce_age > START_MAX_NONCE_AGE:
            return 401, {"error": "unauthorized"}
        now = self.now()
        reason = None
        self.start_times = [t for t in self.start_times if now - t < 86400]
        if not self.remote_start_enabled:
            reason = "remote start is disabled"
        elif self.start_times and now - self.start_times[-1] < self.start_cooldown:
            reason = "cooldown: wait 60 s between starts"
        elif len([t for t in self.start_times if now - t < 3600]) >= 3:
            reason = "hourly limit reached"
        elif len(self.start_times) >= 10:
            reason = "daily limit reached"
        if reason:
            self.remote_state, self.remote_detail, self.remote_since = "refused", reason, now
            return 403, {"error": "refused", "reason": reason}
        self.start_times.append(now)
        return self._submit("remote_start")

    RANGES = {"hold_ms": (10, 500), "gap_ms": (0, 5000), "temp_min_f": (40, 100), "temp_max_f": (40, 100),
              "clamp_extra": (0, 10), "rs_pulse_ms": (20, 2000), "rs_gap_ms": (0, 5000), "rs_pulses": (1, 3),
              "rs_confirm_ms": (1000, 120000), "fan_steps": (2, 12), "fan_home_extra": (0, 5), "seat_heater_stages": (1, 6)}

    def _settings(self, doc: dict[str, Any]) -> tuple[int, dict]:
        if self.running_job is not None or not self._queue.empty():
            return 503, {"error": "busy"}
        new = dict(self.settings)
        for k, v in doc.items():
            if k == "reset_on_engine_stop":
                if not isinstance(v, bool):
                    return 400, {"error": "invalid_arguments", "reason": "reset_on_engine_stop must be true or false"}
            elif k in self.RANGES:
                lo, hi = self.RANGES[k]
                if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
                    return 400, {"error": "invalid_arguments", "reason": "setting out of range"}
            else:
                return 400, {"error": "invalid_arguments", "reason": "unknown setting"}
            new[k] = v
        if new["temp_min_f"] >= new["temp_max_f"]:
            return 400, {"error": "invalid_arguments", "reason": "temp_min_f must be below temp_max_f"}
        self.settings = new
        if self.temperature is not None and not new["temp_min_f"] <= self.temperature <= new["temp_max_f"]:
            self.temperature = None
        if self.fan is not None and self.fan > new["fan_steps"]:
            self.fan = None
        return 200, dict(self.settings)

    def _ota_status(self) -> dict[str, Any]:
        o = self.ota
        return {"state": o["state"], "received": o["received"], "size": o["size"], "version": o["version"],
                "pending_verify": self.pending_verify, "running_slot": self.slot, "slot_size": 6553600}

    def confirm_ota(self) -> None:
        self.pending_verify = False

    def _ota_start(self, doc: dict[str, Any]) -> tuple[int, dict]:
        refused = lambda why: (409, {"error": "refused", "reason": why})  # noqa: E731
        if self.ota["state"] == "receiving":
            return refused("an upload is already in progress")
        if self.running_job is not None or not self._queue.empty():
            return refused("the car is busy with a command")
        if self.engine_running:
            return refused("the engine is running")
        size, sha, version = doc.get("size"), doc.get("sha256", ""), doc.get("version", "")
        if not isinstance(size, int) or isinstance(size, bool) or not version or len(version) > 31:
            return 400, {"error": "invalid_arguments"}
        if not 100000 <= size <= 6553600:
            return refused("size is not plausible for this device")
        if len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha.lower()):
            return refused("sha256 must be 64 hex digits")
        token = secrets.token_hex(16)
        self.ota = {**self.ota, "state": "armed", "token": token, "size": size, "sha": sha.lower(), "version": version,
                    "received": 0, "armed": self.now()}
        self.ota_events.append("armed")
        return 200, {"upload_token": token, "expires_in": 600}

    def _ota_put(self, token: str, body: bytes) -> tuple[int, dict]:
        o = self.ota
        fresh = self.now() - o["armed"] < 600
        ok = o["state"] == "armed" and o["token"] is not None and secrets.compare_digest(token, o["token"]) and fresh
        o["token"] = None                                     # single use
        if not ok:
            o["state"] = "idle"
            return 403, {"error": "forbidden"}
        o["state"] = "receiving"
        def fail(reason: str) -> tuple[int, dict]:
            o["state"], o["received"] = "idle", 0
            self.ota_events.append("failed: " + reason)
            return 400, {"error": "upload_failed", "reason": reason}
        if len(body) != o["size"]:
            return fail("size does not match what was declared")
        if hashlib.sha256(body).hexdigest() != o["sha"]:
            return fail("sha256 mismatch")
        if body[:1] != b"\xe9":
            return fail("not a valid firmware image")
        o["state"], o["received"] = "ready_to_reboot", len(body)
        self.ota_events.append("ready")
        return 200, {"ok": True, "reboot_required": True}

    def _first_key(self, body: bytes, peer: str) -> tuple[int, dict]:
        now = self.now()
        recent = [t for t in self.setup_attempts.get(peer, []) if now - t < 60]
        self.setup_attempts[peer] = recent + [now]
        if len(recent) >= 5:
            return 429, {"error": "rate_limited"}
        if self.keys:
            return 409, {"error": "setup_closed"}
        try:
            doc = json.loads(body)
            raw = base64.b64decode(doc["public_key"])
            ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), raw)
        except Exception:  # noqa: BLE001
            return 400, {"error": "invalid_public_key"}
        if not doc.get("id") or len(doc["id"]) > 15:
            return 400, {"error": "invalid_arguments"}
        self.keys[doc["id"]] = {"role": "admin", "pub": raw}
        return 201, {"id": doc["id"], "role": "admin"}

    def _admin(self, method: str, path: str, doc: dict[str, Any], key_id: str) -> tuple[int, dict]:
        if path == "/api/v1/admin/info" and method == "GET":
            return 200, {"firmware": self.firmware_version, "cert_fingerprint": ":".join(self.fingerprint[i:i + 2].upper() for i in range(0, 64, 2)), "build": "mock", "uptime_s": int(self.now() - self.uptime_start),
                         "reset_reason": "power-on", "free_heap": 123456, "min_free_heap": 100000, "cellular": True,
                         "tunnel": True, "keys": len(self.keys), "you": key_id,
                         "link": {"tunnel_up": True, "bad_for_s": 0, "watchdog_step": 0, "watchdog_restarts": 0},
                         "remote_start_enabled": self.remote_start_enabled, "running_slot": self.slot,
                         "next_slot": "app1" if self.slot == "app0" else "app0", "slot_size": 6553600}
        if path == "/api/v1/admin/logs" and method == "GET":
            return 200, {"current": self.log_current, "previous": self.log_previous, "saved": self.log_saved, "reset_reason": "link watchdog restart"}
        if path == "/api/v1/admin/log" and method == "GET":
            return 200, {"entries": [{"age_s": int(self.now() - t), "ip": "10.8.0.3", "key": k, "result": r}
                                     for t, k, r in reversed(self.auth_log[-32:])]}
        if path == "/api/v1/admin/settings" and method == "GET":
            return 200, dict(self.settings)
        if path == "/api/v1/admin/settings" and method == "POST":
            return self._settings(doc)
        if path == "/api/v1/admin/remote-start-enabled" and method == "POST":
            if not isinstance(doc.get("enabled"), bool):
                return 400, {"error": "invalid_arguments"}
            self.remote_start_enabled = doc["enabled"]
            return 200, {"enabled": self.remote_start_enabled}
        if path == "/api/v1/admin/reboot" and method == "POST":
            if self.ota["state"] == "receiving":
                return 409, {"error": "update_in_progress"}
            self.rebooted += 1
            if self.ota["state"] == "ready_to_reboot":
                self.firmware_version, self.slot = self.ota["version"], ("app1" if self.slot == "app0" else "app0")
                self.pending_verify = True
                self.ota = {**self.ota, "state": "idle", "token": None, "version": ""}
                self.uptime_start = self.now()
            return 202, {"rebooting": True}
        if path == "/api/v1/admin/firmware" and method == "GET":
            return 200, self._ota_status()
        if path == "/api/v1/admin/firmware" and method == "POST":
            return self._ota_start(doc)
        if path == "/api/v1/admin/keys" and method == "GET":
            return 200, {"keys": [{"id": k, "role": v["role"],
                                   "fingerprint": hashlib.sha256(v["pub"]).hexdigest()[:16],
                                   "public_key": base64.b64encode(v["pub"]).decode()} for k, v in self.keys.items()]}
        if path == "/api/v1/admin/keys" and method == "POST":
            try:
                raw = base64.b64decode(doc["public_key"])
                ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), raw)
            except Exception:  # noqa: BLE001
                return 400, {"error": "invalid_public_key"}
            if doc.get("role") not in ("admin", "control") or not doc.get("id"):
                return 400, {"error": "invalid_arguments"}
            if doc["id"] == key_id and doc["role"] != self.keys[key_id]["role"]:
                return 409, {"error": "cannot_change_own_role"}
            if doc["id"] not in self.keys and len(self.keys) >= 8:
                return 409, {"error": "key_store_full"}
            self.keys[doc["id"]] = {"role": doc["role"], "pub": raw}
            return 201, {"id": doc["id"], "role": doc["role"]}
        if path.startswith("/api/v1/admin/keys/") and method == "DELETE":
            target = path.rsplit("/", 1)[1]
            if target == key_id:
                return 409, {"error": "cannot_delete_own_key"}
            if target not in self.keys:
                return 404, {"error": "not_found"}
            del self.keys[target]
            return 200, {"removed": target}
        return 404, {"error": "not_found"}

    # ---------------------------------------------------------------- the worker (mirrors actions.cpp and climate.cpp)
    async def _press(self, name: str, times: int = 1) -> None:
        for _ in range(times):
            self.log_press.append(name)
            await asyncio.sleep(self.press_seconds)

    async def _work(self) -> None:
        while True:
            job_id = await self._queue.get()
            job = self.jobs[job_id]
            self.running_job = job_id
            job["status"] = "running"
            a = job["args"]
            ok, result = True, "done"
            kind = job["kind"]
            if kind == "temperature":
                downs = (self.settings["temp_max_f"] - self.settings["temp_min_f"]) + self.settings["clamp_extra"]
                await self._press("temp_down", downs)
                await self._press("temp_up", a["value"] - self.settings["temp_min_f"])
                self.temperature = a["value"]
                result = f"temperature set to {a['value']} F"
            elif kind == "fan_level":
                await self._press("fan_down", (self.settings["fan_steps"] - 1) + self.settings["fan_home_extra"])
                await self._press("fan_up", a["level"] - 1)
                self.fan = a["level"]
                result = f"fan set to {a['level']}"
            elif kind == "fan_step":
                await self._press("fan_up" if a["up"] else "fan_down", a["steps"])
                if self.fan is not None:
                    self.fan = max(1, min(self.settings["fan_steps"], self.fan + (a["steps"] if a["up"] else -a["steps"])))
                result = f"fan {'up' if a['up'] else 'down'} {a['steps']}"
            elif kind == "heater":
                cycle = self.settings["seat_heater_stages"] + 1
                presses = (a["level"] - self.heater[a["seat"]] + cycle) % cycle
                await self._press(f"heater_{a['seat']}", presses)
                self.heater[a["seat"]] = a["level"]
                result = f"{a['seat']} heater {HEATER_NAMES[a['level']]}"
            elif kind == "defrost":
                if self.defrost[a["zone"]] != a["on"]:
                    await self._press(f"defrost_{a['zone']}")
                    self.defrost[a["zone"]] = a["on"]
                result = f"{a['zone']} defrost {'on' if a['on'] else 'off'}"
            elif kind == "remote_start":
                if self.engine_running:
                    self.remote_state, self.remote_detail = "confirmed", "engine was already running"
                    result = "confirmed"
                else:
                    self.remote_state, self.remote_detail, self.remote_since = "starting", "trigger being sent", self.now()
                    await asyncio.sleep(self.start_seconds)
                    if self.engine_will_start:
                        self.set_engine(True)
                        self.remote_state, self.remote_detail = "confirmed", "confirmed after 3010 ms"
                        result = "confirmed"
                    else:
                        self.remote_state, self.remote_detail = "failed", "engine did not report running"
                        ok, result = False, "not confirmed"
                    self.remote_since = self.now()
            job["ok"], job["result"], job["status"] = ok, result, "done"
            self.running_job = None
