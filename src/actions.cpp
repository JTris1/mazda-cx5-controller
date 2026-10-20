#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "actions.h"
#include "config.h"
#include "engine.h"
#include "log.h"
#include "outputs.h"
#include "remote_start.h"

enum class ActionType : uint8_t { TempStep, SetTemperature, SetHeater, SetDefrost, FanStep, Power, RemoteStart };

struct Job {
  uint32_t id;
  ActionType type;
  int a;
  int b;
};

static constexpr int QUEUE_DEPTH = 8;
static constexpr int HISTORY = 16;  // larger than QUEUE_DEPTH so queued jobs are never overwritten

static QueueHandle_t queue = nullptr;
static SemaphoreHandle_t outputsMutex = nullptr;  // held while a job runs or a manual tool is active
static SemaphoreHandle_t jobsMutex = nullptr;     // protects the history ring and the id counter
static JobInfo history[HISTORY];
static uint32_t nextId = 1;

// --- job history ----------------------------------------------------------------------
static uint32_t recordNew() {
  xSemaphoreTake(jobsMutex, portMAX_DELAY);
  uint32_t id = nextId++;
  JobInfo& j = history[id % HISTORY];
  j.id = id;
  j.state = JobState::Queued;
  j.ok = false;
  j.message[0] = '\0';
  xSemaphoreGive(jobsMutex);
  return id;
}

static void setState(uint32_t id, JobState state, bool ok = false, const char* message = "") {
  xSemaphoreTake(jobsMutex, portMAX_DELAY);
  JobInfo& j = history[id % HISTORY];
  if (j.id == id) {
    j.state = state;
    j.ok = ok;
    strncpy(j.message, message, sizeof(j.message) - 1);
    j.message[sizeof(j.message) - 1] = '\0';
  }
  xSemaphoreGive(jobsMutex);
}

bool actionsGet(uint32_t id, JobInfo& out) {
  bool found = false;
  xSemaphoreTake(jobsMutex, portMAX_DELAY);
  const JobInfo& j = history[id % HISTORY];
  if (id != 0 && j.id == id) {
    out = j;
    found = true;
  }
  xSemaphoreGive(jobsMutex);
  return found;
}

bool actionsWait(uint32_t id, uint32_t timeoutMs, JobInfo& out) {
  uint32_t start = millis();
  while (actionsGet(id, out)) {
    if (out.state == JobState::Done) return true;
    if (millis() - start > timeoutMs) return false;
    delay(10);
  }
  return false;  // the job fell out of the history ring
}

// --- worker --------------------------------------------------------------------------
static void runJob(const Job& job) {
  bool ok = true;
  const char* message = "done";
  switch (job.type) {
    case ActionType::TempStep:       climateTempStep(job.a != 0, job.b); break;
    case ActionType::SetTemperature: climateSetTemperature(job.a); break;
    case ActionType::SetHeater:      climateSetHeater(job.a == 0 ? SEAT_DRIVER : SEAT_PASSENGER, job.b); break;
    case ActionType::SetDefrost:     climateSetDefrost(job.a == 0 ? DEFROST_FRONT : DEFROST_REAR, job.b != 0); break;
    case ActionType::FanStep:        climateFanStep(job.a != 0, job.b); break;
    case ActionType::Power:          climatePowerPress(); break;
    case ActionType::RemoteStart:
      ok = sendRemoteStart();
      message = ok ? "confirmed" : "not confirmed";
      break;
  }
  setState(job.id, JobState::Done, ok, message);
}

static void workerTask(void*) {
  Job job;
  for (;;) {
    if (xQueueReceive(queue, &job, portMAX_DELAY) != pdTRUE) continue;
    xSemaphoreTake(outputsMutex, portMAX_DELAY);
    setState(job.id, JobState::Running);
    runJob(job);
    xSemaphoreGive(outputsMutex);
  }
}

void actionsBegin() {
  queue = xQueueCreate(QUEUE_DEPTH, sizeof(Job));
  outputsMutex = xSemaphoreCreateMutex();
  jobsMutex = xSemaphoreCreateMutex();
  for (JobInfo& j : history) j.id = 0;
  xTaskCreatePinnedToCore(workerTask, "actions", 6144, nullptr, 1, nullptr, 1);
}

bool actionsLock(uint32_t timeoutMs) {
  return xSemaphoreTake(outputsMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void actionsUnlock() {
  xSemaphoreGive(outputsMutex);
}

// --- submit ---------------------------------------------------------------------------
static Submit submit(ActionType type, int a, int b, uint32_t* idOut) {
  uint32_t id = recordNew();
  Job job{id, type, a, b};
  if (xQueueSend(queue, &job, 0) != pdTRUE) {
    setState(id, JobState::Done, false, "busy");
    return Submit::Busy;
  }
  if (idOut) *idOut = id;
  return Submit::Ok;
}

static bool validSteps(int n, int maxSteps) {
  return n >= 1 && n <= maxSteps;
}

Submit actionTempStep(bool up, int n, uint32_t* id) {
  if (!expanderOk) return Submit::NoExpander;
  if (!validSteps(n, STEPS_MAX)) return Submit::BadArgs;
  return submit(ActionType::TempStep, up ? 1 : 0, n, id);
}

Submit actionSetTemperature(int targetF, uint32_t* id) {
  if (!expanderOk) return Submit::NoExpander;
  if (targetF < cfg.tempMinF || targetF > cfg.tempMaxF) return Submit::BadArgs;
  return submit(ActionType::SetTemperature, targetF, 0, id);
}

Submit actionSetHeater(Seat seat, int level, uint32_t* id) {
  if (!expanderOk) return Submit::NoExpander;
  if (level < 0 || level > cfg.seatHeaterStages) return Submit::BadArgs;
  return submit(ActionType::SetHeater, seat == SEAT_DRIVER ? 0 : 1, level, id);
}

Submit actionSetDefrost(DefrostZone zone, bool on, uint32_t* id) {
  if (!expanderOk) return Submit::NoExpander;
  return submit(ActionType::SetDefrost, zone == DEFROST_FRONT ? 0 : 1, on ? 1 : 0, id);
}

Submit actionFanStep(bool up, int n, uint32_t* id) {
  if (!expanderOk) return Submit::NoExpander;
  if (!validSteps(n, FAN_STEPS_MAX)) return Submit::BadArgs;
  return submit(ActionType::FanStep, up ? 1 : 0, n, id);
}

Submit actionPower(uint32_t* id) {
  if (!expanderOk) return Submit::NoExpander;
  return submit(ActionType::Power, 0, 0, id);
}

Submit actionRemoteStart(uint32_t* id) {
  return submit(ActionType::RemoteStart, 0, 0, id);
}

const char* submitText(Submit s) {
  switch (s) {
    case Submit::Ok:         return "queued";
    case Submit::BadArgs:    return "invalid arguments";
    case Submit::NoExpander: return "Expander is missing. Refused";
    case Submit::Busy:       return "busy, try again";
  }
  return "unknown";
}
