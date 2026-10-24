#pragma once
#include <stdint.h>

// Optional GPIO marker inside the REAL cluster capture path (SFJC1 option
// "tp1"). Qualified only for XIAO D1/GPIO2; no timer, no separate schedule.
// Prepare ~2 s before the deadline (pin -> output LOW), Rise directly before the
// capturing esp_camera_fb_get(), Fall directly after it returns, Release after
// the shot (output LOW -> input without pulls, so an external pull-down keeps
// the line low). Rise/Fall are single register writes from IRAM.
bool clusterCapturePulsePrepare(const char *&reason);
void clusterCapturePulseRise();
void clusterCapturePulseFall();
void clusterCapturePulseRelease();
bool clusterCapturePulsePrepared();
bool clusterCapturePulseEmitted();  // rising edge of the current/last shot was driven
