#pragma once

#include <Arduino.h>

// Web server lifecycle
void webConfigStart();
void webConfigStop();
void webConfigLoop();

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

// Diagnostic tools
void webSystemInfo();
void webPSRAMTest();
void webSDBenchmark();
