#pragma once
#include <Arduino.h>

// Passive GPIO meter (role independent). Captures the physical rising edges of
// up to three external signals - e.g. the optional cluster capture test pulse of
// several nodes - and stores their differences measured with this device's own
// esp_timer clock. Uses neither cluster time nor ESP-NOW and sends nothing.
// Reuses the existing GPIO lease (Freenove pins 14/21/41, Drone Mode, explicit
// release in board_config.h). Never starts by itself.
namespace ClusterHwTimeMeter {
bool start(String &error);
void stop(const char *reason);
void service();  // call regularly from the main loop
bool active();
String statusJson();
String csv();
}
