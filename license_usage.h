#pragma once

#include <Arduino.h>
#include <stdint.h>

// Persistent usage accounting for the commercial license/trial layer.
//
// IMPORTANT:
// - This module only measures and persists usage. It does NOT currently block,
//   limit or otherwise change SensorForge product behavior.
// - Counters live in internal NVS, not on the SD card.
// - Segment rotation is intentionally not counted as a new recording event.
// - The persistent format is redundant + CRC protected for corruption
//   detection. This is robustness, not cryptographic anti-tamper protection.
// - No NVS write is performed on recording start or in the per-frame path.
// - Total system uptime is intentionally approximate. Runtime is accumulated in
//   RTC-retained RAM and only flushed to NVS opportunistically / every several
//   hours, keeping flash wear negligible. A hard power loss can therefore lose
//   a small amount of uptime, which is acceptable for the usage display.

struct LicenseUsageSnapshot {
    // Durably committed filming time.
    uint64_t recordedSecondsTotal;

    // Committed filming time plus the currently running, not-yet-checkpointed
    // part of an active recording. Use this for live UI display.
    uint64_t recordedSecondsEffectiveTotal;

    // Total time with at least one active RTSP/HTTP-MJPEG stream client.
    // Multiple simultaneous viewers count only once (wall-clock streaming time).
    uint64_t streamedSecondsTotal;

    // Live effective streaming total including not-yet-persisted RTC carry.
    uint64_t streamedSecondsEffectiveTotal;

    // Approximate accumulated awake/system runtime across deep-sleep cycles.
    uint64_t systemUptimeSecondsTotal;

    uint32_t recordingsTotal;
    uint32_t interruptedRecordingsTotal;
    int64_t lastTrustedEpoch;
    bool recordingActive;
    bool streamingActive;
    bool storageHealthy;
    bool integrityError;
};

// Load the persistent usage state from NVS. Safe to call once during boot after
// licenseBegin(). A recording that was durably checkpointed as active on the
// previous boot is recorded as interrupted and closed without inventing time.
void licenseUsageBegin();

// Cheap periodic bookkeeping. This only updates RTC-retained RAM during normal
// operation and writes NVS at most after a long accumulated uptime interval.
// Safe to call from every loop iteration.
void licenseUsagePeriodic();

// Observe the current system clock as a trusted time anchor. The caller must
// invoke this only after a trusted RTC restore or successful NTP synchronization.
// Values older than 2021-01-01 UTC are ignored. The stored value is monotonic:
// it can advance but never move backwards. To avoid NVS wear across frequent
// deep-sleep wakes, at most one new anchor is persisted per UTC day.
bool licenseUsageObserveTrustedTimeNow();

// Recording lifecycle hooks. Started is RAM-only and must be called only after
// recorderStart() succeeded. Checkpoint is for an already-safe container
// boundary (for example after recorderEnd() during segment rotation). Stopped
// commits the final remainder and closes the event.
void licenseUsageRecordingStarted();
bool licenseUsageRecordingCheckpoint();
bool licenseUsageRecordingStopped();

// Stream accounting is wall-clock based: active=true while at least one real
// RTSP or HTTP-MJPEG client is consuming the live stream. Repeated calls with
// the same state are cheap. Time is accumulated in RTC-retained RAM and only
// flushed to NVS in coarse intervals or together with another usage write.
void licenseUsageStreamingSetActive(bool active);

LicenseUsageSnapshot licenseUsageSnapshot();
String licenseUsageDiagnosticSummary();
