#pragma once

// Drives the trigger output LOW. Call first in setup(), before anything else.
void remoteStartBegin();

// Pulses the module's trigger input, then waits for the engine input to confirm.
// Returns true only once the engine reads running (or if it was already running).
bool sendRemoteStart();
