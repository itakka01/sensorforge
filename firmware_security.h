#pragma once

#include <Arduino.h>
#include <FS.h>
#include <psa/crypto.h>

#include "firmware_package_format.h"

// Signed package format 2 (firmware_package_format.h): 256-byte header.
static const size_t SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE = FirmwarePackage::kHeaderSize;
static const size_t SENSORFORGE_FIRMWARE_SIGNATURE_MAX_BYTES = FirmwarePackage::kSignatureMax;

struct SensorForgeFirmwarePackageInfo
{
    uint32_t firmwareSize = 0;
    FirmwarePackage::Header header;   // parsed, not yet trusted
    // Set by the caller before the final verification: accept a release older
    // than the running one (explicit operator choice only).
    bool allowOlder = false;
};

struct SensorForgeFirmwareHashContext
{
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool active = false;
};

// "SENSORFORGE|RELEASE=<tag>|END" (read by the signing tool).
const char *firmwareSecurityReleaseMarker();

// Board id of this build ("xiao", "freenove", "diymore").
const char *firmwareSecurityLocalBoardId();

// Early rejection (wrong board, or older without allowOlder) right after the
// header was parsed, before anything is written. Not trusted yet; the final
// verification repeats it after the signature check.
bool firmwareSecurityCheckPolicy(
    const SensorForgeFirmwarePackageInfo &info,
    String &error
);

bool firmwareSecurityParseHeader(
    const uint8_t *header,
    size_t headerLength,
    SensorForgeFirmwarePackageInfo &info,
    String &error
);

bool firmwareSecurityReadHeader(
    File &packageFile,
    SensorForgeFirmwarePackageInfo &info,
    String &error
);

bool firmwareSecurityHashBegin(
    SensorForgeFirmwareHashContext &context,
    String &error
);

bool firmwareSecurityHashUpdate(
    SensorForgeFirmwareHashContext &context,
    const uint8_t *data,
    size_t length,
    String &error
);

// Image SHA-256 == signed header digest, ECDSA signature over the signed
// header block, then board/version policy (firmwareSecurityCheckPolicy).
// Every install path must pass this before an image can become bootable.
bool firmwareSecurityHashFinishAndVerify(
    SensorForgeFirmwareHashContext &context,
    const SensorForgeFirmwarePackageInfo &info,
    String &error
);

void firmwareSecurityHashAbort(
    SensorForgeFirmwareHashContext &context
);

bool firmwareSecurityVerifyPackageFile(
    File &packageFile,
    SensorForgeFirmwarePackageInfo &info,
    String &error
);
