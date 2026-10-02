#include "firmware_security.h"
#include "firmware_public_key.h"

#include <mbedtls/pk.h>
#include <string.h>

namespace {

static const uint8_t FIRMWARE_PACKAGE_MAGIC[8] = {
    'S', 'F', 'W', 'P', 'K', 'G', '1', 0
};

static const uint8_t FIRMWARE_PACKAGE_VERSION = 1U;
static const uint8_t FIRMWARE_SIGNATURE_ALGORITHM_ECDSA_P256_SHA256 = 1U;

static uint16_t readU16LE(const uint8_t *value)
{
    return
        (uint16_t)value[0] |
        ((uint16_t)value[1] << 8);
}

static uint32_t readU32LE(const uint8_t *value)
{
    return
        (uint32_t)value[0] |
        ((uint32_t)value[1] << 8) |
        ((uint32_t)value[2] << 16) |
        ((uint32_t)value[3] << 24);
}

static bool firmwareSecurityVerifySignature(
    const uint8_t digest[32],
    const uint8_t *signature,
    size_t signatureLength,
    String &error
)
{
    if (
        !digest ||
        !signature ||
        signatureLength == 0 ||
        signatureLength > SENSORFORGE_FIRMWARE_SIGNATURE_MAX_BYTES
    ) {
        error = "firmware signature metadata is invalid";
        return false;
    }

    mbedtls_pk_context publicKey;
    mbedtls_pk_init(&publicKey);

    int parseResult =
        mbedtls_pk_parse_public_key(
            &publicKey,
            (const unsigned char *)SENSORFORGE_FIRMWARE_PUBLIC_KEY_PEM,
            strlen(SENSORFORGE_FIRMWARE_PUBLIC_KEY_PEM) + 1
        );

    if (parseResult != 0) {
        mbedtls_pk_free(&publicKey);
        error = "firmware public key could not be loaded";
        return false;
    }

    int verifyResult =
        mbedtls_pk_verify(
            &publicKey,
            MBEDTLS_MD_SHA256,
            digest,
            32,
            signature,
            signatureLength
        );

    mbedtls_pk_free(&publicKey);

    if (verifyResult != 0) {
        error = "firmware signature is invalid";
        return false;
    }

    return true;
}

} // namespace

bool firmwareSecurityParseHeader(
    const uint8_t *header,
    size_t headerLength,
    SensorForgeFirmwarePackageInfo &info,
    String &error
)
{
    error = "";
    info = SensorForgeFirmwarePackageInfo();

    if (
        !header ||
        headerLength < SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE
    ) {
        error = "firmware package header is incomplete";
        return false;
    }

    if (
        memcmp(
            header,
            FIRMWARE_PACKAGE_MAGIC,
            sizeof(FIRMWARE_PACKAGE_MAGIC)
        ) != 0
    ) {
        error = "not a SensorForge signed firmware package";
        return false;
    }

    if (header[8] != FIRMWARE_PACKAGE_VERSION) {
        error = "unsupported firmware package version";
        return false;
    }

    if (
        header[9] !=
        FIRMWARE_SIGNATURE_ALGORITHM_ECDSA_P256_SHA256
    ) {
        error = "unsupported firmware signature algorithm";
        return false;
    }

    if (
        readU16LE(header + 10) !=
        SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE
    ) {
        error = "invalid firmware package header size";
        return false;
    }

    info.firmwareSize =
        readU32LE(header + 12);

    if (info.firmwareSize == 0) {
        error = "firmware package contains an empty image";
        return false;
    }

    memcpy(
        info.expectedDigest,
        header + 16,
        sizeof(info.expectedDigest)
    );

    info.signatureLength =
        readU16LE(header + 48);

    if (
        info.signatureLength == 0 ||
        info.signatureLength > SENSORFORGE_FIRMWARE_SIGNATURE_MAX_BYTES
    ) {
        error = "firmware package signature length is invalid";
        return false;
    }

    memcpy(
        info.signature,
        header + 50,
        info.signatureLength
    );

    return true;
}

bool firmwareSecurityReadHeader(
    File &packageFile,
    SensorForgeFirmwarePackageInfo &info,
    String &error
)
{
    error = "";

    if (!packageFile) {
        error = "firmware package is not open";
        return false;
    }

    if (!packageFile.seek(0)) {
        error = "cannot seek firmware package";
        return false;
    }

    uint8_t header[SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE] = {};

    size_t got =
        packageFile.read(
            header,
            sizeof(header)
        );

    if (got != sizeof(header)) {
        error = "firmware package header is incomplete";
        return false;
    }

    return
        firmwareSecurityParseHeader(
            header,
            sizeof(header),
            info,
            error
        );
}

bool firmwareSecurityHashBegin(
    SensorForgeFirmwareHashContext &context,
    String &error
)
{
    error = "";
    firmwareSecurityHashAbort(context);

    if (psa_crypto_init() != PSA_SUCCESS) {
        error = "crypto subsystem initialization failed";
        return false;
    }

    context.operation = PSA_HASH_OPERATION_INIT;

    psa_status_t result =
        psa_hash_setup(
            &context.operation,
            PSA_ALG_SHA_256
        );

    if (result != PSA_SUCCESS) {
        context.operation = PSA_HASH_OPERATION_INIT;
        error = "SHA-256 initialization failed";
        return false;
    }

    context.active = true;
    return true;
}

bool firmwareSecurityHashUpdate(
    SensorForgeFirmwareHashContext &context,
    const uint8_t *data,
    size_t length,
    String &error
)
{
    error = "";

    if (!context.active) {
        error = "firmware hash is not active";
        return false;
    }

    if (!data && length != 0) {
        error = "firmware hash input is invalid";
        return false;
    }

    if (length == 0)
        return true;

    psa_status_t result =
        psa_hash_update(
            &context.operation,
            data,
            length
        );

    if (result != PSA_SUCCESS) {
        firmwareSecurityHashAbort(context);
        error = "SHA-256 update failed";
        return false;
    }

    return true;
}

bool firmwareSecurityHashFinishAndVerify(
    SensorForgeFirmwareHashContext &context,
    const SensorForgeFirmwarePackageInfo &info,
    String &error
)
{
    error = "";

    if (!context.active) {
        error = "firmware hash is not active";
        return false;
    }

    uint8_t digest[32] = {};
    size_t digestLength = 0;

    psa_status_t result =
        psa_hash_finish(
            &context.operation,
            digest,
            sizeof(digest),
            &digestLength
        );

    if (result != PSA_SUCCESS) {
        psa_hash_abort(
            &context.operation
        );
    }

    context.active = false;
    context.operation = PSA_HASH_OPERATION_INIT;

    if (
        result != PSA_SUCCESS ||
        digestLength != sizeof(digest)
    ) {
        error = "SHA-256 finalization failed";
        return false;
    }

    if (
        memcmp(
            digest,
            info.expectedDigest,
            sizeof(digest)
        ) != 0
    ) {
        error = "firmware SHA-256 does not match signed package";
        return false;
    }

    return
        firmwareSecurityVerifySignature(
            digest,
            info.signature,
            info.signatureLength,
            error
        );
}

void firmwareSecurityHashAbort(
    SensorForgeFirmwareHashContext &context
)
{
    if (context.active) {
        psa_hash_abort(
            &context.operation
        );
    }

    context.active = false;
    context.operation = PSA_HASH_OPERATION_INIT;
}

bool firmwareSecurityVerifyPackageFile(
    File &packageFile,
    SensorForgeFirmwarePackageInfo &info,
    String &error
)
{
    error = "";

    if (!firmwareSecurityReadHeader(
            packageFile,
            info,
            error
        )) {
        return false;
    }

    const uint64_t expectedPackageSize =
        (uint64_t)SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE +
        (uint64_t)info.firmwareSize;

    if ((uint64_t)packageFile.size() != expectedPackageSize) {
        error = "firmware package size does not match its signed header";
        return false;
    }

    if (!packageFile.seek(
            SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE
        )) {
        error = "cannot seek to firmware image payload";
        return false;
    }

    SensorForgeFirmwareHashContext hashContext;

    if (!firmwareSecurityHashBegin(
            hashContext,
            error
        )) {
        return false;
    }

    static uint8_t buffer[4096];
    uint32_t remaining =
        info.firmwareSize;

    while (remaining > 0) {
        size_t requested =
            remaining < sizeof(buffer)
            ? remaining
            : sizeof(buffer);

        size_t got =
            packageFile.read(
                buffer,
                requested
            );

        if (got != requested) {
            firmwareSecurityHashAbort(hashContext);
            error = "firmware package ended unexpectedly";
            return false;
        }

        if (!firmwareSecurityHashUpdate(
                hashContext,
                buffer,
                got,
                error
            )) {
            return false;
        }

        remaining -=
            (uint32_t)got;

        yield();
    }

    bool verified =
        firmwareSecurityHashFinishAndVerify(
            hashContext,
            info,
            error
        );

    packageFile.seek(0);
    return verified;
}
