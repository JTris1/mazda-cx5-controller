"""Coordinator: polls the device and runs commands with pending / confirm / revert behaviour.

Why this is more than a polling loop
    The car cannot report heater, defrost, temperature or fan state, so the device keeps a *tracked*
    state (what it last pressed). Commands are slow (a temperature run is 10-15 s, a start 3-10 s), so:

    * Every control has at most one command in flight. A newer target for the same control replaces the
      older one (slow controls wait for DEBOUNCE_SLOW of quiet first), so tapping + five times sends one
      absolute command.
    * While a command is queued or running the entity shows the *pending* target and says so.
    * When the job finishes the coordinator refreshes from the device, which is the single source of
      truth: success shows the device's value, failure drops the pending value (the entity reverts) and
      raises a notification and an event.
    * One shared poller follows jobs. The device runs jobs in order and rate limits challenges, so asking
      about the oldest unfinished job is enough.
"""
from __future__ import annotations

import asyncio
import logging
from datetime import datetime, timezone
from typing import Any

from homeassistant.components import persistent_notification
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryAuthFailed, HomeAssistantError
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import Cx5AuthError, Cx5Client, Cx5Error, Cx5ForbiddenError, Cx5RateLimitedError, Cx5RefusedError
from .const import (
    DEBOUNCE_NONE, DEBOUNCE_SLOW, DOMAIN, EVENT_COMMAND_FAILED, EVENT_REMOTE_START, JOB_POLL_SECONDS,
    JOB_TIMEOUT_SECONDS, POLL_ENGINE, POLL_IDLE, RS_STARTING,
)

_LOGGER = logging.getLogger(__name__)

# control key -> debounce seconds
CONTROLS = {
    "temperature": DEBOUNCE_SLOW,
    "fan": DEBOUNCE_SLOW,
    "heater_driver": DEBOUNCE_NONE,
    "heater_passenger": DEBOUNCE_NONE,
    "defrost_front": DEBOUNCE_NONE,
    "defrost_rear": DEBOUNCE_NONE,
}
CONTROL_NAMES = {
    "temperature": "temperature", "fan": "fan speed",
    "heater_driver": "driver seat heater", "heater_passenger": "passenger seat heater",
    "defrost_front": "front defrost", "defrost_rear": "rear defrost",
}


class Cx5Coordinator(DataUpdateCoordinator[dict[str, Any]]):
    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, client: Cx5Client, *, debounce_scale: float = 1.0) -> None:
        super().__init__(hass, _LOGGER, config_entry=entry, name=DOMAIN, update_interval=POLL_IDLE)
        self.client = client
        self.entry = entry
        self.debounce_scale = debounce_scale        # tests shrink the debounce
        self.pending: dict[str, Any] = {}           # control -> target the user asked for, not yet confirmed
        self.last_command: dict[str, Any] | None = None
        self.remote_start_local: str | None = None  # "starting" while a remote start we sent is in progress
        self._targets: dict[str, Any] = {}
        self._tasks: dict[str, asyncio.Task] = {}
        self._sleeping: set[str] = set()
        self._job_waiters: dict[int, asyncio.Future] = {}
        self._job_poller: asyncio.Task | None = None

    # ------------------------------------------------------------------ polling
    async def _async_update_data(self) -> dict[str, Any]:
        try:
            data = await self.client.state()
        except (Cx5AuthError, Cx5ForbiddenError) as err:
            raise ConfigEntryAuthFailed(str(err)) from err
        except Cx5RateLimitedError as err:
            raise UpdateFailed(str(err)) from err
        except Cx5Error as err:
            raise UpdateFailed(str(err)) from err
        self.update_interval = POLL_ENGINE if data.get("engine_running") else POLL_IDLE
        return data

    async def async_shutdown(self) -> None:
        for task in list(self._tasks.values()):
            task.cancel()
        if self._job_poller:
            self._job_poller.cancel()
        for fut in self._job_waiters.values():
            if not fut.done():
                fut.cancel()
        await asyncio.gather(*self._tasks.values(), *( [self._job_poller] if self._job_poller else [] ), return_exceptions=True)
        await self.client.close()
        await super().async_shutdown()

    # ------------------------------------------------------------------ helpers for entities
    @property
    def online(self) -> bool:
        return self.last_update_success and self.data is not None

    @property
    def climate(self) -> dict[str, Any]:
        return (self.data or {}).get("climate", {})

    @property
    def limits(self) -> dict[str, Any]:
        return (self.data or {}).get("limits", {})

    def tracked(self, control: str) -> Any:
        c = self.climate
        if control == "temperature":
            return c.get("temperature_f")
        if control == "fan":
            return c.get("fan")
        kind, _, where = control.partition("_")
        return c.get(kind, {}).get(where)

    def value(self, control: str) -> Any:
        """What an entity should show: the pending target if there is one, else the device's tracked value."""
        return self.pending[control] if control in self.pending else self.tracked(control)

    @property
    def remote_start_state(self) -> str:
        return self.remote_start_local or (self.data or {}).get("remote_start", {}).get("state", "idle")

    @property
    def remote_start_available(self) -> bool:
        rs = (self.data or {}).get("remote_start", {})
        return bool(self.online and rs.get("enabled") and not self.data.get("engine_running")
                    and self.remote_start_state != RS_STARTING)

    # ------------------------------------------------------------------ commands
    def _require_online(self) -> None:
        if not self.online:
            raise HomeAssistantError("The CX-5 controller is offline, so the command was not sent.")

    async def async_set(self, control: str, target: Any) -> None:
        """Ask for a new value. Returns immediately; the entity shows `target` as pending until the job finishes."""
        self._require_online()
        self._targets[control] = target
        self.pending[control] = target
        self.async_update_listeners()
        task = self._tasks.get(control)
        if task and not task.done():
            if control not in self._sleeping:
                return                      # a job for this control is running: it will pick up the new target
            task.cancel()                   # still debouncing: restart the quiet period
        task = self.hass.async_create_background_task(
            self._run(control), f"{DOMAIN} {control}", eager_start=False)
        self._tasks[control] = task

    async def _run(self, control: str) -> None:
        me = asyncio.current_task()
        try:
            self._sleeping.add(control)
            try:
                await asyncio.sleep(CONTROLS[control] * self.debounce_scale)
            finally:
                self._sleeping.discard(control)
            while True:
                target = self._targets[control]
                ok, message = await self._execute(control, target)
                self._record(control, target, ok, message)
                if self._targets[control] != target:
                    continue                # a newer target arrived while this one ran
                break
        except asyncio.CancelledError:
            raise
        finally:
            if self._tasks.get(control) is me:
                self._tasks.pop(control, None)
                self.pending.pop(control, None)
        await self.async_refresh()
        self.async_update_listeners()

    async def _execute(self, control: str, target: Any) -> tuple[bool, str]:
        try:
            if control == "temperature":
                job = await self.client.temperature(int(target))
            elif control == "fan":
                job = await self.client.fan_level(int(target))
            elif control.startswith("heater_"):
                job = await self.client.heater(control.split("_", 1)[1], target)
            else:
                job = await self.client.defrost(control.split("_", 1)[1], bool(target))
            result = await self._wait_job(job)
        except Cx5Error as err:
            return False, str(err)
        except asyncio.TimeoutError:
            return False, "the device did not report a result in time"
        return bool(result.get("ok")), str(result.get("result", ""))

    def _record(self, control: str, target: Any, ok: bool, message: str) -> None:
        self.last_command = {"control": control, "target": target, "ok": ok, "message": message,
                             "time": datetime.now(timezone.utc)}
        if ok:
            return
        name = CONTROL_NAMES[control]
        text = f"Could not set the {name} to {target}: {message}. It was left as the device last reported it."
        self.hass.bus.async_fire(EVENT_COMMAND_FAILED, {"entry_id": self.entry.entry_id, "control": control,
                                                          "target": target, "reason": message})
        persistent_notification.async_create(self.hass, text, title="CX-5 command failed",
                                             notification_id=f"{DOMAIN}_{self.entry.entry_id}_{control}")

    # ------------------------------------------------------------------ job following
    async def _wait_job(self, job_id: int) -> dict[str, Any]:
        fut: asyncio.Future = self.hass.loop.create_future()
        self._job_waiters[job_id] = fut
        if self._job_poller is None or self._job_poller.done():
            self._job_poller = self.hass.async_create_background_task(self._poll_jobs(), f"{DOMAIN} jobs", eager_start=False)
        try:
            return await asyncio.wait_for(fut, JOB_TIMEOUT_SECONDS)
        finally:
            self._job_waiters.pop(job_id, None)

    async def _poll_jobs(self) -> None:
        failures = 0
        while self._job_waiters:
            job_id = min(self._job_waiters)
            try:
                job = await self.client.job(job_id)
                failures = 0
            except Cx5Error as err:
                failures += 1
                if failures >= 5:
                    for fut in self._job_waiters.values():
                        if not fut.done():
                            fut.set_exception(err)
                    return
                await asyncio.sleep(JOB_POLL_SECONDS * self.debounce_scale)
                continue
            fut = self._job_waiters.get(job_id)
            if job.get("status") == "done":
                if fut and not fut.done():
                    fut.set_result(job)
                continue                    # look at the next job straight away
            await asyncio.sleep(JOB_POLL_SECONDS * self.debounce_scale)

    # ------------------------------------------------------------------ remote start
    async def async_remote_start(self) -> None:
        self._require_online()
        if not self.remote_start_available:
            raise HomeAssistantError("Remote start is not available right now (disabled, already running, or in progress).")
        self.remote_start_local = RS_STARTING
        self.async_update_listeners()
        try:
            try:
                job = await self.client.remote_start()
            except Cx5RefusedError as err:
                self._remote_start_event("refused", err.reason)
                raise HomeAssistantError(f"The device refused the remote start: {err.reason}") from err
            except Cx5Error as err:
                self._remote_start_event("failed", str(err))
                raise HomeAssistantError(f"Remote start was not sent: {err}") from err
            try:
                result = await self._wait_job(job)
            except (Cx5Error, asyncio.TimeoutError) as err:
                self._remote_start_event("failed", str(err) or "no result from the device")
                return
            ok = bool(result.get("ok"))
            self.last_command = {"control": "remote_start", "target": None, "ok": ok,
                                 "message": str(result.get("result", "")), "time": datetime.now(timezone.utc)}
            self._remote_start_event("confirmed" if ok else "failed", str(result.get("result", "")))
        finally:
            self.remote_start_local = None
            await self.async_refresh()
            self.async_update_listeners()

    def _remote_start_event(self, result: str, detail: str) -> None:
        self.hass.bus.async_fire(EVENT_REMOTE_START, {"entry_id": self.entry.entry_id, "result": result, "detail": detail})
        if result != "confirmed":
            persistent_notification.async_create(
                self.hass, f"Remote start {result}: {detail}", title="CX-5 remote start",
                notification_id=f"{DOMAIN}_{self.entry.entry_id}_remote_start")
        else:
            persistent_notification.async_dismiss(self.hass, f"{DOMAIN}_{self.entry.entry_id}_remote_start")

    # ------------------------------------------------------------------ resync
    async def async_assume(self, **fields: Any) -> None:
        """Tell the device what the car really shows. Presses nothing."""
        self._require_online()
        await self.client.assume(**fields)
        await self.async_request_refresh()
