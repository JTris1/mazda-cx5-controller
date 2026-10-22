#pragma once

// Drives the trigger output LOW. Call first in setup(), before anything else.
void remoteStartBegin();

// Policy for starts requested over the network (the serial `start` command bypasses it):
// off until enabled on the serial console, then a 60 s cooldown, max 3 per hour, 10 per day.
// Counters live in RAM and reset on reboot. The enabled flag is kept in flash.
bool remoteStartEnabled();
void setRemoteStartEnabled(bool on);
// True if a network-requested start may go ahead (and records it). Otherwise *reason says why.
bool remoteStartPolicyAllow(const char** reason);

// Pulses the module's trigger input, then waits for the engine input to confirm.
// Returns true only once the engine reads running (or if it was already running).
bool sendRemoteStart();
