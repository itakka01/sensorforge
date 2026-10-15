#pragma once

#include <Arduino.h>
#include <stdint.h>

// Local SensorForge evaluation accounting.
//
// The factory/default state is TRIAL. No activation code is required to start
// using the complete product. The local trial ends when either the accumulated
// active system time, trusted calendar time, recording allowance or streaming
// allowance is exhausted. A valid external FULL/SERVICE/EVALUATION activation code is still
// handled by license.cpp and takes precedence in license_policy.cpp.
//
// This module deliberately stores only a few baselines/window markers. High-
// frequency usage is measured by license_usage.cpp; no per-frame writes occur.

static constexpr uint64_t SENSORFORGE_TRIAL_ACTIVE_SECONDS =
    14ULL * 24ULL * 60ULL * 60ULL;
static constexpr uint64_t SENSORFORGE_TRIAL_RECORDING_SECONDS =
    20ULL * 60ULL * 60ULL;
static constexpr uint64_t SENSORFORGE_TRIAL_STREAMING_SECONDS =
    100ULL * 60ULL * 60ULL;

static constexpr uint32_t SENSORFORGE_DEMO_RECORDINGS_PER_DAY = 10U;
static constexpr uint64_t SENSORFORGE_DEMO_RECORDING_SECONDS_PER_DAY =
    60ULL * 60ULL;
static constexpr uint64_t SENSORFORGE_DEMO_STREAMING_SECONDS_PER_DAY =
    60ULL * 60ULL;

struct LicenseTrialSnapshot {
    bool initialized;
    bool storageHealthy;
    bool integrityError;
    bool expired;
    bool trustedCalendarAvailable;

    uint64_t activeSecondsUsed;
    uint64_t mediaSecondsUsed;
    uint64_t recordingSecondsUsed;
    uint64_t streamingSecondsUsed;
    uint64_t activeSecondsRemaining;
    uint64_t recordingSecondsRemaining;
    uint64_t streamingSecondsRemaining;
    uint64_t trustedCalendarSecondsUsed;

    int64_t trialStartTrustedEpoch;
    int64_t lastTrustedEpoch;
};

struct LicenseDemoDaySnapshot {
    bool initialized;
    bool storageHealthy;
    uint32_t dayKey;
    uint32_t recordingsUsed;
    uint64_t recordingSecondsUsed;
    uint64_t streamingSecondsUsed;
};

// Initialize/persist the one-time local trial baselines. Existing Beta 25-27
// usage counters become the baseline, so upgrading development units starts a
// fresh trial instead of consuming historical test usage.
void licenseTrialBegin();

// Call only when the system clock is known to be trustworthy (RTC restore or
// successful NTP sync). The first such observation anchors the 14-day calendar
// trial. Later observations are already retained by license_usage.cpp.
bool licenseTrialObserveTrustedTimeNow();

LicenseTrialSnapshot licenseTrialSnapshot();

// Returns current DEMO-day consumption. A day is UTC calendar-day when a
// trusted clock exists; otherwise it is a coarse 24 h active-uptime bucket.
// Window rollover causes at most one tiny NVS write per day.
LicenseDemoDaySnapshot licenseTrialDemoDaySnapshot();

String licenseTrialDiagnosticSummary();
