"""Firmware update flow against the mock (mirrors src/ota.cpp and the handlers in src/admin_api.cpp)."""
import asyncio
import hashlib
import ssl

import pytest

from custom_components.cx5_controller.api import (
    Cx5Client, Cx5DeviceError, generate_private_key_pem, load_private_key, public_key_b64,
)


def image(size=150_000, magic=b"\xe9", fill=b"\x5a"):
    return magic + fill * (size - 1)


async def put(device, token, data):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    r, w = await asyncio.open_connection(device.host, device.port, ssl=ctx)
    w.write(f"PUT /api/v1/admin/firmware/{token} HTTP/1.1\r\nHost: x\r\nContent-Length: {len(data)}\r\nConnection: close\r\n\r\n".encode() + data)
    await w.drain()
    raw = await r.read()
    w.close()
    return int(raw.split()[1]), raw.partition(b"\r\n\r\n")[2]


@pytest.fixture
async def admin(device):
    pem = generate_private_key_pem()
    device.add_key("root", "admin", public_key_b64(load_private_key(pem)))
    c = Cx5Client(device.host, device.fingerprint, "root", pem, port=device.port)
    yield c
    await c.close()


async def start(admin, data, version="0.3.0"):
    return (await admin.call("POST", "/api/v1/admin/firmware",
                             {"size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "version": version}))["upload_token"]


async def test_happy_path_then_reboot_runs_new_slot_on_probation(admin, device):
    data = image()
    token = await start(admin, data)
    assert (await admin.call("GET", "/api/v1/admin/firmware"))["state"] == "armed"
    status, _ = await put(device, token, data)
    assert status == 200
    st = await admin.call("GET", "/api/v1/admin/firmware")
    assert st["state"] == "ready_to_reboot" and st["version"] == "0.3.0"
    await admin.call("POST", "/api/v1/admin/reboot", {})
    st = await admin.call("GET", "/api/v1/admin/firmware")
    assert st["running_slot"] == "app1" and st["pending_verify"] is True
    assert (await admin.state())["firmware"] == "0.3.0"


async def test_token_is_single_use(admin, device):
    data = image()
    token = await start(admin, data)
    assert (await put(device, token, data))[0] == 200
    assert (await put(device, token, data))[0] == 403


async def test_wrong_token_is_rejected_and_disarms(admin, device):
    data = image()
    token = await start(admin, data)
    assert (await put(device, "0" * 32, data))[0] == 403
    assert (await put(device, token, data))[0] == 403        # the real token no longer works either


@pytest.mark.parametrize("mutate,reason", [
    (lambda d: d[:-1] + b"\x00", "sha256 mismatch"),
    (lambda d: d + b"x", "size does not match"),
    (lambda d: d[:-10], "size does not match"),
])
async def test_tampered_upload_is_discarded(admin, device, mutate, reason):
    data = image()
    token = await start(admin, data)
    status, body = await put(device, token, mutate(data))
    assert status == 400 and reason.encode() in body
    assert (await admin.call("GET", "/api/v1/admin/firmware"))["state"] == "idle"
    assert device.slot == "app0"


async def test_not_an_esp_image_is_rejected(admin, device):
    data = image(magic=b"\x00")
    token = await start(admin, data)
    status, body = await put(device, token, data)
    assert status == 400 and b"not a valid firmware image" in body


@pytest.mark.parametrize("body", [{"size": 10, "sha256": "0" * 64, "version": "x"}, {"size": 150000, "sha256": "zz", "version": "x"},
                                  {"size": 9_999_999, "sha256": "0" * 64, "version": "x"}])
async def test_start_validates_the_manifest(admin, body):
    with pytest.raises(Cx5DeviceError) as e:
        await admin.call("POST", "/api/v1/admin/firmware", body)
    assert e.value.status == 409


async def test_refused_while_busy_or_engine_running(admin, device):
    device.press_seconds = 0.05
    ctl_pem = generate_private_key_pem()
    device.add_key("ctl", "control", public_key_b64(load_private_key(ctl_pem)))
    ctl = Cx5Client(device.host, device.fingerprint, "ctl", ctl_pem, port=device.port)
    await ctl.temperature(70)
    with pytest.raises(Cx5DeviceError, match="409"):
        await start(admin, image())
    await ctl.wait_job(1, timeout=10, interval=0.02)
    device.set_engine(True)
    with pytest.raises(Cx5DeviceError, match="409"):
        await start(admin, image())
    await ctl.close()


async def test_control_key_cannot_start_an_update(client):
    from custom_components.cx5_controller.api import Cx5ForbiddenError
    with pytest.raises(Cx5ForbiddenError):
        await client.call("POST", "/api/v1/admin/firmware", {"size": 150000, "sha256": "0" * 64, "version": "x"})


async def test_unsigned_start_is_rejected(device):
    from custom_components.cx5_controller.api import Cx5AuthError
    c = Cx5Client(device.host, device.fingerprint, "nobody", generate_private_key_pem(), port=device.port)
    with pytest.raises(Cx5AuthError):
        await c.call("POST", "/api/v1/admin/firmware", {"size": 150000, "sha256": "0" * 64, "version": "x"})
    await c.close()
