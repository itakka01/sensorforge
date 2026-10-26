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

// Frame-clock sync for cluster shots: the sensor's frame timing is free
// running, so "the next frame" after a µs-exact trigger starts anywhere within
// one frame period (OV5640 ~52 ms). Holding the sensor in software power down
// until the deadline and releasing it there restarts every sensor's frame
// clock at the same moment; the first frame after the release is captured.
// Hold: true when the sensor is (now) held (OV3660/OV5640 only).
bool cameraFrameSyncHold();
// Release at the deadline (one SCCB write); true when it was held and is running.
bool cameraFrameSyncRelease();
bool cameraFrameSyncHeld();
// Restores the JPEG quality lowered for the restart frame (no-op otherwise).
// Called by the shot once the restart frame has started, and by every
// cameraWakeForUse(). False if the sensor refused the write.
bool cameraFrameSyncRestoreQuality();
