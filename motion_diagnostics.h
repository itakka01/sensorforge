#pragma once

#include <Arduino.h>

// RAM-only motion diagnostics for WebConfig. No values are persisted and no
// per-edge diagnostic writes are added to the SD/logging path.
struct MotionDiagnosticsSnapshot {
    // Raw GPIO level. presenceDebouncedActive is retained for compatibility
    // with the existing status endpoint and intentionally mirrors the raw level:
    // diagnostics must not filter a real PIR/OT2 HIGH away from recording.
    bool presenceActive;
    bool presenceDebouncedActive;
    bool radarMotionActive;

    // Sampled GPIO transition diagnostics. A LOW interval must remain stable
    // briefly before another rising event is counted as a new trigger; short
    // chatter is suppressed from creating duplicate queued events.
    uint32_t presenceTriggerCount;
    uint32_t presenceRawRiseCount;
    uint32_t presenceRejectedGlitchCount;
    bool presenceLastTriggerValid;
    uint32_t presenceLastTriggerAgeMs;

    uint32_t radarTriggerCount;
    bool radarLastTriggerValid;
    uint32_t radarLastTriggerAgeMs;
};

// Call once after the boot-time motion sensor detection/startup guard. A real
// presence wake can be supplied so the wake event is retained even if the
// digital level changes before WebConfig is opened.
void motionDiagnosticsBegin(bool presenceWakeAtBoot);

// Lightweight level sampling for diagnostics plus a one-shot backup trigger.
// A sampled HIGH is accepted immediately; only the LOW re-arm state is
// debounced to prevent duplicate triggers from chatter. No GPIO ISR is used.
void motionDiagnosticsLoop();

// Historic names retained for the existing light-sleep RTC ownership handoff.
// They suspend/resume polling; they do not attach a GPIO interrupt.
void motionDiagnosticsSuspendPresenceInterrupt();
void motionDiagnosticsResumePresenceInterrupt();

// Freenove offline-awake workaround: GPIO21 is RTC-capable and the same
// RTC input path is already proven by EXT1 light-sleep wake. When enabled,
// presence sampling uses rtc_gpio_get_level() while the ESP32 remains awake.
// Returns true if the requested backend was configured successfully.
bool motionDiagnosticsSetPresenceRtcSampling(bool enabled);
bool motionDiagnosticsPresenceRtcSamplingActive();

// Light sleep does not reboot. Record an EXT1 presence wake immediately.
void motionDiagnosticsNotePresenceTrigger();

// Authoritative awake presence level. This deliberately returns the raw GPIO
// HIGH/LOW state so diagnostics cannot suppress an actual PIR/OT2 signal.
bool motionDiagnosticsPresenceActive();

// Return true for a short time after the most recent accepted presence event.
bool motionDiagnosticsPresenceTriggeredRecently(
    uint32_t maxAgeMs
);

// Backup one-shot start event. Raw GPIO HIGH remains the primary trigger; this
// latch preserves an observed event if its level returns LOW before the next
// recording decision.
bool motionDiagnosticsPresenceStartPending(
    uint32_t maxAgeMs
);

// Consume the backup start event after a recording starts or is deliberately
// rejected. Diagnostic counters/timestamps remain intact.
void motionDiagnosticsConsumePresenceStartTrigger();

// Obtain a coherent RAM snapshot for the WebConfig live-motion endpoint.
void motionDiagnosticsGetSnapshot(MotionDiagnosticsSnapshot &snapshot);
