#pragma once

#include <Arduino.h>

// RAM-only motion diagnostics for WebConfig. No values are persisted and no
// diagnostic trigger writes are added to the SD/logging path.
struct MotionDiagnosticsSnapshot {
    bool presenceActive;
    bool radarMotionActive;

    uint32_t presenceTriggerCount;
    bool presenceLastTriggerValid;
    uint32_t presenceLastTriggerAgeMs;

    uint32_t radarTriggerCount;
    bool radarLastTriggerValid;
    uint32_t radarLastTriggerAgeMs;
};

// Call once after the boot-time motion sensor detection/startup guard. A real
// presence wake can be supplied so the wake event is retained even if the
// digital pulse ends before WebConfig is opened.
void motionDiagnosticsBegin(bool presenceWakeAtBoot);

// Lightweight edge tracking. Call once per main-loop pass after radarLoop().
void motionDiagnosticsLoop();

// Light sleep does not reboot. Record an EXT1 presence wake immediately so a
// short PIR/OT2 pulse cannot be missed by later browser polling.
void motionDiagnosticsNotePresenceTrigger();

// Obtain a coherent RAM snapshot for the WebConfig live-motion endpoint.
void motionDiagnosticsGetSnapshot(MotionDiagnosticsSnapshot &snapshot);
