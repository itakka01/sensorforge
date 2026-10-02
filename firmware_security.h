#pragma once

#include <Arduino.h>
#include <FS.h>
#include <psa/crypto.h>

static const size_t SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE = 128U;
static const size_t SENSORFORGE_FIRMWARE_SIGNATURE_MAX_BYTES = 72U;

struct SensorForgeFirmwarePackageInfo
{
    uint32_t firmwareSize = 0;
    uint8_t expectedDigest[32] = {};
    uint8_t signature[SENSORFORGE_FIRMWARE_SIGNATURE_MAX_BYTES] = {};
    size_t signatureLength = 0;
};

struct SensorForgeFirmwareHashContext
{
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool active = false;
};

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
