#pragma once
#include <Arduino.h>
#include <stddef.h>
#include "firmware_security.h"

// Final package checks, common to manual uploads and future remote downloads.
// Must run BEFORE esp_ota_end() and boot-partition selection.
// reportedTransportBytes may be 0 if the transport cannot supply a total size.
// Hash context is finalized only after structural and board checks pass.
bool firmwareOtaVerifyCompletePackage(
    const SensorForgeFirmwarePackageInfo &package,
    SensorForgeFirmwareHashContext &hash,
    bool boardMarkerFound,
    size_t payloadBytes,
    size_t packageBytes,
    size_t reportedTransportBytes,
    String &error);
