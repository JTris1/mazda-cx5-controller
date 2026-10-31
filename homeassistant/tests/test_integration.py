import asyncio

import pytest
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import entity_registry as er


async def until(cond, timeout=5.0):
    end = asyncio.get_event_loop().time() + timeout
    while not cond():
        if asyncio.get_event_loop().time() > end:
            raise AssertionError("condition not met in time")
        await asyncio.sleep(0.01)


def eid(hass, entry, key):
    reg = er.async_get(hass)
    return reg.async_get_entity_id_by_unique_id if False else next(
        e.entity_id for e in er.async_entries_for_config_entry(reg, entry.entry_id) if e.unique_id.endswith(f"_{key}"))


async def refresh(hass, entry):
    await entry.runtime_data.async_refresh()
    await hass.async_block_till_done()


async def test_entities_created(hass, integration):
    reg = er.async_get(hass)
    keys = {e.unique_id.split("_", 1)[1] for e in er.async_entries_for_config_entry(reg, integration.entry_id)}
    assert {"remote_start", "reset_tracked", "climate", "heater_driver", "heater_passenger", "defrost_front",
            "defrost_rear", "remote_start_state", "last_command", "uptime", "firmware", "connectivity",
            "engine_running", "remote_start_enabled"} <= keys
    assert hass.states.get(eid(hass, integration, "remote_start_state")).state == "idle"
    assert hass.states.get(eid(hass, integration, "engine_running")).state == "off"


async def test_temperature_taps_coalesce_into_one_command(hass, integration, device):
    climate = eid(hass, integration, "climate")
    for t in (71, 72, 73, 75):
        await hass.services.async_call("climate", "set_temperature", {"entity_id": climate, "temperature": t}, blocking=True)
    st = hass.states.get(climate)
    assert st.attributes["temperature"] == 75 and st.attributes["pending"] is True   # shows the target at once
    await until(lambda: hass.states.get(climate).attributes["pending"] is False and device.temperature == 75)
    assert [c for c in device.calls if c["path"] == "/api/v1/temperature"].__len__() == 1
    assert hass.states.get(climate).attributes["temperature"] == 75


async def test_new_target_during_run_is_applied_after(hass, integration, device):
    device.press_seconds = 0.02
    climate = eid(hass, integration, "climate")
    await hass.services.async_call("climate", "set_temperature", {"entity_id": climate, "temperature": 70}, blocking=True)
    await until(lambda: device.running_job is not None)
    await hass.services.async_call("climate", "set_temperature", {"entity_id": climate, "temperature": 80}, blocking=True)
    await until(lambda: hass.states.get(climate).attributes["pending"] is False)
    assert device.temperature == 80
    assert hass.states.get(climate).attributes["temperature"] == 80


async def test_fan_mode(hass, integration, device):
    climate = eid(hass, integration, "climate")
    await hass.services.async_call("climate", "set_fan_mode", {"entity_id": climate, "fan_mode": "5"}, blocking=True)
    await until(lambda: device.fan == 5 and not integration.runtime_data.pending)
    assert hass.states.get(climate).attributes["fan_mode"] == "5"


async def test_heater_select_pending_then_confirmed(hass, integration, device):
    device.press_seconds = 0.03
    sel = eid(hass, integration, "heater_driver")
    await hass.services.async_call("select", "select_option", {"entity_id": sel, "option": "medium"}, blocking=True)
    st = hass.states.get(sel)
    assert st.state == "medium" and st.attributes["pending"] is True
    await until(lambda: hass.states.get(sel).attributes["pending"] is False)
    assert hass.states.get(sel).state == "medium" and device.heater["driver"] == 2


async def test_defrost_switch(hass, integration, device):
    sw = eid(hass, integration, "defrost_front")
    await hass.services.async_call("switch", "turn_on", {"entity_id": sw}, blocking=True)
    await until(lambda: device.defrost["front"] and not integration.runtime_data.pending)
    assert hass.states.get(sw).state == "on" and hass.states.get(sw).attributes["assumed_state"] is True


async def test_failed_command_reverts_and_notifies(hass, integration, device):
    events = []
    hass.bus.async_listen("cx5_controller_command_failed", lambda e: events.append(e))
    sel = eid(hass, integration, "heater_passenger")
    device.fail_next.append((503, {"error": "busy"}))
    await hass.services.async_call("select", "select_option", {"entity_id": sel, "option": "high"}, blocking=True)
    assert hass.states.get(sel).state == "high"                              # shown as pending
    await until(lambda: hass.states.get(sel).attributes["pending"] is False)
    assert hass.states.get(sel).state == "off"                               # reverted to the device's value
    await hass.async_block_till_done()
    assert events and events[0].data["control"] == "heater_passenger"
    assert any("failed" in n["title"].lower() for n in hass.data["persistent_notification"].values())


async def test_remote_start_sequence(hass, integration, device):
    device.start_seconds = 0.3
    button = eid(hass, integration, "remote_start")
    sensor = eid(hass, integration, "remote_start_state")
    events = []
    hass.bus.async_listen("cx5_controller_remote_start", lambda e: events.append(e))
    task = hass.async_create_task(hass.services.async_call("button", "press", {"entity_id": button}, blocking=True))
    await until(lambda: hass.states.get(sensor).state == "starting")
    assert hass.states.get(button).state == "unavailable"                    # cannot be pressed twice
    await task
    await until(lambda: hass.states.get(sensor).state == "confirmed")
    assert hass.states.get(eid(hass, integration, "engine_running")).state == "on"
    assert hass.states.get(button).state == "unavailable"                    # engine already running
    await hass.async_block_till_done()
    assert events[-1].data["result"] == "confirmed"


async def test_remote_start_failure_is_reported(hass, integration, device):
    device.engine_will_start = False
    button = eid(hass, integration, "remote_start")
    events = []
    hass.bus.async_listen("cx5_controller_remote_start", lambda e: events.append(e))
    await hass.services.async_call("button", "press", {"entity_id": button}, blocking=True)
    await until(lambda: events)
    assert events[0].data["result"] == "failed"
    await refresh(hass, integration)
    assert hass.states.get(eid(hass, integration, "remote_start_state")).state == "failed"


async def test_remote_start_refused(hass, integration, device):
    device.remote_start_enabled = False
    await refresh(hass, integration)
    button = eid(hass, integration, "remote_start")
    assert hass.states.get(button).state == "unavailable"                    # disabled on the device
    device.remote_start_enabled = True
    await refresh(hass, integration)
    device.start_times.append(device.now())                                  # cooldown active
    with pytest.raises(HomeAssistantError, match="cooldown"):
        await hass.services.async_call("button", "press", {"entity_id": button}, blocking=True)
    await hass.async_block_till_done()
    assert hass.states.get(eid(hass, integration, "remote_start_state")).state == "refused"


async def test_offline_makes_entities_unavailable_and_rejects_commands(hass, integration, device):
    await device.stop()
    await refresh(hass, integration)
    sel = eid(hass, integration, "heater_driver")
    assert hass.states.get(sel).state == "unavailable"
    assert hass.states.get(eid(hass, integration, "connectivity")).state == "off"
    with pytest.raises(HomeAssistantError, match="offline"):
        await integration.runtime_data.async_set("heater_driver", "low")
    device._worker = None  # already cancelled by stop()


async def test_polling_speeds_up_with_engine(hass, integration, device):
    from datetime import timedelta
    assert integration.runtime_data.update_interval == timedelta(seconds=60)
    device.set_engine(True)
    await refresh(hass, integration)
    assert integration.runtime_data.update_interval == timedelta(seconds=15)


async def test_resync_service(hass, integration, device):
    await hass.services.async_call("cx5_controller", "resync", {"temperature": 68, "driver_heater": "high", "rear_defrost": True}, blocking=True)
    assert device.temperature == 68 and device.heater["driver"] == 1 and device.defrost["rear"] is True
    assert device.log_press == []
    assert hass.states.get(eid(hass, integration, "heater_driver")).state == "high"


async def test_reset_tracked_button(hass, integration, device):
    await integration.runtime_data.async_assume(temperature=70, fan=3, heater={"driver": "high"})
    await hass.services.async_call("button", "press", {"entity_id": eid(hass, integration, "reset_tracked")}, blocking=True)
    assert device.temperature is None and device.fan is None and device.heater["driver"] == 0


async def test_config_flow(hass, device, key_pem):
    from homeassistant import config_entries
    r = await hass.config_entries.flow.async_init("cx5_controller", context={"source": config_entries.SOURCE_USER})
    assert r["type"] == "form"
    base = {"host": device.host, "port": device.port, "key_id": "ha", "private_key": key_pem}
    bad = await hass.config_entries.flow.async_configure(r["flow_id"], {**base, "fingerprint": "00" * 32})
    assert bad["errors"] == {"base": "pin_mismatch"}
    bad = await hass.config_entries.flow.async_configure(r["flow_id"], {**base, "fingerprint": "nope"})
    assert bad["errors"] == {"fingerprint": "invalid_fingerprint"}
    bad = await hass.config_entries.flow.async_configure(r["flow_id"], {**base, "private_key": "junk", "fingerprint": device.fingerprint})
    assert bad["errors"] == {"private_key": "invalid_key"}
    ok = await hass.config_entries.flow.async_configure(r["flow_id"], {**base, "fingerprint": device.fingerprint})
    assert ok["type"] == "create_entry"
    await hass.async_block_till_done()
    for e in hass.config_entries.async_entries("cx5_controller"):
        await hass.config_entries.async_unload(e.entry_id)
    await hass.async_block_till_done()


async def test_config_flow_generates_a_key_and_waits_for_enrollment(hass, device):
    from homeassistant import config_entries
    from custom_components.cx5_controller.api import load_private_key, public_key_b64
    r = await hass.config_entries.flow.async_init("cx5_controller", context={"source": config_entries.SOURCE_USER})
    r = await hass.config_entries.flow.async_configure(r["flow_id"], {
        "host": device.host, "port": device.port, "key_id": "ha",
        "fingerprint": ":".join(device.fingerprint[i:i + 2] for i in range(0, 64, 2)).upper(), "private_key": ""})
    assert r["step_id"] == "enroll"
    pub = r["description_placeholders"]["public_key"]
    assert r["description_placeholders"]["key_id"] == "ha"
    device.keys.clear()
    r = await hass.config_entries.flow.async_configure(r["flow_id"], {})          # not enrolled yet
    assert r["errors"] == {"base": "not_enrolled"}
    device.add_key("ha", "control", pub)                                          # the user enrolls it on the admin page
    r = await hass.config_entries.flow.async_configure(r["flow_id"], {})
    assert r["type"] == "create_entry"
    assert public_key_b64(load_private_key(r["data"]["private_key"])) == pub
    await hass.async_block_till_done()
    for e in hass.config_entries.async_entries("cx5_controller"):
        await hass.config_entries.async_unload(e.entry_id)
    await hass.async_block_till_done()


async def test_unknown_temperature_still_shows_a_target_so_the_card_is_usable(hass, integration, device):
    climate = eid(hass, integration, "climate")
    st = hass.states.get(climate)
    assert st.attributes["temperature"] == 72 and st.attributes["temperature_known"] is False
    assert st.attributes["min_temp"] == 60 and st.attributes["max_temp"] == 84

