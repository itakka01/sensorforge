#pragma once

#include <Arduino.h>
#include <stdint.h>

// Product-policy layer kept deliberately separate from cryptographic license
// validation. V1 does not yet restrict unlicensed devices; the values below
// provide the stable integration point for later commercial limits.
struct LicensePolicy {
    bool recordingAllowed;
    uint32_t maxRecordingSeconds;   // 0 = unlimited
    uint32_t maxRecordingsPerDay;   // 0 = unlimited
    bool syncApiAllowed;
    bool radarConfigurationAllowed;
    bool advancedAnalyticsAllowed;
    bool watermarkRequired;
};

LicensePolicy licensePolicy();
