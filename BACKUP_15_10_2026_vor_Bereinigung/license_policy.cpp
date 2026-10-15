#include "license_policy.h"

#include "license.h"
#include "license_trial.h"

namespace {

static uint32_t clampU64ToU32(
    uint64_t value
)
{
    return value > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)value;
}

static uint32_t remainingU32(
    uint64_t used,
    uint64_t limit
)
{
    if (used >= limit)
        return 0U;

    return clampU64ToU32(limit - used);
}

} // namespace

LicenseProductMode licenseProductMode()
{
    // A cryptographically valid purchased/service license always wins over the
    // built-in evaluation counters. A valid EVALUATION code can be used later
    // by the license server as an explicit trial extension/override.
    if (licenseIsValid()) {
        switch (licenseEdition()) {
            case LICENSE_EDITION_FULL:
                return LICENSE_PRODUCT_MODE_FULL;
            case LICENSE_EDITION_SERVICE:
                return LICENSE_PRODUCT_MODE_SERVICE;
            case LICENSE_EDITION_EVALUATION:
                return LICENSE_PRODUCT_MODE_TRIAL;
            case LICENSE_EDITION_UNLICENSED:
            default:
                break;
        }
    }

    const LicenseTrialSnapshot trial =
        licenseTrialSnapshot();

    return trial.expired
        ? LICENSE_PRODUCT_MODE_DEMO
        : LICENSE_PRODUCT_MODE_TRIAL;
}

const char *licenseProductModeName()
{
    switch (licenseProductMode()) {
        case LICENSE_PRODUCT_MODE_TRIAL:
            return "TRIAL";
        case LICENSE_PRODUCT_MODE_DEMO:
            return "DEMO";
        case LICENSE_PRODUCT_MODE_FULL:
            return "FULL";
        case LICENSE_PRODUCT_MODE_SERVICE:
            return "SERVICE";
        default:
            return "TRIAL";
    }
}

LicensePolicy licensePolicy()
{
    LicensePolicy policy = {};
    policy.mode = licenseProductMode();

    // Safety/recovery behavior is never disabled by licensing. The policy only
    // gates new commercial/product actions.
    policy.recordingAllowed = true;
    policy.streamingAllowed = true;
    policy.syncApiAllowed = true;
    policy.radarConfigurationAllowed = true;
    policy.advancedAnalyticsAllowed = true;
    policy.watermarkRequired = false;

    if (policy.mode != LICENSE_PRODUCT_MODE_DEMO)
        return policy;

    const LicenseDemoDaySnapshot demo =
        licenseTrialDemoDaySnapshot();

    // If the tiny demo-window state cannot be persisted, fail open. Storage
    // faults must not unexpectedly make a field unit unusable.
    if (!demo.initialized || !demo.storageHealthy)
        return policy;

    policy.maxRecordingsPerDay =
        SENSORFORGE_DEMO_RECORDINGS_PER_DAY;
    policy.maxRecordingSecondsPerDay =
        (uint32_t)SENSORFORGE_DEMO_RECORDING_SECONDS_PER_DAY;
    policy.maxStreamingSecondsPerDay =
        (uint32_t)SENSORFORGE_DEMO_STREAMING_SECONDS_PER_DAY;

    policy.recordingsRemainingToday =
        demo.recordingsUsed >= SENSORFORGE_DEMO_RECORDINGS_PER_DAY
        ? 0U
        : SENSORFORGE_DEMO_RECORDINGS_PER_DAY - demo.recordingsUsed;

    policy.recordingSecondsRemainingToday =
        remainingU32(
            demo.recordingSecondsUsed,
            SENSORFORGE_DEMO_RECORDING_SECONDS_PER_DAY
        );

    policy.streamingSecondsRemainingToday =
        remainingU32(
            demo.streamingSecondsUsed,
            SENSORFORGE_DEMO_STREAMING_SECONDS_PER_DAY
        );

    policy.recordingAllowed =
        policy.recordingsRemainingToday > 0U &&
        policy.recordingSecondsRemainingToday > 0U;

    policy.streamingAllowed =
        policy.streamingSecondsRemainingToday > 0U;

    // Radar configuration/calibration deliberately stays available in DEMO.
    // It is useful for evaluation and is not a meaningful commercial bypass.
    policy.radarConfigurationAllowed = true;

    // Keep the local management/API plane available for diagnostics and setup.
    // Broader feature-tiering can be added later without touching this core
    // trial transition.
    policy.syncApiAllowed = true;
    policy.advancedAnalyticsAllowed = true;

    return policy;
}

bool licensePolicyRecordingStartAllowed()
{
    return licensePolicy().recordingAllowed;
}

bool licensePolicyRecordingContinueAllowed()
{
    const LicensePolicy policy =
        licensePolicy();

    if (policy.mode != LICENSE_PRODUCT_MODE_DEMO)
        return true;

    // Daily event count is a start gate only. Do not stop the 10th event just
    // because recordingsRemainingToday became zero immediately after start.
    return policy.recordingSecondsRemainingToday > 0U;
}

bool licensePolicyStreamingAllowed()
{
    return licensePolicy().streamingAllowed;
}
