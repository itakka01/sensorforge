#pragma once

#include <Arduino.h>

// Camera idle standby (Drone Mode). The sensor's software power down
// (register 0x3008 bit 6, OV3660/OV5640) stops frame output after
// kCameraIdleStandbyMs without use: less current, less heat and no frame
// traffic. Every camera user wakes it first with cameraWakeForUse(); a planned
// cluster shot does so 2 s ahead (clusterCapturePrepareCamera). Implemented in
// sensorforge.ino (owns the camera); loopTask only.
constexpr uint32_t kCameraIdleStandbyMs = 30000UL;

// Wakes the camera if it is in standby and marks it as used now (restarts the
// idle timer). True when the camera is awake (or off); false if waking failed.
bool cameraWakeForUse();

// Called from loop(): enters standby after the idle time in Drone Mode, and
// wakes the camera again when Drone Mode ends or another owner appears.
void cameraIdleStandbyService();

// True while the camera is in idle standby (not the light-sleep standby).
bool cameraIdleStandbyActive();
