"""Climate: target temperature and fan speed, both absolute and debounced."""
from __future__ import annotations

from homeassistant.components.climate import ClimateEntity, ClimateEntityFeature, HVACMode
from homeassistant.const import UnitOfTemperature

from .entity import Cx5Entity


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([Cx5Climate(entry.runtime_data)])


class Cx5Climate(Cx5Entity, ClimateEntity):
    _attr_temperature_unit = UnitOfTemperature.FAHRENHEIT
    _attr_hvac_modes = [HVACMode.AUTO]
    _attr_hvac_mode = HVACMode.AUTO
    _attr_supported_features = ClimateEntityFeature.TARGET_TEMPERATURE | ClimateEntityFeature.FAN_MODE
    _attr_target_temperature_step = 1
    _attr_assumed_state = True
    _attr_name = None
    _enable_turn_on_off_backwards_compat = False

    def __init__(self, c):
        super().__init__(c, "climate", "Climate")
        self._attr_name = None

    @property
    def min_temp(self) -> float:
        return float(self.coordinator.limits.get("temp_min_f", 60))

    @property
    def max_temp(self) -> float:
        return float(self.coordinator.limits.get("temp_max_f", 84))

    @property
    def fan_modes(self) -> list[str]:
        return [str(i) for i in range(1, int(self.coordinator.limits.get("fan_steps", 7)) + 1)]

    @property
    def target_temperature(self) -> float | None:
        v = self.coordinator.value("temperature")
        if v is None:
            # Home Assistant's thermostat hides its temperature controls when the target is null. Until the
            # controller has set a temperature once, show the middle of the range and flag it as a placeholder.
            return float(round((self.min_temp + self.max_temp) / 2))
        return float(v)

    @property
    def fan_mode(self) -> str | None:
        v = self.coordinator.value("fan")
        return None if v is None else str(v)

    @property
    def extra_state_attributes(self) -> dict:
        co = self.coordinator
        return {
            "assumed": True,
            "temperature_known": co.value("temperature") is not None,
            "pending_temperature": co.pending.get("temperature"),
            "pending_fan": co.pending.get("fan"),
            "pending": "temperature" in co.pending or "fan" in co.pending,
            "last_confirmed_temperature": co.tracked("temperature"),
            "last_confirmed_fan": co.tracked("fan"),
        }

    async def async_set_temperature(self, **kwargs) -> None:
        if (t := kwargs.get("temperature")) is not None:
            await self.coordinator.async_set("temperature", int(round(t)))

    async def async_set_fan_mode(self, fan_mode: str) -> None:
        await self.coordinator.async_set("fan", int(fan_mode))

    async def async_set_hvac_mode(self, hvac_mode: HVACMode) -> None:
        return None
