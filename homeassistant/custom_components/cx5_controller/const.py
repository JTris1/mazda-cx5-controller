"""Constants for the CX-5 climate remote integration."""
from datetime import timedelta

DOMAIN = "cx5_controller"

CONF_FINGERPRINT = "fingerprint"
CONF_KEY_ID = "key_id"
CONF_PRIVATE_KEY = "private_key"

DEFAULT_PORT = 443

# Polling. The device is on cellular data, so idle polling is slow; it speeds up while the engine runs.
POLL_IDLE = timedelta(seconds=60)
POLL_ENGINE = timedelta(seconds=15)

# The device allows 30 challenges per minute per address and every call (state, job poll) uses one.
JOB_POLL_SECONDS = 3.0
JOB_TIMEOUT_SECONDS = 120.0

# Controls with a slow "homing" run wait for the user to stop tapping before sending one absolute command.
DEBOUNCE_SLOW = 1.5
DEBOUNCE_NONE = 0.0

HEATER_OPTIONS = ["off", "low", "medium", "high"]

EVENT_REMOTE_START = f"{DOMAIN}_remote_start"
EVENT_COMMAND_FAILED = f"{DOMAIN}_command_failed"

RS_STARTING = "starting"
