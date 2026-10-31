#pragma once
#include <ArduinoJson.h>
#include <esp_http_server.h>
#include "auth.h"

// Helpers shared by api.cpp (control endpoints) and admin_api.cpp (admin page, settings, setup, OTA).
// Not part of the public API of the module: include only from those two files.

constexpr size_t MAX_BODY = 512;

struct Call {
  AuthOutcome who;
  char body[MAX_BODY + 1];
  size_t bodyLen = 0;
  char path[96];
};

esp_err_t reply(httpd_req_t* req, int code, const JsonDocument& doc);
esp_err_t replyError(httpd_req_t* req, int code, const char* error);
bool arrivedOnTunnel(httpd_req_t* req);   // false (and the connection is closed) when it did not arrive on the tunnel
uint32_t clientIp(httpd_req_t* req);

// Reads the request body into call.body (bounded by MAX_BODY) and copies the path. Answers 4xx itself and
// returns false on failure. Does not check any signature.
bool readBody(httpd_req_t* req, Call& call);

// readBody() + signature check for `needed`. Answers 401/403/429 itself and returns false on failure.
bool authenticate(httpd_req_t* req, Role needed, Call& call);
bool parseBody(const Call& call, JsonDocument& doc);

void apiAddHandler(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*));

// admin_api.cpp
void adminRegister();   // registers its handlers on the running server
void adminPoll();       // called from apiPoll(): performs a requested reboot
