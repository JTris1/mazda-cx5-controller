import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from custom_components.cx5_controller.api import Cx5Client, generate_private_key_pem, load_private_key, public_key_b64  # noqa: E402
from tests.mock_device import MockDevice  # noqa: E402


@pytest.fixture
def key_pem():
    return generate_private_key_pem()


@pytest.fixture
async def device(key_pem):
    async with MockDevice() as dev:
        dev.add_key("ha", "control", public_key_b64(load_private_key(key_pem)))
        yield dev


@pytest.fixture
async def client(device, key_pem):
    c = Cx5Client(device.host, device.fingerprint, "ha", key_pem, port=device.port, timeout=5)
    yield c
    await c.close()


@pytest.fixture(autouse=True)
def _allow_loopback(socket_enabled):
    """The mock device is a real TLS server on 127.0.0.1; the HA test plugin blocks sockets by default."""
    yield


@pytest.fixture(autouse=True)
def _auto_enable_custom_integrations(enable_custom_integrations):
    yield


@pytest.fixture
def fast(monkeypatch):
    """Shrink the integration's waits so tests run in milliseconds."""
    from custom_components.cx5_controller import coordinator as co
    for k in co.CONTROLS:
        monkeypatch.setitem(co.CONTROLS, k, 0.05 if co.CONTROLS[k] else 0.0)
    monkeypatch.setattr(co, "JOB_POLL_SECONDS", 0.02)


@pytest.fixture
async def integration(hass, device, key_pem, fast):
    from homeassistant.util.unit_system import US_CUSTOMARY_SYSTEM
    from pytest_homeassistant_custom_component.common import MockConfigEntry
    hass.config.units = US_CUSTOMARY_SYSTEM
    entry = MockConfigEntry(domain="cx5_controller", unique_id="x", data={
        "host": device.host, "port": device.port, "fingerprint": device.fingerprint,
        "key_id": "ha", "private_key": key_pem})
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    yield entry
    await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()
