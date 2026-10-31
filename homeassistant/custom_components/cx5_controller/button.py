"""Buttons: remote start (an action, never a toggle) and a tracked-state reset."""
from __future__ import annotations

from homeassistant.components.button import ButtonEntity
from homeassistant.const import EntityCategory

from .entity import Cx5Entity


async def async_setup_entry(hass, entry, async_add_entities):
    c = entry.runtime_data
    async_add_entities([RemoteStartButton(c), ResetTrackedButton(c)])


class RemoteStartButton(Cx5Entity, ButtonEntity):
    _attr_icon = "mdi:engine"

    def __init__(self, c):
        super().__init__(c, "remote_start", "Remote start")

    @property
    def available(self) -> bool:
        return self.coordinator.remote_start_available

    async def async_press(self) -> None:
        await self.coordinator.async_remote_start()


class ResetTrackedButton(Cx5Entity, ButtonEntity):
    """Use after the physical buttons were used: heaters and defrost are set to off, temperature and fan become
    unknown (the next change then homes them), without pressing anything in the car."""
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_icon = "mdi:backup-restore"

    def __init__(self, c):
        super().__init__(c, "reset_tracked", "Reset tracked state")

    async def async_press(self) -> None:
        await self.coordinator.async_assume(
            temperature=None, fan=None,
            heater={"driver": "off", "passenger": "off"}, defrost={"front": False, "rear": False})
