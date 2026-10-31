import asyncio

import pytest

from custom_components.cx5_controller.api import (
    Cx5AuthError, Cx5Client, Cx5ConnectionError, Cx5DeviceError, Cx5ForbiddenError, Cx5PinError,
    Cx5RateLimitedError, Cx5RefusedError, generate_private_key_pem, load_private_key, public_key_b64,
)


async def run_job(client, job_id):
    return await client.wait_job(job_id, timeout=10, interval=0.01)


async def test_state(client):
    s = await client.state()
    assert s["firmware"] == "0.2.0"
    assert s["climate"]["heater"] == {"driver": "off", "passenger": "off"}


async def test_connection_reuse_one_handshake(client, device):
    for _ in range(4):
        await client.state()
    assert client.handshakes == 1 and device.connections == 1


async def test_reconnects_after_idle_drop(client, device):
    await client.state()
    device.idle_timeout = 0.05
    await client.close()
    await client.state()
    assert client.handshakes == 2


async def test_dropped_reused_connection_retries(client, device):
    await client.state()
    # server forgets the connection: simulate by closing client-side socket underneath
    client._conn.writer.transport.abort()
    assert (await client.state())["firmware"]
    assert client.handshakes == 2


async def test_pin_mismatch_sends_nothing(device, key_pem):
    c = Cx5Client(device.host, "00" * 32, "ha", key_pem, port=device.port, timeout=5)
    with pytest.raises(Cx5PinError):
        await c.state()
    assert device.calls == [] and not device.nonces


async def test_unknown_key_rejected(device):
    c = Cx5Client(device.host, device.fingerprint, "ha", generate_private_key_pem(), port=device.port, timeout=5)
    await device.stop() if False else None
    other = Cx5Client(device.host, device.fingerprint, "nobody", generate_private_key_pem(), port=device.port)
    with pytest.raises(Cx5AuthError):
        await other.state()
    with pytest.raises(Cx5AuthError):   # right id, wrong private key
        await c.state()
    await c.close(); await other.close()


async def test_control_key_cannot_use_admin(client):
    with pytest.raises(Cx5ForbiddenError):
        await client.keys()


async def test_admin_key_management(device):
    admin_pem = generate_private_key_pem()
    device.add_key("admin", "admin", public_key_b64(load_private_key(admin_pem)))
    c = Cx5Client(device.host, device.fingerprint, "admin", admin_pem, port=device.port)
    new = public_key_b64(load_private_key(generate_private_key_pem()))
    await c.add_key("laptop", "control", new)
    assert {k["id"] for k in await c.keys()} == {"ha", "admin", "laptop"}
    with pytest.raises(Cx5DeviceError):
        await c.delete_key("admin")
    await c.delete_key("laptop")
    assert "laptop" not in device.keys
    await c.close()


async def test_heater_cycle_presses_and_state(client, device):
    r = await run_job(client, await client.heater("driver", "medium"))
    assert r["ok"] and device.log_press == ["heater_driver"] * 2
    device.log_press.clear()
    await run_job(client, await client.heater("driver", "off"))
    assert device.log_press == ["heater_driver"] * 2      # medium(2) -> low(3) -> off(0)
    assert (await client.state())["climate"]["heater"]["driver"] == "off"


async def test_defrost_idempotent(client, device):
    await run_job(client, await client.defrost("front", True))
    await run_job(client, await client.defrost("front", True))
    assert device.log_press == ["defrost_front"]
    assert (await client.state())["climate"]["defrost"]["front"] is True


async def test_temperature_homing(client, device):
    await run_job(client, await client.temperature(72))
    assert device.log_press.count("temp_down") == 26 and device.log_press.count("temp_up") == 12
    assert (await client.state())["climate"]["temperature_f"] == 72
    with pytest.raises(Cx5DeviceError):
        await client.temperature(90)


async def test_fan_level_and_step(client, device):
    await run_job(client, await client.fan_level(4))
    assert device.log_press.count("fan_down") == 7 and device.log_press.count("fan_up") == 3
    await run_job(client, await client.fan_step(True, 2))
    assert (await client.state())["climate"]["fan"] == 6
    with pytest.raises(Cx5DeviceError):
        await client.fan_level(8)


async def test_jobs_run_one_at_a_time_and_busy_shown(client, device):
    device.press_seconds = 0.01
    a = await client.temperature(70)
    b = await client.heater("passenger", "high")
    s = await client.state()
    assert s["busy"]["running"] in (a, b)
    ra, rb = await run_job(client, a), await run_job(client, b)
    assert ra["ok"] and rb["ok"]


async def test_remote_start_confirmed(client, device):
    r = await run_job(client, await client.remote_start())
    assert r["ok"]
    s = await client.state()
    assert s["engine_running"] and s["remote_start"]["state"] == "confirmed"


async def test_remote_start_not_confirmed(client, device):
    device.engine_will_start = False
    r = await run_job(client, await client.remote_start())
    assert not r["ok"]
    assert (await client.state())["remote_start"]["state"] == "failed"


async def test_remote_start_policy(client, device):
    await run_job(client, await client.remote_start())
    with pytest.raises(Cx5RefusedError, match="cooldown"):
        await client.remote_start()
    device.remote_start_enabled = False
    device.start_times.clear()
    with pytest.raises(Cx5RefusedError, match="disabled"):
        await client.remote_start()
    assert (await client.state())["remote_start"]["state"] == "refused"


async def test_assume_updates_tracked_state_without_presses(client, device):
    await client.assume(temperature=70, fan=3, heater={"driver": "low"}, defrost={"rear": True})
    c = (await client.state())["climate"]
    assert (c["temperature_f"], c["fan"], c["heater"]["driver"], c["defrost"]["rear"]) == (70, 3, "low", True)
    assert device.log_press == []
    with pytest.raises(Cx5DeviceError):
        await client.assume(fan=99)
    with pytest.raises(Cx5DeviceError):
        await client.assume()


async def test_engine_stop_resets_heaters(client, device):
    await client.assume(heater={"driver": "high"}, defrost={"front": True})
    device.set_engine(True)
    device.set_engine(False)
    c = (await client.state())["climate"]
    assert c["heater"]["driver"] == "off" and c["defrost"]["front"] is False


async def test_power_is_disabled(client):
    with pytest.raises(Cx5DeviceError) as e:
        await client.call("POST", "/api/v1/power", {"action": "toggle"})
    assert e.value.status == 503


async def test_rate_limit_after_repeated_failures(device):
    bad = Cx5Client(device.host, device.fingerprint, "ha", generate_private_key_pem(), port=device.port)
    for _ in range(5):
        with pytest.raises(Cx5AuthError):
            await bad.state()
    with pytest.raises(Cx5RateLimitedError):
        await bad.state()
    await bad.close()


async def test_nonce_cannot_be_replayed(device, key_pem):
    from custom_components.cx5_controller.api import sign
    import hashlib, json, ssl
    key = load_private_key(key_pem)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    r, w = await asyncio.open_connection(device.host, device.port, ssl=ctx)

    async def req(method, path, headers=""):
        w.write(f"{method} {path} HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n{headers}\r\n".encode())
        await w.drain()
        head = await r.readuntil(b"\r\n\r\n")
        n = int([l for l in head.decode().split("\r\n") if l.lower().startswith("content-length")][0].split(":")[1])
        return int(head.split()[1]), json.loads(await r.readexactly(n))

    _, ch = await req("GET", "/api/v1/challenge")
    h = f"X-CX5-Key: ha\r\nX-CX5-Nonce: {ch['nonce']}\r\nX-CX5-Signature: {sign(key, ch['nonce'], 'ha', 'GET', '/api/v1/state', b'')}\r\n"
    assert (await req("GET", "/api/v1/state", h))[0] == 200
    assert (await req("GET", "/api/v1/state", h))[0] == 401
    w.close()


async def test_nonce_expires(device, client):
    device.nonces.clear()
    # age every issued nonce beyond the TTL by shifting the clock the device sees
    base = device.now
    device.now = lambda: base() + 0
    orig = device._authenticate
    def aged(*a, **k):
        device.now = lambda: base() + 31
        return orig(*a, **k)
    device._authenticate = aged
    with pytest.raises(Cx5AuthError):
        await client.state()


async def test_unreachable_device_is_connection_error(key_pem):
    c = Cx5Client("127.0.0.1", "00" * 32, "ha", key_pem, port=1, timeout=1)
    with pytest.raises(Cx5ConnectionError):
        await c.state()


async def test_timeout_is_connection_error(client, device):
    client.timeout = 0.2
    device.response_delay = 0.4
    with pytest.raises(Cx5ConnectionError):
        await client.state()
    await client.close()
    await asyncio.sleep(0.6)   # let the delayed mock handler finish before the loop closes


async def test_challenge_rate_limit_matches_firmware(device, client):
    device.challenge_limit = 3
    await client.state()
    await client.close()
    with pytest.raises(Cx5RateLimitedError):
        for _ in range(5):
            await client.state()
