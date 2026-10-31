"""Status sensors."""
from __future__ import annotations

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity
from homeassistant.const import EntityCategory, UnitOfTime

from .entity import Cx5Entity

RS_STATES = ["idle", "starting", "confirmed", "failed", "refused"]


async def async_setup_entry(hass, entry, async_add_entities):
    c = entry.runtime_data
    async_add_entities([RemoteStartSensor(c), LastCommandSensor(c), UptimeSensor(c), FirmwareSensor(c)])


class RemoteStartSensor(Cx5Entity, SensorEntity):
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_options = RS_STATES
    _attr_icon = "mdi:engine"

    def __init__(self, c):
        super().__init__(c, "remote_start_state", "Remote start status")

    @property
    def native_value(self) -> str:
        return self.coordinator.remote_start_state

    @property
    def extra_state_attributes(self) -> dict:
        rs = (self.coordinator.data or {}).get("remote_start", {})
        return {"detail": rs.get("detail", ""), "age_s": rs.get("age_s")}


class LastCommandSensor(Cx5Entity, SensorEntity):
    _attr_icon = "mdi:history"

    def __init__(self, c):
        super().__init__(c, "last_command", "Last command")

    @property
    def available(self) -> bool:
        return True

    @property
    def native_value(self) -> str | None:
        last = self.coordinator.last_command
        if not last:
            return None
        return ("OK: " if last["ok"] else "Failed: ") + (last["message"] or last["control"])[:240]

    @property
    def extra_state_attributes(self) -> dict:
        last = self.coordinator.last_command or {}
        return {k: (v.isoformat() if hasattr(v, "isoformat") else v) for k, v in last.items()}


class UptimeSensor(Cx5Entity, SensorEntity):
    _attr_device_class = SensorDeviceClass.DURATION
    _attr_native_unit_of_measurement = UnitOfTime.SECONDS
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, c):
        super().__init__(c, "uptime", "Uptime")

    @property
    def native_value(self):
        return (self.coordinator.data or {}).get("uptime_s")


class FirmwareSensor(Cx5Entity, SensorEntity):
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, c):
        super().__init__(c, "firmware", "Firmware")

    @property
    def native_value(self):
        return (self.coordinator.data or {}).get("firmware")
