"""Runs the mock device on http://localhost:8765 so the admin page can be tried in a browser.

    python tests/serve_mock.py [--admin-key-b64 <public key>]   # optional: pre-enroll an admin key
Stop with Ctrl-C. Plain HTTP on localhost is a secure context, so WebCrypto works without certificate warnings.
"""
import argparse
import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tests.mock_device import MockDevice  # noqa: E402


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--admin-key-b64")
    args = ap.parse_args()
    dev = MockDevice(tls=False, press_seconds=0.05)
    dev.port = args.port
    if args.admin_key_b64:
        dev.add_key("admin", "admin", args.admin_key_b64)
    await dev.start()
    print(f"mock device on http://127.0.0.1:{dev.port}/admin", flush=True)
    await asyncio.Event().wait()


asyncio.run(main())
