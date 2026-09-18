#include "license_policy.h"
#include "license.h"

LicensePolicy licensePolicy()
{
    LicensePolicy policy;

    // Initial rollout: licensing is observable and cryptographically enforced
    // only at the license-validation layer. Product restrictions are defined in
    // a later step, so both licensed and unlicensed units remain permissive now.
    policy.recordingAllowed = true;
    policy.maxRecordingSeconds = 0;
    policy.maxRecordingsPerDay = 0;
    policy.syncApiAllowed = true;
    policy.radarConfigurationAllowed = true;
    policy.advancedAnalyticsAllowed = true;
    policy.watermarkRequired = false;


    return policy;
}
