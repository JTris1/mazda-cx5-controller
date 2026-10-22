#pragma once
#include <Arduino.h>

// Cellular data over the SIM7670G modem, as a PPP network interface. This is the device's only
// network connection. cellBegin() starts a background task that powers the modem, registers on
// the network and brings PPP up, and keeps retrying (with a growing delay) if anything fails.
void cellBegin();

// True once PPP has an IP address.
bool cellConnected();

// One-shot report used by the "net" console command.
void cellPrintStatus();

// The lwIP interface of the PPP link (what WireGuard sends its packets through), or nullptr.
struct netif* cellLwipNetif();
