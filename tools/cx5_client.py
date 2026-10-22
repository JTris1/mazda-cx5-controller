#!/usr/bin/env python3
"""Command-line client for the CX-5 controller's signed HTTPS API.

Setup (once):
    python3 -m venv ~/.venvs/cx5 && ~/.venvs/cx5/bin/pip install cryptography
    tools/cx5_client.py keygen laptop        # makes ~/.cx5/laptop.pem, prints the public key
    # on the ESP32 console:  keyadd laptop admin <the public key printed above>

Connection settings come from flags or environment variables:
    --host / CX5_HOST                  tunnel address of the ESP32 (e.g. 192.168.10.2)
    --fingerprint / CX5_FINGERPRINT    SHA-256 fingerprint of the device certificate (printed by
                                       tools/gen_cert.sh and by the console command `api`)
    --key-id / CX5_KEY_ID              id the key was enrolled under
    --key-file / CX5_KEY_FILE          private key file (default ~/.cx5/<key-id>.pem)

The certificate is not checked against a CA: the client pins the fingerprint and refuses to send
anything (even the challenge request) to a server that presents a different certificate.
"""
import argparse, base64, hashlib, http.client, json, os, socket, ssl, sys, time

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
except ImportError:
    sys.exit("This client needs the 'cryptography' package: pip install cryptography")


def key_dir():
    return os.path.expanduser("~/.cx5")


def load_key(path):
    with open(path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def public_key_b64(private_key):
    raw = private_key.public_key().public_bytes(
        serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    return base64.b64encode(raw).decode()


def sign(private_key, nonce, key_id, method, path, body):
    """Returns base64(r||s) over the signed string described in src/auth.h."""
    signed = "\n".join(["CX5-V1", nonce, key_id, method, path, hashlib.sha256(body).hexdigest()]).encode()
    r, s = decode_dss_signature(private_key.sign(signed, ec.ECDSA(hashes.SHA256())))
    return base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()


class Device:
    def __init__(self, host, fingerprint, key_id, private_key, timeout=30, timing=False, tls13=False):
        self.host, self.key_id, self.key = host, key_id, private_key
        self.timing = timing
        self.tls13 = tls13
        self.fingerprint = fingerprint.replace(":", "").lower()
        self.timeout = timeout

    def _connect(self):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE          # replaced by the fingerprint pin below
        if not self.tls13:
            # TLS 1.2 is what the ESP32 server is known to handle; TLS 1.3 reports a finished handshake
            # to the client before the server has verified it, which hides server-side failures.
            ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        conn = http.client.HTTPSConnection(self.host, 443, context=ctx, timeout=self.timeout)
        conn.auto_open = 0   # never reconnect silently: a new connection must go through the pin check
        t0 = time.monotonic()
        conn.connect()
        self._note(f"TCP + TLS handshake: {time.monotonic() - t0:.2f} s")
        seen = hashlib.sha256(conn.sock.getpeercert(binary_form=True)).hexdigest()
        if seen != self.fingerprint:
            conn.close()
            sys.exit(f"Certificate fingerprint mismatch.\n  expected {self.fingerprint}\n  got      {seen}\n"
                     "Refusing to send anything.")
        return conn

    def _note(self, text):
        if self.timing:
            print(f"  [timing] {text}", file=sys.stderr)

    def _exchange(self, conn, method, path, body=b"", headers=None, last=False):
        """One request on an open connection. Returns (status, data, server_will_close).

        The ESP32's HTTPS server only keeps a connection open if the request says so explicitly,
        so every request but the last asks for keep-alive, and the last one asks the server to close
        (it only has two connection slots)."""
        t0 = time.monotonic()
        h = {"Content-Type": "application/json", "Connection": "close" if last else "keep-alive", **(headers or {})}
        conn.request(method, path, body=body if body else None, headers=h)
        resp = conn.getresponse()
        data = resp.read()
        self._note(f"{method} {path}: {time.monotonic() - t0:.2f} s")
        try:
            parsed = json.loads(data) if data else {}
        except ValueError:
            parsed = {"raw": data.decode(errors="replace")}
        return resp.status, parsed, resp.will_close

    def call(self, method, path, payload=None):
        # One pinned connection for both the challenge and the signed request (the nonce is
        # single-use, but the TLS session is not), so the slow TLS handshake happens once.
        conn = self._connect()
        try:
            status, challenge, will_close = self._exchange(conn, "GET", "/api/v1/challenge")
            if status != 200:
                return status, challenge
            if will_close:               # the server closed it: open a new, pinned connection
                conn.close()
                conn = self._connect()
            body = json.dumps(payload, separators=(",", ":")).encode() if payload is not None else b""
            nonce = challenge["nonce"]
            headers = {"X-CX5-Key": self.key_id, "X-CX5-Nonce": nonce,
                       "X-CX5-Signature": sign(self.key, nonce, self.key_id, method, path, body)}
            status, data, _ = self._exchange(conn, method, path, body, headers, last=True)
            return status, data
        finally:
            conn.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=os.environ.get("CX5_HOST"))
    p.add_argument("--fingerprint", default=os.environ.get("CX5_FINGERPRINT"))
    p.add_argument("--key-id", default=os.environ.get("CX5_KEY_ID"))
    p.add_argument("--key-file", default=os.environ.get("CX5_KEY_FILE"))
    p.add_argument("--timing", action="store_true", help="print how long each step takes")
    p.add_argument("--tls13", action="store_true", help="allow TLS 1.3 (default is TLS 1.2 only)")
    sub = p.add_subparsers(dest="cmd", required=True)

    kg = sub.add_parser("keygen", help="create a new private key and print its public key")
    kg.add_argument("id")
    kg.add_argument("--role", choices=["control", "admin"], default="admin", help="role to put in the printed keyadd command (default admin)")
    pk = sub.add_parser("pubkey", help="print the public key for the configured key")

    sub.add_parser("state")
    h = sub.add_parser("heater"); h.add_argument("seat", choices=["driver", "passenger"]); h.add_argument("level", choices=["off", "low", "medium", "high"])
    d = sub.add_parser("defrost"); d.add_argument("zone", choices=["front", "rear"]); d.add_argument("action", choices=["on", "off"])
    t = sub.add_parser("temperature"); t.add_argument("value", type=int)
    f = sub.add_parser("fan"); f.add_argument("action", choices=["up", "down"]); f.add_argument("steps", type=int, nargs="?", default=1)
    sub.add_parser("power")
    sub.add_parser("remote-start")
    j = sub.add_parser("job"); j.add_argument("id", type=int)
    sub.add_parser("keys")
    ka = sub.add_parser("key-add"); ka.add_argument("id"); ka.add_argument("role", choices=["control", "admin"]); ka.add_argument("public_key")
    kd = sub.add_parser("key-delete"); kd.add_argument("id")
    args = p.parse_args()

    if args.cmd == "keygen":
        os.makedirs(key_dir(), mode=0o700, exist_ok=True)
        path = os.path.join(key_dir(), args.id + ".pem")
        if os.path.exists(path):
            sys.exit(f"{path} already exists; refusing to overwrite it")
        key = ec.generate_private_key(ec.SECP256R1())
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as out:
            out.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                        serialization.NoEncryption()))
        print(f"Private key written to {path} (keep it secret).")
        print("Public key (give this to the ESP32):")
        print(public_key_b64(key))
        print(f"\nPaste this line into the ESP32 console as it is:\n\nkeyadd {args.id} {args.role} {public_key_b64(key)}")
        return

    key_id = args.key_id
    if not key_id:
        sys.exit("Set --key-id or CX5_KEY_ID")
    key = load_key(args.key_file or os.path.join(key_dir(), key_id + ".pem"))
    if args.cmd == "pubkey":
        print(public_key_b64(key))
        return
    if not args.host or not args.fingerprint:
        sys.exit("Set --host and --fingerprint (or CX5_HOST / CX5_FINGERPRINT)")
    dev = Device(args.host, args.fingerprint, key_id, key, timing=args.timing, tls13=args.tls13)

    calls = {
        "state": lambda: dev.call("GET", "/api/v1/state"),
        "heater": lambda: dev.call("POST", "/api/v1/heater", {"seat": args.seat, "level": args.level}),
        "defrost": lambda: dev.call("POST", "/api/v1/defrost", {"zone": args.zone, "action": args.action}),
        "temperature": lambda: dev.call("POST", "/api/v1/temperature", {"value": args.value}),
        "fan": lambda: dev.call("POST", "/api/v1/fan", {"action": args.action, "steps": args.steps}),
        "power": lambda: dev.call("POST", "/api/v1/power", {"action": "toggle"}),
        "remote-start": lambda: dev.call("POST", "/api/v1/remote-start", {}),
        "job": lambda: dev.call("GET", f"/api/v1/jobs/{args.id}"),
        "keys": lambda: dev.call("GET", "/api/v1/admin/keys"),
        "key-add": lambda: dev.call("POST", "/api/v1/admin/keys", {"id": args.id, "role": args.role, "public_key": args.public_key}),
        "key-delete": lambda: dev.call("DELETE", f"/api/v1/admin/keys/{args.id}"),
    }
    status, data = calls[args.cmd]()
    print(f"HTTP {status}")
    print(json.dumps(data, indent=2))
    sys.exit(0 if 200 <= status < 300 else 1)


if __name__ == "__main__":
    main()
