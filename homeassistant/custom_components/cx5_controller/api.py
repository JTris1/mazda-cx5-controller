"""Client for the CX-5 controller's signed HTTPS API (see docs/api.md).

No Home Assistant imports: this module is plain asyncio + cryptography so it can be tested alone.

How a call works
    1. Open a TLS connection (TLS 1.2 only) and check the server certificate's SHA-256 fingerprint
       against the pinned value *before sending anything*.
    2. GET /api/v1/challenge for a one-time nonce.
    3. Send the real request with X-CX5-Key / X-CX5-Nonce / X-CX5-Signature headers, where the signature is
       ECDSA P-256 / SHA-256 over "CX5-V1\\n<nonce>\\n<key id>\\n<METHOD>\\n<path>\\n<sha256(body)>".
    Steps 2 and 3 share one connection, and the connection is kept for a few seconds so a burst of calls
    (for example polling a job) pays for the slow TLS handshake once.
"""
from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import ssl
import time
from dataclasses import dataclass
from typing import Any

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

CONNECTION_REUSE_SECONDS = 3.5   # the device closes idle connections after about 5 s
DEFAULT_TIMEOUT = 20.0


class Cx5Error(Exception):
    """Base class for every error raised by this client."""


class Cx5ConnectionError(Cx5Error):
    """The device could not be reached (network, TLS or timeout)."""


class Cx5PinError(Cx5ConnectionError):
    """The device presented a different certificate than the pinned one. Nothing was sent."""


class Cx5AuthError(Cx5Error):
    """The device rejected the signature or the key (HTTP 401). The reason is only visible on the device."""


class Cx5ForbiddenError(Cx5Error):
    """The key's role is not allowed to do this (HTTP 403)."""


class Cx5RefusedError(Cx5Error):
    """The device understood but refused, with a human-readable reason (for example remote start policy)."""

    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


class Cx5RateLimitedError(Cx5Error):
    """Too many failures or challenges from this address (HTTP 429)."""


class Cx5DeviceError(Cx5Error):
    """The device reported a problem (HTTP 400/404/409/503) with a short error code."""

    def __init__(self, status: int, code: str):
        super().__init__(f"{code} (HTTP {status})")
        self.status = status
        self.code = code


def normalize_fingerprint(value: str) -> str:
    """Lower-case hex without separators. Raises ValueError unless it is a SHA-256 fingerprint (64 hex digits)."""
    cleaned = value.replace(":", "").replace(" ", "").strip().lower()
    if len(cleaned) != 64 or any(c not in "0123456789abcdef" for c in cleaned):
        raise ValueError("not a SHA-256 fingerprint")
    return cleaned


def load_private_key(pem: str | bytes) -> ec.EllipticCurvePrivateKey:
    data = pem.encode() if isinstance(pem, str) else pem
    key = serialization.load_pem_private_key(data, password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
        raise ValueError("the key must be an ECDSA P-256 (prime256v1) private key")
    return key


def generate_private_key_pem() -> str:
    key = ec.generate_private_key(ec.SECP256R1())
    return key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    ).decode()


def public_key_b64(key: ec.EllipticCurvePrivateKey) -> str:
    """The form the device's `keyadd` expects: base64 of the 65-byte uncompressed point."""
    raw = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    return base64.b64encode(raw).decode()


def signed_string(nonce: str, key_id: str, method: str, path: str, body: bytes) -> bytes:
    return "\n".join(["CX5-V1", nonce, key_id, method, path, hashlib.sha256(body).hexdigest()]).encode()


def sign(key: ec.EllipticCurvePrivateKey, nonce: str, key_id: str, method: str, path: str, body: bytes) -> str:
    """Base64 of the raw 64-byte r||s signature the device expects."""
    der = key.sign(signed_string(nonce, key_id, method, path, body), ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()


@dataclass
class _Connection:
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    opened: float
    last_used: float

    def close(self) -> None:
        try:
            self.writer.close()
        except Exception:  # noqa: BLE001 - closing must never raise
            pass


class Cx5Client:
    def __init__(
        self,
        host: str,
        fingerprint: str,
        key_id: str,
        private_key_pem: str | bytes,
        *,
        port: int = 443,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> None:
        self.host = host
        self.port = port
        self.key_id = key_id
        self.timeout = timeout
        self._fingerprint = normalize_fingerprint(fingerprint)
        self._key = load_private_key(private_key_pem)
        self._lock = asyncio.Lock()          # the device has two connection slots: one call at a time
        self._conn: _Connection | None = None
        self.handshakes = 0                  # how many TLS connections were opened (for tests and diagnostics)

    # ------------------------------------------------------------------ connection handling
    async def _open(self) -> _Connection:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE          # replaced by the fingerprint pin below
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2   # the ESP32 server is only reliable on TLS 1.2
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port, ssl=ctx, server_hostname=self.host), self.timeout
            )
        except (OSError, asyncio.TimeoutError, ssl.SSLError) as err:
            raise Cx5ConnectionError(f"cannot connect to {self.host}:{self.port}: {err or type(err).__name__}") from err
        self.handshakes += 1
        ssl_object = writer.get_extra_info("ssl_object")
        der = ssl_object.getpeercert(binary_form=True) if ssl_object else None
        if not der or hashlib.sha256(der).hexdigest() != self._fingerprint:
            writer.close()
            raise Cx5PinError("the device's certificate does not match the pinned fingerprint; nothing was sent")
        now = time.monotonic()
        return _Connection(reader, writer, now, now)

    async def _get_connection(self) -> tuple[_Connection, bool]:
        """Returns (connection, reused)."""
        conn = self._conn
        if conn and (time.monotonic() - conn.last_used) < CONNECTION_REUSE_SECONDS and not conn.writer.is_closing():
            return conn, True
        if conn:
            conn.close()
        self._conn = None
        conn = await self._open()
        self._conn = conn
        return conn, False

    async def close(self) -> None:
        async with self._lock:
            if self._conn:
                self._conn.close()
                self._conn = None

    # ------------------------------------------------------------------ one HTTP exchange
    async def _exchange(
        self, conn: _Connection, method: str, path: str, body: bytes, headers: dict[str, str]
    ) -> tuple[int, dict[str, Any], bool]:
        head = [f"{method} {path} HTTP/1.1", f"Host: {self.host}", "Connection: keep-alive", "Accept: application/json"]
        if body:
            head += ["Content-Type: application/json", f"Content-Length: {len(body)}"]
        head += [f"{k}: {v}" for k, v in headers.items()]
        conn.writer.write(("\r\n".join(head) + "\r\n\r\n").encode() + body)
        await conn.writer.drain()

        status_line = await conn.reader.readline()
        if not status_line:
            raise ConnectionResetError("the device closed the connection")
        parts = status_line.decode(errors="replace").split(" ", 2)
        if len(parts) < 2 or not parts[0].startswith("HTTP/"):
            raise Cx5ConnectionError(f"unexpected reply: {status_line!r}")
        status = int(parts[1])
        response_headers: dict[str, str] = {}
        while True:
            line = await conn.reader.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            name, _, value = line.decode(errors="replace").partition(":")
            response_headers[name.strip().lower()] = value.strip()
        length = int(response_headers.get("content-length", "0") or 0)
        payload = await conn.reader.readexactly(length) if length else b""
        will_close = response_headers.get("connection", "").lower() == "close"
        try:
            data = json.loads(payload) if payload else {}
        except ValueError as err:
            raise Cx5ConnectionError("the device sent a reply that is not JSON") from err
        conn.last_used = time.monotonic()
        return status, data, will_close

    # ------------------------------------------------------------------ signed call
    async def call(self, method: str, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        """Performs one signed request and returns the parsed JSON body, or raises a Cx5Error."""
        async with self._lock:
            for attempt in (0, 1):
                conn, reused = await self._get_connection()
                try:
                    return await asyncio.wait_for(self._signed(conn, method, path, payload), self.timeout)
                except (ConnectionError, asyncio.IncompleteReadError, ssl.SSLError, asyncio.TimeoutError, OSError) as err:
                    conn.close()
                    self._conn = None
                    if reused and attempt == 0:
                        continue   # the device dropped an idle connection: retry once on a fresh one
                    raise Cx5ConnectionError(f"lost the connection to the device: {err or type(err).__name__}") from err
            raise Cx5ConnectionError("unreachable")  # pragma: no cover

    async def _signed(
        self, conn: _Connection, method: str, path: str, payload: dict[str, Any] | None
    ) -> dict[str, Any]:
        status, challenge, will_close = await self._exchange(conn, "GET", "/api/v1/challenge", b"", {})
        if will_close:
            raise ConnectionResetError("the device closed the connection after the challenge")
        self._raise_for_status(status, challenge)
        nonce = challenge["nonce"]
        body = json.dumps(payload, separators=(",", ":")).encode() if payload is not None else b""
        headers = {
            "X-CX5-Key": self.key_id,
            "X-CX5-Nonce": nonce,
            "X-CX5-Signature": sign(self._key, nonce, self.key_id, method, path, body),
        }
        status, data, will_close = await self._exchange(conn, method, path, body, headers)
        if will_close and self._conn is conn:
            conn.close()
            self._conn = None
        self._raise_for_status(status, data)
        return data

    @staticmethod
    def _raise_for_status(status: int, data: dict[str, Any]) -> None:
        if 200 <= status < 300:
            return
        code = str(data.get("error", "error"))
        if status == 401:
            raise Cx5AuthError("the device rejected the credentials")
        if status == 403:
            if code == "refused":
                raise Cx5RefusedError(str(data.get("reason", "refused")))
            raise Cx5ForbiddenError("this key is not allowed to do that")
        if status == 429:
            raise Cx5RateLimitedError("the device is rate limiting this address")
        raise Cx5DeviceError(status, code)

    # ------------------------------------------------------------------ the API, one method per endpoint
    async def state(self) -> dict[str, Any]:
        return await self.call("GET", "/api/v1/state")

    async def job(self, job_id: int) -> dict[str, Any]:
        return await self.call("GET", f"/api/v1/jobs/{job_id}")

    async def heater(self, seat: str, level: str) -> int:
        return (await self.call("POST", "/api/v1/heater", {"seat": seat, "level": level}))["job_id"]

    async def defrost(self, zone: str, on: bool) -> int:
        return (await self.call("POST", "/api/v1/defrost", {"zone": zone, "action": "on" if on else "off"}))["job_id"]

    async def temperature(self, value_f: int) -> int:
        return (await self.call("POST", "/api/v1/temperature", {"value": int(value_f)}))["job_id"]

    async def fan_level(self, level: int) -> int:
        return (await self.call("POST", "/api/v1/fan", {"level": int(level)}))["job_id"]

    async def fan_step(self, up: bool, steps: int = 1) -> int:
        return (await self.call("POST", "/api/v1/fan", {"action": "up" if up else "down", "steps": steps}))["job_id"]

    async def remote_start(self) -> int:
        return (await self.call("POST", "/api/v1/remote-start", {}))["job_id"]

    async def assume(self, **fields: Any) -> None:
        """Correct the device's tracked state without pressing anything (keys as in docs/api.md)."""
        await self.call("POST", "/api/v1/assume", fields)

    # admin
    async def keys(self) -> list[dict[str, Any]]:
        return (await self.call("GET", "/api/v1/admin/keys"))["keys"]

    async def add_key(self, key_id: str, role: str, public_key_b64_value: str) -> None:
        await self.call("POST", "/api/v1/admin/keys", {"id": key_id, "role": role, "public_key": public_key_b64_value})

    async def delete_key(self, key_id: str) -> None:
        await self.call("DELETE", f"/api/v1/admin/keys/{key_id}")

    async def wait_job(self, job_id: int, *, timeout: float = 60.0, interval: float = 1.5) -> dict[str, Any]:
        """Polls a job until it is done (returns the job) or the timeout passes (raises asyncio.TimeoutError)."""
        deadline = time.monotonic() + timeout
        while True:
            job = await self.job(job_id)
            if job.get("status") == "done":
                return job
            if time.monotonic() >= deadline:
                raise asyncio.TimeoutError(f"job {job_id} did not finish within {timeout} s")
            await asyncio.sleep(interval)
