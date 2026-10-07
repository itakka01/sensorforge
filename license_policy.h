#pragma once

#include <Arduino.h>
#include <stdint.h>

// Effective commercial/product mode. This is deliberately separate from the
// cryptographic LicenseEdition in license.h: a device without an installed key
// starts in the local TRIAL, then falls back to DEMO when that trial is used.
enum LicenseProductMode : uint8_t {
    LICENSE_PRODUCT_MODE_TRIAL = 0,
    LICENSE_PRODUCT_MODE_DEMO,
    LICENSE_PRODUCT_MODE_FULL,
    LICENSE_PRODUCT_MODE_SERVICE
};

struct LicensePolicy {
    LicenseProductMode mode;

    // Recording gate. The daily count is a start limit; once the 10th DEMO
    // event has started it may continue until the daily time allowance ends.
    bool recordingAllowed;
    uint32_t maxRecordingSeconds;         // 0 = no per-event license limit
    uint32_t maxRecordingsPerDay;         // 0 = unlimited
    uint32_t maxRecordingSecondsPerDay;   // 0 = unlimited
    uint32_t recordingSecondsRemainingToday;
    uint32_t recordingsRemainingToday;

    bool streamingAllowed;
    uint32_t maxStreamingSecondsPerDay;   // 0 = unlimited
    uint32_t streamingSecondsRemainingToday;

    bool syncApiAllowed;
    bool radarConfigurationAllowed;
    bool advancedAnalyticsAllowed;
    bool watermarkRequired;
};

LicenseProductMode licenseProductMode();
const char *licenseProductModeName();

LicensePolicy licensePolicy();

// Convenience helpers for hot product paths. They perform no flash writes
// except the once-per-day DEMO-window rollover in the policy layer.
bool licensePolicyRecordingStartAllowed();
bool licensePolicyRecordingContinueAllowed();
bool licensePolicyStreamingAllowed();
