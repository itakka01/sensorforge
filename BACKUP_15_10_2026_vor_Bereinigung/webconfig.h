#pragma once

#include <Arduino.h>

// Web server lifecycle
void webConfigStart();
void webConfigStop();
void webConfigLoop();

// API/system-action hooks. These reuse the same delayed WebConfig lifecycle
// used by the browser UI so HTTP responses can complete before restart/power-off.
// The functions refuse unsafe transitions such as reboot/shutdown during an
// active recording and return a machine-readable reason in error.
bool webConfigScheduleReboot(uint32_t delayMs, String &error);
bool webConfigScheduleShutdown(uint32_t delayMs, String &error);

// Software PIR simulation
bool webConfigMotionActive();
uint32_t webConfigMotionRemainingMs();

// True while the browser is actively consuming the live camera preview.
// Motion-trigger decisions and NEW recording starts are suppressed while true.
bool webConfigCameraPreviewActive();

// Temporary operator pause for automatic motion-triggered recording.
// This state is RAM-only and automatically clears when WebConfig is no longer
// actively held open by a visible browser page.
bool webConfigRecordingPaused();

// True when the WebConfig server has seen no HTTP/browser activity
// for at least timeoutSec seconds.
// timeoutSec == 0 always returns false.
bool webConfigInactiveFor(unsigned long timeoutSec);

// Remaining seconds until the same inactivity timeout would currently fire.
// Returns -1 when the timeout is not applicable/known (WebConfig inactive,
// timeout disabled, maintenance hold active, etc.). This is read-only status
// information and does not alter or prolong the WiFi/WebConfig lifecycle.
int32_t webConfigInactivityRemainingSeconds(unsigned long timeoutSec);

// Diagnostic tools
void webSystemInfo();
void webPSRAMTest();
void webSDBenchmark();
