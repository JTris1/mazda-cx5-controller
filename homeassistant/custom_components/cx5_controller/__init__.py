"""CX-5 climate remote: Home Assistant integration."""
from __future__ import annotations

import voluptuous as vol
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.typing import ConfigType

from .api import Cx5Client
from .const import CONF_FINGERPRINT, CONF_KEY_ID, CONF_PRIVATE_KEY, DEFAULT_PORT, DOMAIN, HEATER_OPTIONS
from .coordinator import Cx5Coordinator

PLATFORMS = ["binary_sensor", "button", "climate", "select", "sensor", "switch"]

RESYNC_SCHEMA = vol.Schema({
    vol.Optional("config_entry_id"): cv.string,
    vol.Optional("temperature"): vol.Any(None, vol.All(vol.Coerce(int), vol.Range(min=40, max=100))),
    vol.Optional("fan"): vol.Any(None, vol.All(vol.Coerce(int), vol.Range(min=1, max=12))),
    vol.Optional("driver_heater"): vol.In(HEATER_OPTIONS),
    vol.Optional("passenger_heater"): vol.In(HEATER_OPTIONS),
    vol.Optional("front_defrost"): cv.boolean,
    vol.Optional("rear_defrost"): cv.boolean,
})


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    async def resync(call: ServiceCall) -> None:
        data = dict(call.data)
        only = data.pop("config_entry_id", None)
        fields: dict = {}
        for key in ("temperature", "fan"):
            if key in data:
                fields[key] = data[key]
        heater = {s: data[f"{s}_heater"] for s in ("driver", "passenger") if f"{s}_heater" in data}
        defrost = {z: data[f"{z}_defrost"] for z in ("front", "rear") if f"{z}_defrost" in data}
        if heater:
            fields["heater"] = heater
        if defrost:
            fields["defrost"] = defrost
        for entry in hass.config_entries.async_entries(DOMAIN):
            if only and entry.entry_id != only:
                continue
            coordinator = getattr(entry, "runtime_data", None)
            if coordinator:
                await coordinator.async_assume(**fields)

    hass.services.async_register(DOMAIN, "resync", resync, schema=RESYNC_SCHEMA)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    client = Cx5Client(entry.data["host"], entry.data[CONF_FINGERPRINT], entry.data[CONF_KEY_ID],
                       entry.data[CONF_PRIVATE_KEY], port=entry.data.get("port", DEFAULT_PORT))
    coordinator = Cx5Coordinator(hass, entry, client)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if ok:
        await entry.runtime_data.async_shutdown()
    return ok
