"""Binary sensors."""
from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.const import EntityCategory

from .entity import Cx5Entity


async def async_setup_entry(hass, entry, async_add_entities):
    c = entry.runtime_data
    async_add_entities([Connectivity(c), EngineRunning(c), RemoteStartEnabled(c)])


class Connectivity(Cx5Entity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, c):
        super().__init__(c, "connectivity", "Connectivity")

    @property
    def available(self) -> bool:
        return True

    @property
    def is_on(self) -> bool:
        return self.coordinator.online


class EngineRunning(Cx5Entity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.RUNNING

    def __init__(self, c):
        super().__init__(c, "engine_running", "Engine")

    @property
    def is_on(self) -> bool | None:
        return (self.coordinator.data or {}).get("engine_running")


class RemoteStartEnabled(Cx5Entity, BinarySensorEntity):
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, c):
        super().__init__(c, "remote_start_enabled", "Remote start enabled")

    @property
    def is_on(self) -> bool | None:
        return (self.coordinator.data or {}).get("remote_start", {}).get("enabled")
