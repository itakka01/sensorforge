#pragma once

#include <Arduino.h>
#include <esp_system.h>

// Crash-loop guard.
//
// Counts panic and watchdog resets that happen before a boot has run stable
// for kStableAfterMs. After kCrashThreshold such resets in a row the device
// starts in safe mode: WiFi and WebConfig only - no camera, recording, motion
// detection, radar or cluster - so it stays reachable and the fault can be
// inspected instead of looping endlessly.
//
// Safe mode persists across software restarts. It ends with a power cycle
// (power-on reset) or with the "Normal neu starten" button in WebConfig.
//
// Independent of the low-power boot-loop protection (bootloop_protection),
// which deliberately counts only power-on/brownout cold boots.
namespace CrashGuard {

constexpr uint8_t kCrashThreshold = 3;
constexpr uint32_t kStableAfterMs = 60000UL;

// Call once early in setup(), before optional subsystems start. NVS only.
void handleBoot(esp_reset_reason_t resetReason);

// True for the whole boot when the crash threshold was reached.
bool safeMode();

uint8_t crashCount();

// Reset reason of the last counted crash, e.g. "INT_WDT".
const char *lastCrashReasonName();

// Call from loop(): marks the boot stable after kStableAfterMs.
void loop();

// Clears the persistent state. The caller restarts the device afterwards.
void clearForNormalBoot();

// Warning box with the restart button for WebConfig pages; empty outside safe mode.
String bannerHtml();

}  // namespace CrashGuard
