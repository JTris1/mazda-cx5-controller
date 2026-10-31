#pragma once
#include <Arduino.h>
#include "climate.h"

// Every physical action (button presses, remote start) goes through this queue and is run
// one at a time by a single worker task. The serial console and, later, the HTTP API both
// submit here, so a slow action (remote start waits up to 20 s) never blocks the caller's
// event loop and two sources can never press buttons at the same time.

enum class Submit : uint8_t { Ok, BadArgs, NoExpander, Busy, Disabled };
enum class JobState : uint8_t { Queued, Running, Done };

struct JobInfo {
  uint32_t id;
  JobState state;
  bool ok;            // valid when state == Done
  char message[48];   // short result, e.g. "done", "confirmed", "not confirmed"
};

// Creates the queue and starts the worker task. Call after Serial.begin() and logBegin().
void actionsBegin();

// Each submit validates its arguments, queues the job and (optionally) returns its id.
Submit actionTempStep(bool up, int n, uint32_t* id = nullptr);
Submit actionSetTemperature(int targetF, uint32_t* id = nullptr);
Submit actionSetHeater(Seat seat, int level, uint32_t* id = nullptr);
Submit actionSetDefrost(DefrostZone zone, bool on, uint32_t* id = nullptr);
Submit actionFanStep(bool up, int n, uint32_t* id = nullptr);
Submit actionSetFan(int level, uint32_t* id = nullptr);   // absolute 1..cfg.fanSteps
Submit actionPower(uint32_t* id = nullptr);
Submit actionRemoteStart(uint32_t* id = nullptr);

// Job status. Only the most recent few jobs are remembered.
bool actionsGet(uint32_t id, JobInfo& out);
bool actionsWait(uint32_t id, uint32_t timeoutMs, JobInfo& out);

// Exclusive access to the outputs for manual tools (the bench command). Waits for the
// current job to finish; queued jobs wait until actionsUnlock().
bool actionsLock(uint32_t timeoutMs);
void actionsUnlock();

// What the worker is doing right now: the running job id (0 if idle) and how many jobs wait behind it.
void actionsBusyInfo(uint32_t& runningId, int& queued);

const char* submitText(Submit s);
