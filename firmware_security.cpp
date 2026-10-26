#include "firmware_security.h"
#include "firmware_public_key.h"
#include "psram_alloc.h"
#include "board_config.h"
#include "sensorforge_version.h"

#include <mbedtls/pk.h>
#include <string.h>

namespace {

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

// Read by sensorforge_firmware_sign.py to put the release into the signed
// header; exactly one occurrence per image (also logged at boot, which keeps
// it in the binary).
static const char FIRMWARE_RELEASE_MARKER[] =
    "SENSORFORGE|RELEASE=" SENSORFORGE_RELEASE_TAG "|END";

const char *firmwareSecurityReleaseMarker()
{
    return FIRMWARE_RELEASE_MARKER;
}

const char *firmwareSecurityLocalBoardId()
{
#if defined(BOARD_XIAO)
    return "xiao";
#elif defined(BOARD_FREENOVE)
    return "freenove";
#elif defined(BOARD_DIYMORE_S3CAM)
    return "diymore";
#else
    return "unknown";
#endif
}

bool firmwareSecurityCheckPolicy(
    const SensorForgeFirmwarePackageInfo &info,
    String &error
)
{
    const char *policyError = nullptr;
    if (!FirmwarePackage::checkPolicy(
            info.header,
            firmwareSecurityLocalBoardId(),
            SENSORFORGE_RELEASE_TAG,
            info.allowOlder,
            policyError
        )) {
        error = String(policyError) + " (package " + info.header.board + " " +
            info.header.release + ", installed " + firmwareSecurityLocalBoardId() + " " +
            SENSORFORGE_RELEASE_TAG + ")";
        return false;
    }
    return true;
}

bool firmwareSecurityParseHeader(
    const uint8_t *header,
    size_t headerLength,
    SensorForgeFirmwarePackageInfo &info,
    String &error
)
{
    error = "";
    info = SensorForgeFirmwarePackageInfo();
    const char *parseError = nullptr;
    if (!FirmwarePackage::parseHeader(header, headerLength, info.header, parseError)) {
        error = parseError ? parseError : "invalid firmware package header";
        return false;
    }
    info.firmwareSize = info.header.imageSize;
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
            info.header.imageDigest,
            sizeof(digest)
        ) != 0
    ) {
        error = "firmware SHA-256 does not match signed package";
        return false;
    }

    // The signature covers the header block (image digest, board, release).
    uint8_t blockDigest[32] = {};
    size_t blockDigestLength = 0;
    if (
        psa_hash_compute(
            PSA_ALG_SHA_256,
            info.header.signedBlock,
            sizeof(info.header.signedBlock),
            blockDigest,
            sizeof(blockDigest),
            &blockDigestLength
        ) != PSA_SUCCESS ||
        blockDigestLength != sizeof(blockDigest)
    ) {
        error = "SHA-256 of the signed header failed";
        return false;
    }

    if (!firmwareSecurityVerifySignature(
            blockDigest,
            info.header.signature,
            info.header.signatureLength,
            error
        )) {
        return false;
    }

    // Only now are board and release trusted.
    return firmwareSecurityCheckPolicy(info, error);
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

    // Temporary PSRAM buffer (was a 4 KB static in internal RAM).
    SfPsramBuffer hashBuffer(4096);

    if (!hashBuffer) {
        firmwareSecurityHashAbort(hashContext);
        error = "no PSRAM buffer for firmware verification";
        return false;
    }

    uint8_t *buffer = hashBuffer.data();
    const size_t bufferSize = hashBuffer.size();
    uint32_t remaining =
        info.firmwareSize;

    while (remaining > 0) {
        size_t requested =
            remaining < bufferSize
            ? remaining
            : bufferSize;

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
