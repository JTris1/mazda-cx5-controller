"""Admin, setup and settings endpoints, as implemented by the mock (which mirrors src/admin_api.cpp)."""
import json
import ssl

import pytest

from custom_components.cx5_controller.api import (
    Cx5Client, Cx5DeviceError, Cx5ForbiddenError, generate_private_key_pem, load_private_key, public_key_b64,
)
from tests.mock_device import MockDevice

import asyncio


async def https(device, method, path, body=None):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    r, w = await asyncio.open_connection(device.host, device.port, ssl=ctx)
    raw = json.dumps(body).encode() if body is not None else b""
    w.write(f"{method} {path} HTTP/1.1\r\nHost: x\r\nContent-Length: {len(raw)}\r\nConnection: close\r\n\r\n".encode() + raw)
    await w.drain()
    data = await r.read()
    w.close()
    head, _, payload = data.partition(b"\r\n\r\n")
    return int(head.split()[1]), head.decode(), payload


@pytest.fixture
async def admin(device):
    pem = generate_private_key_pem()
    device.add_key("root", "admin", public_key_b64(load_private_key(pem)))
    c = Cx5Client(device.host, device.fingerprint, "root", pem, port=device.port)
    yield c
    await c.close()


async def test_setup_open_only_while_no_keys():
    async with MockDevice() as dev:
        status, _, body = await https(dev, "GET", "/api/v1/setup")
        assert json.loads(body)["setup_open"] is True
        pub = public_key_b64(load_private_key(generate_private_key_pem()))
        status, _, body = await https(dev, "POST", "/api/v1/setup/first-key", {"id": "first", "public_key": pub})
        assert status == 201 and json.loads(body)["role"] == "admin"
        status, _, body = await https(dev, "POST", "/api/v1/setup/first-key", {"id": "second", "public_key": pub})
        assert status == 409 and json.loads(body)["error"] == "setup_closed"
        assert json.loads((await https(dev, "GET", "/api/v1/setup"))[2])["setup_open"] is False


async def test_setup_rejects_bad_key_and_is_rate_limited():
    async with MockDevice() as dev:
        for _ in range(5):
            status, _, _ = await https(dev, "POST", "/api/v1/setup/first-key", {"id": "x", "public_key": "AAAA"})
            assert status == 400
        status, _, _ = await https(dev, "POST", "/api/v1/setup/first-key", {"id": "x", "public_key": "AAAA"})
        assert status == 429


async def test_page_is_served_with_csp(device):
    status, head, body = await https(device, "GET", "/admin")
    assert status == 200 and b"<title>" in body
    assert "content-security-policy: default-src 'none'" in head.lower()


async def test_unknown_path_is_404_without_touching_the_auth_log(device):
    assert (await https(device, "GET", "/favicon.ico"))[0] == 404
    assert device.auth_log == []


async def test_info_log_and_settings(admin, device):
    info = await admin.call("GET", "/api/v1/admin/info")
    assert info["you"] == "root" and info["running_slot"] == "app0"
    bad = Cx5Client(device.host, device.fingerprint, "ghost", generate_private_key_pem(), port=device.port)
    with pytest.raises(Exception):
        await bad.state()
    await bad.close()
    log = (await admin.call("GET", "/api/v1/admin/log"))["entries"]
    assert log[0]["key"] == "ghost" and log[0]["result"] == "unknown key"

    s = await admin.call("POST", "/api/v1/admin/settings", {"fan_steps": 6, "reset_on_engine_stop": False})
    assert s["fan_steps"] == 6 and s["reset_on_engine_stop"] is False
    assert (await admin.state())["limits"]["fan_steps"] == 6


@pytest.mark.parametrize("body", [{"temp_min_f": 90}, {"fan_steps": 99}, {"nope": 1}, {"fan_steps": "7"}, {"rs_pulses": 0},
                                  {"reset_on_engine_stop": 1}])
async def test_settings_validation_changes_nothing(admin, device, body):
    before = dict(device.settings)
    with pytest.raises(Cx5DeviceError) as e:
        await admin.call("POST", "/api/v1/admin/settings", {"fan_steps": 5, **body})
    assert e.value.status == 400 and device.settings == before


async def test_settings_refused_while_busy(admin, device):
    device.press_seconds = 0.05
    await admin.temperature(70) if False else None
    ctl_pem = generate_private_key_pem()
    device.add_key("ctl", "control", public_key_b64(load_private_key(ctl_pem)))
    ctl = Cx5Client(device.host, device.fingerprint, "ctl", ctl_pem, port=device.port)
    await ctl.temperature(70)
    with pytest.raises(Cx5DeviceError) as e:
        await admin.call("POST", "/api/v1/admin/settings", {"fan_steps": 5})
    assert e.value.status == 503
    await ctl.wait_job(1, timeout=10, interval=0.02)
    await ctl.close()


async def test_range_change_forgets_tracked_values(admin, device):
    await admin.assume(temperature=84, fan=7)
    await admin.call("POST", "/api/v1/admin/settings", {"temp_max_f": 80, "fan_steps": 5})
    c = (await admin.state())["climate"]
    assert c["temperature_f"] is None and c["fan"] is None


async def test_remote_start_enable_and_reboot(admin, device):
    assert (await admin.call("POST", "/api/v1/admin/remote-start-enabled", {"enabled": False}))["enabled"] is False
    assert (await admin.state())["remote_start"]["enabled"] is False
    with pytest.raises(Cx5DeviceError):
        await admin.call("POST", "/api/v1/admin/remote-start-enabled", {"enabled": "yes"})
    await admin.call("POST", "/api/v1/admin/reboot", {})
    assert device.rebooted == 1


async def test_keys_expose_public_key_and_role_change(admin, device):
    other = public_key_b64(load_private_key(generate_private_key_pem()))
    await admin.add_key("phone", "control", other)
    listed = {k["id"]: k for k in await admin.keys()}
    assert listed["phone"]["public_key"] == other
    await admin.add_key("phone", "admin", listed["phone"]["public_key"])      # re-adding the same id changes the role
    assert device.keys["phone"]["role"] == "admin"


async def test_cannot_change_own_role(admin, device):
    own = {k["id"]: k for k in await admin.keys()}["root"]["public_key"]
    with pytest.raises(Cx5DeviceError) as e:
        await admin.add_key("root", "control", own)
    assert e.value.status == 409 and e.value.code == "cannot_change_own_role"


async def test_control_key_cannot_reach_admin_endpoints(client):
    for path in ("/api/v1/admin/info", "/api/v1/admin/settings", "/api/v1/admin/log"):
        with pytest.raises(Cx5ForbiddenError):
            await client.call("GET", path)


async def test_device_logs_and_link_status(admin, device):
    logs = await admin.call("GET", "/api/v1/admin/logs")
    assert "Link watchdog" in logs["previous"] and "Booting" in logs["current"]
    assert (await admin.call("GET", "/api/v1/admin/info"))["link"]["tunnel_up"] is True

