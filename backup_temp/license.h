#pragma once

#include <Arduino.h>
#include <stdint.h>

// SensorForge offline, board-bound license status.
// Internal identifiers deliberately remain English and stable.
enum LicenseStatus : uint8_t {
    LICENSE_STATUS_MISSING = 0,
    LICENSE_STATUS_VALID,
    LICENSE_STATUS_INVALID_FORMAT,
    LICENSE_STATUS_INVALID_SIGNATURE,
    LICENSE_STATUS_WRONG_DEVICE,
    LICENSE_STATUS_WRONG_PRODUCT,
    LICENSE_STATUS_UNSUPPORTED_VERSION,
    LICENSE_STATUS_EXPIRED,
    LICENSE_STATUS_TIME_UNAVAILABLE,
    LICENSE_STATUS_STORAGE_ERROR
};

enum LicenseEdition : uint8_t {
    LICENSE_EDITION_UNLICENSED = 0,
    LICENSE_EDITION_FULL = 1,
    LICENSE_EDITION_EVALUATION = 2,
    LICENSE_EDITION_SERVICE = 3
};

static constexpr uint32_t LICENSE_FEATURE_RECORDING = 1UL << 0;
static constexpr uint32_t LICENSE_FEATURE_RADAR = 1UL << 1;
static constexpr uint32_t LICENSE_FEATURE_SYNC_API = 1UL << 2;
static constexpr uint32_t LICENSE_FEATURE_ADVANCED_ANALYTICS = 1UL << 3;

// Initialize the hardware identity and load the internally stored activation
// code from /license.dat in LittleFS. LittleFS must already be mounted; call
// after loadConfig().
void licenseBegin();

// Re-read and validate the installed license from internal LittleFS.
bool licenseReload();

LicenseStatus licenseStatus();
const char *licenseStatusName(LicenseStatus status);
const char *licenseStatusName();
bool licenseIsValid();

LicenseEdition licenseEdition();
const char *licenseEditionName();
uint32_t licenseFeatureFlags();
bool licenseFeatureEnabled(uint32_t featureMask);

// Public, product-specific identifier derived from the factory eFuse MAC.
// This is the identifier customers provide when a board-bound license is issued.
String licenseHardwareId();

String licenseId();
String licenseIssuedDateText();
String licenseExpiryDateText();

// Validate an SF1 activation code and atomically store it in internal LittleFS.
// Only a code that is cryptographically valid for this board is accepted.
// No reboot is required.
bool licenseInstallCode(
    const String &activationCode,
    String &error,
    LicenseStatus *resultStatus = nullptr
);

// Remove the installed license from internal LittleFS and return to MISSING.
bool licenseRemove(String &error);
