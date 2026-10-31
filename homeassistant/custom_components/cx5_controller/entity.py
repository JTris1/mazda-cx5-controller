"""Shared entity base."""
from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import Cx5Coordinator


class Cx5Entity(CoordinatorEntity[Cx5Coordinator]):
    _attr_has_entity_name = True

    def __init__(self, coordinator: Cx5Coordinator, key: str, name: str) -> None:
        super().__init__(coordinator)
        self._key = key
        self._attr_name = name
        self._attr_unique_id = f"{coordinator.entry.entry_id}_{key}"

    @property
    def device_info(self) -> DeviceInfo:
        fw = (self.coordinator.data or {}).get("firmware")
        return DeviceInfo(identifiers={(DOMAIN, self.coordinator.entry.entry_id)}, name="CX-5 climate remote",
                          manufacturer="DIY", model="ESP32-S3 + SIM7670G", sw_version=fw)

    @property
    def available(self) -> bool:
        return self.coordinator.online

    def _control_attrs(self, control: str) -> dict:
        co = self.coordinator
        return {"assumed": True, "pending": control in co.pending, "last_confirmed": co.tracked(control)}
