"""Defrost, as assumed-state switches (the car does not report them)."""
from __future__ import annotations

from homeassistant.components.switch import SwitchEntity

from .entity import Cx5Entity


async def async_setup_entry(hass, entry, async_add_entities):
    c = entry.runtime_data
    async_add_entities([DefrostSwitch(c, "front"), DefrostSwitch(c, "rear")])


class DefrostSwitch(Cx5Entity, SwitchEntity):
    _attr_assumed_state = True
    _attr_icon = "mdi:car-defrost-front"

    def __init__(self, c, zone: str):
        super().__init__(c, f"defrost_{zone}", f"{zone.title()} defrost")
        self._control = f"defrost_{zone}"
        if zone == "rear":
            self._attr_icon = "mdi:car-defrost-rear"

    @property
    def is_on(self) -> bool | None:
        return self.coordinator.value(self._control)

    @property
    def extra_state_attributes(self) -> dict:
        return self._control_attrs(self._control)

    async def async_turn_on(self, **kwargs) -> None:
        await self.coordinator.async_set(self._control, True)

    async def async_turn_off(self, **kwargs) -> None:
        await self.coordinator.async_set(self._control, False)
