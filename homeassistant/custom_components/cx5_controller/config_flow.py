"""Config flow: host, certificate fingerprint, key id and private key."""
from __future__ import annotations

from typing import Any

import voluptuous as vol
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.helpers.selector import TextSelector, TextSelectorConfig

from .api import (
    generate_private_key_pem, public_key_b64,
    Cx5AuthError, Cx5Client, Cx5ConnectionError, Cx5ForbiddenError, Cx5PinError, load_private_key, normalize_fingerprint,
)
from .const import CONF_FINGERPRINT, CONF_KEY_ID, CONF_PRIVATE_KEY, DEFAULT_PORT, DOMAIN


async def _validate(user_input: dict[str, Any]) -> tuple[dict[str, str], dict[str, Any] | None]:
    errors: dict[str, str] = {}
    try:
        fingerprint = normalize_fingerprint(user_input[CONF_FINGERPRINT])
    except ValueError:
        return {CONF_FINGERPRINT: "invalid_fingerprint"}, None
    try:
        load_private_key(user_input[CONF_PRIVATE_KEY])
    except Exception:  # noqa: BLE001
        return {CONF_PRIVATE_KEY: "invalid_key"}, None
    client = Cx5Client(user_input["host"], fingerprint, user_input[CONF_KEY_ID], user_input[CONF_PRIVATE_KEY],
                       port=user_input.get("port", DEFAULT_PORT), timeout=25)
    try:
        state = await client.state()
    except Cx5PinError:
        errors["base"] = "pin_mismatch"
    except Cx5ConnectionError:
        errors["base"] = "cannot_connect"
    except (Cx5AuthError, Cx5ForbiddenError):
        errors["base"] = "invalid_auth"
    except Exception:  # noqa: BLE001
        errors["base"] = "unknown"
    finally:
        await client.close()
    return errors, (None if errors else state)


def _schema(defaults: dict[str, Any]) -> vol.Schema:
    return vol.Schema({
        vol.Required("host", default=defaults.get("host", "")): str,
        vol.Optional("port", default=defaults.get("port", DEFAULT_PORT)): int,
        vol.Required(CONF_FINGERPRINT, default=defaults.get(CONF_FINGERPRINT, "")): str,
        vol.Required(CONF_KEY_ID, default=defaults.get(CONF_KEY_ID, "homeassistant")): str,
        vol.Optional(CONF_PRIVATE_KEY, default=defaults.get(CONF_PRIVATE_KEY, "")):
            TextSelector(TextSelectorConfig(multiline=True)),
    })


class Cx5ConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1
    _data: dict[str, Any]

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            await self.async_set_unique_id(f"{user_input['host']}:{user_input.get('port', DEFAULT_PORT)}")
            self._abort_if_unique_id_configured()
            data = dict(user_input)
            try:
                data[CONF_FINGERPRINT] = normalize_fingerprint(user_input[CONF_FINGERPRINT])
            except ValueError:
                errors[CONF_FINGERPRINT] = "invalid_fingerprint"
            else:
                if not (data.get(CONF_PRIVATE_KEY) or "").strip():
                    # No key supplied: make one here. The private key never leaves Home Assistant.
                    data[CONF_PRIVATE_KEY] = generate_private_key_pem()
                    self._data = data
                    return await self.async_step_enroll()
                errors, _ = await _validate(data)
                if not errors:
                    return self.async_create_entry(title="CX-5 climate remote", data=data)
        return self.async_show_form(step_id="user", data_schema=_schema(user_input or {}), errors=errors)

    async def async_step_enroll(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """Shows the generated public key; finishes once the controller accepts it."""
        errors: dict[str, str] = {}
        if user_input is not None:
            errors, _ = await _validate(self._data)
            if not errors:
                return self.async_create_entry(title="CX-5 climate remote", data=self._data)
            if errors.get("base") == "invalid_auth":
                errors["base"] = "not_enrolled"
        return self.async_show_form(
            step_id="enroll", data_schema=vol.Schema({}), errors=errors,
            description_placeholders={"key_id": self._data[CONF_KEY_ID],
                                      "public_key": public_key_b64(load_private_key(self._data[CONF_PRIVATE_KEY]))})

    async def async_step_reauth(self, entry_data: dict[str, Any]) -> ConfigFlowResult:
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        entry = self._get_reauth_entry()
        errors: dict[str, str] = {}
        if user_input is not None:
            merged = {**entry.data, **user_input}
            errors, _ = await _validate(merged)
            if not errors:
                merged[CONF_FINGERPRINT] = normalize_fingerprint(merged[CONF_FINGERPRINT])
                return self.async_update_reload_and_abort(entry, data=merged)
        schema = vol.Schema({
            vol.Required(CONF_FINGERPRINT, default=entry.data[CONF_FINGERPRINT]): str,
            vol.Required(CONF_KEY_ID, default=entry.data[CONF_KEY_ID]): str,
            vol.Required(CONF_PRIVATE_KEY): TextSelector(TextSelectorConfig(multiline=True)),
        })
        return self.async_show_form(step_id="reauth_confirm", data_schema=schema, errors=errors)
