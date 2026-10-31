"""Seat heaters."""
from __future__ import annotations

from homeassistant.components.select import SelectEntity

from .const import HEATER_OPTIONS
from .entity import Cx5Entity


async def async_setup_entry(hass, entry, async_add_entities):
    c = entry.runtime_data
    async_add_entities([HeaterSelect(c, "driver"), HeaterSelect(c, "passenger")])


class HeaterSelect(Cx5Entity, SelectEntity):
    _attr_options = HEATER_OPTIONS
    _attr_icon = "mdi:car-seat-heater"
    _attr_assumed_state = True

    def __init__(self, c, seat: str):
        super().__init__(c, f"heater_{seat}", f"{seat.title()} seat heater")
        self._control = f"heater_{seat}"

    @property
    def current_option(self) -> str | None:
        return self.coordinator.value(self._control)

    @property
    def extra_state_attributes(self) -> dict:
        return self._control_attrs(self._control)

    async def async_select_option(self, option: str) -> None:
        await self.coordinator.async_set(self._control, option)
