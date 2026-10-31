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

// What the last remote start request did, for the dashboard (reported by the API's state call).
enum class RemoteStartState : uint8_t { Idle, Starting, Confirmed, Failed, Refused };
struct RemoteStartInfo {
  RemoteStartState state;
  char detail[48];      // short reason or result, e.g. "confirmed after 3010 ms", "not confirmed"
  uint32_t sinceMs;     // millis() when state last changed
};
void remoteStartInfo(RemoteStartInfo& out);
const char* remoteStartStateName(RemoteStartState s);
void remoteStartNoteRefused(const char* reason);   // the API calls this when the policy says no

// Pulses the module's trigger input, then waits for the engine input to confirm.
// Returns true only once the engine reads running (or if it was already running).
bool sendRemoteStart();
