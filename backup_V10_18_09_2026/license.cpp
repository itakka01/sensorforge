#include "license.h"
#include "license_public_key.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <psa/crypto.h>
#include <string.h>
#include <time.h>

namespace {

static const char *LICENSE_PATH = "/license.dat";
static const char *LICENSE_TMP_PATH = "/license.tmp";
static const char *LICENSE_BAK_PATH = "/license.bak";
static const size_t LICENSE_MAX_CODE_BYTES = 512U;
static const size_t LICENSE_PAYLOAD_BYTES = 52U;
static const uint8_t LICENSE_FORMAT_VERSION = 1U;
static const uint8_t LICENSE_PRODUCT_SENSORFORGE = 1U;

struct LicenseData {
    uint8_t formatVersion;
    uint8_t productId;
    LicenseEdition edition;
    uint32_t featureFlags;
    uint32_t issuedDays;
    uint32_t expiresDays;
    uint8_t hardwareId[16];
    uint8_t licenseId[16];
};

static LicenseStatus currentStatus = LICENSE_STATUS_MISSING;
static LicenseData currentLicense = {};
static uint8_t deviceHardwareId[16] = {};
static bool hardwareIdentityReady = false;
static String hardwareIdText;

static uint32_t readU32LE(const uint8_t *value)
{
    return
        (uint32_t)value[0] |
        ((uint32_t)value[1] << 8) |
        ((uint32_t)value[2] << 16) |
        ((uint32_t)value[3] << 24);
}

static bool computeSha256(
    const uint8_t *input,
    size_t inputLength,
    uint8_t output[32]
)
{
    if (
        (!input && inputLength != 0) ||
        !output
    ) {
        return false;
    }

    if (psa_crypto_init() != PSA_SUCCESS)
        return false;

    size_t outputLength = 0;

    psa_status_t result =
        psa_hash_compute(
            PSA_ALG_SHA_256,
            input,
            inputLength,
            output,
            32,
            &outputLength
        );

    return
        result == PSA_SUCCESS &&
        outputLength == 32;
}

static void clearLicenseData()
{
    memset(
        &currentLicense,
        0,
        sizeof(currentLicense)
    );

    currentLicense.edition =
        LICENSE_EDITION_UNLICENSED;
}

static String base32HardwareId(const uint8_t *bytes, size_t length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

    String encoded;
    encoded.reserve(26);

    uint32_t buffer = 0;
    uint8_t bits = 0;

    for (size_t i = 0; i < length; ++i) {
        buffer =
            (buffer << 8) |
            bytes[i];
        bits += 8;

        while (bits >= 5) {
            bits -= 5;
            encoded +=
                alphabet[
                    (buffer >> bits) & 0x1FU
                ];
        }
    }

    if (bits > 0) {
        encoded +=
            alphabet[
                (buffer << (5 - bits)) & 0x1FU
            ];
    }

    String formatted = "SF-";

    for (size_t i = 0; i < encoded.length(); ++i) {
        if (
            i > 0 &&
            (
                i == 5 ||
                i == 10 ||
                i == 15 ||
                i == 20
            )
        ) {
            formatted += '-';
        }

        formatted += encoded[i];
    }

    return formatted;
}

static bool buildHardwareIdentity()
{
    if (hardwareIdentityReady)
        return true;

    uint8_t factoryMac[6] = {};

    if (
        esp_efuse_mac_get_default(
            factoryMac
        ) != ESP_OK
    ) {
        return false;
    }

    static const char domain[] =
        "SENSORFORGE-HWID-V1";

    uint8_t input[sizeof(domain) - 1 + sizeof(factoryMac)];

    memcpy(
        input,
        domain,
        sizeof(domain) - 1
    );

    memcpy(
        input + sizeof(domain) - 1,
        factoryMac,
        sizeof(factoryMac)
    );

    uint8_t digest[32] = {};

    if (!computeSha256(
            input,
            sizeof(input),
            digest
        )) {
        return false;
    }

    memcpy(
        deviceHardwareId,
        digest,
        sizeof(deviceHardwareId)
    );

    hardwareIdText =
        base32HardwareId(
            deviceHardwareId,
            sizeof(deviceHardwareId)
        );

    hardwareIdentityReady = true;
    return true;
}

static bool decodeBase64Url(
    String text,
    uint8_t *output,
    size_t outputCapacity,
    size_t &outputLength
)
{
    outputLength = 0;

    text.trim();

    if (!text.length())
        return false;

    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];

        if (c == '-') {
            text.setCharAt(i, '+');
        } else if (c == '_') {
            text.setCharAt(i, '/');
        } else if (
            !((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9'))
        ) {
            return false;
        }
    }

    size_t remainder =
        text.length() % 4U;

    if (remainder == 1U)
        return false;

    while (text.length() % 4U)
        text += '=';

    int result =
        mbedtls_base64_decode(
            output,
            outputCapacity,
            &outputLength,
            (const unsigned char *)text.c_str(),
            text.length()
        );

    return result == 0;
}


static bool parseActivationCode(
    const String &input,
    String &payloadText,
    String &signatureText,
    String &error
)
{
    String code = input;
    code.trim();

    if (!code.startsWith("SF1:")) {
        error =
            "activation code must start with SF1:";
        return false;
    }

    String body =
        code.substring(4);

    int separator =
        body.indexOf('.');

    if (
        separator <= 0 ||
        separator >= (int)body.length() - 1 ||
        body.indexOf('.', separator + 1) >= 0
    ) {
        error =
            "activation code has invalid structure";
        return false;
    }

    payloadText =
        body.substring(0, separator);

    signatureText =
        body.substring(separator + 1);

    payloadText.trim();
    signatureText.trim();

    if (
        !payloadText.length() ||
        !signatureText.length()
    ) {
        error =
            "activation code is incomplete";
        return false;
    }

    return true;
}


static bool parsePayload(
    const uint8_t *payload,
    size_t payloadLength,
    LicenseData &data,
    LicenseStatus &status,
    String &error
)
{
    if (payloadLength != LICENSE_PAYLOAD_BYTES) {
        status =
            LICENSE_STATUS_INVALID_FORMAT;
        error =
            "unexpected license payload length";
        return false;
    }

    if (
        payload[0] != 'S' ||
        payload[1] != 'F' ||
        payload[2] != 'L' ||
        payload[3] != '1'
    ) {
        status =
            LICENSE_STATUS_INVALID_FORMAT;
        error =
            "invalid license payload magic";
        return false;
    }

    data.formatVersion =
        payload[4];

    if (
        data.formatVersion !=
        LICENSE_FORMAT_VERSION
    ) {
        status =
            LICENSE_STATUS_UNSUPPORTED_VERSION;
        error =
            "unsupported license format version";
        return false;
    }

    data.productId =
        payload[5];

    uint8_t editionValue =
        payload[6];

    if (
        editionValue != LICENSE_EDITION_FULL &&
        editionValue != LICENSE_EDITION_EVALUATION &&
        editionValue != LICENSE_EDITION_SERVICE
    ) {
        status =
            LICENSE_STATUS_INVALID_FORMAT;
        error =
            "unsupported license edition";
        return false;
    }

    if (payload[7] != 0) {
        status =
            LICENSE_STATUS_INVALID_FORMAT;
        error =
            "reserved license payload byte is not zero";
        return false;
    }

    data.edition =
        (LicenseEdition)editionValue;

    data.featureFlags =
        readU32LE(payload + 8);

    data.issuedDays =
        readU32LE(payload + 12);

    data.expiresDays =
        readU32LE(payload + 16);

    memcpy(
        data.hardwareId,
        payload + 20,
        sizeof(data.hardwareId)
    );

    memcpy(
        data.licenseId,
        payload + 36,
        sizeof(data.licenseId)
    );

    return true;
}

static bool verifySignature(
    const uint8_t *payload,
    size_t payloadLength,
    const uint8_t *signature,
    size_t signatureLength
)
{
    uint8_t digest[32] = {};

    if (!computeSha256(
            payload,
            payloadLength,
            digest
        )) {
        return false;
    }

    mbedtls_pk_context publicKey;
    mbedtls_pk_init(&publicKey);

    int parseResult =
        mbedtls_pk_parse_public_key(
            &publicKey,
            (const unsigned char *)
                SENSORFORGE_LICENSE_PUBLIC_KEY_PEM,
            strlen(
                SENSORFORGE_LICENSE_PUBLIC_KEY_PEM
            ) + 1
        );

    if (parseResult != 0) {
        mbedtls_pk_free(&publicKey);
        return false;
    }

    int verifyResult =
        mbedtls_pk_verify(
            &publicKey,
            MBEDTLS_MD_SHA256,
            digest,
            sizeof(digest),
            signature,
            signatureLength
        );

    mbedtls_pk_free(&publicKey);

    return verifyResult == 0;
}

static bool evaluateActivationCode(
    const String &activationCode,
    LicenseData &data,
    LicenseStatus &status,
    String &error
)
{
    memset(
        &data,
        0,
        sizeof(data)
    );

    data.edition =
        LICENSE_EDITION_UNLICENSED;

    if (!buildHardwareIdentity()) {
        status =
            LICENSE_STATUS_STORAGE_ERROR;
        error =
            "factory hardware identity unavailable";
        return false;
    }

    String code =
        activationCode;
    code.trim();

    if (
        !code.length() ||
        code.length() > LICENSE_MAX_CODE_BYTES
    ) {
        status =
            LICENSE_STATUS_INVALID_FORMAT;
        error =
            "activation code is empty or too long";
        return false;
    }

    String payloadText;
    String signatureText;

    if (!parseActivationCode(
            code,
            payloadText,
            signatureText,
            error
        )) {
        status =
            LICENSE_STATUS_INVALID_FORMAT;
        return false;
    }

    uint8_t payload[LICENSE_PAYLOAD_BYTES] = {};
    size_t payloadLength = 0;

    if (!decodeBase64Url(
            payloadText,
            payload,
            sizeof(payload),
            payloadLength
        )) {
        status =
            LICENSE_STATUS_INVALID_FORMAT;
        error =
            "activation payload is not valid base64url";
        return false;
    }

    uint8_t signature[96] = {};
    size_t signatureLength = 0;

    if (!decodeBase64Url(
            signatureText,
            signature,
            sizeof(signature),
            signatureLength
        )) {
        status =
            LICENSE_STATUS_INVALID_FORMAT;
        error =
            "activation signature is not valid base64url";
        return false;
    }

    if (!parsePayload(
            payload,
            payloadLength,
            data,
            status,
            error
        )) {
        return false;
    }

    if (!verifySignature(
            payload,
            payloadLength,
            signature,
            signatureLength
        )) {
        status =
            LICENSE_STATUS_INVALID_SIGNATURE;
        error =
            "license signature verification failed";
        return false;
    }

    if (
        data.productId !=
        LICENSE_PRODUCT_SENSORFORGE
    ) {
        status =
            LICENSE_STATUS_WRONG_PRODUCT;
        error =
            "license is for a different product";
        return false;
    }

    if (
        memcmp(
            data.hardwareId,
            deviceHardwareId,
            sizeof(deviceHardwareId)
        ) != 0
    ) {
        status =
            LICENSE_STATUS_WRONG_DEVICE;
        error =
            "license is for a different device";
        return false;
    }

    if (data.expiresDays != 0) {
        time_t now =
            time(nullptr);

        if (now < (time_t)1609459200) {
            status =
                LICENSE_STATUS_TIME_UNAVAILABLE;
            error =
                "system time is unavailable for license expiry validation";
            return false;
        }

        uint32_t currentDays =
            (uint32_t)(
                (uint64_t)now /
                86400ULL
            );

        if (currentDays > data.expiresDays) {
            status =
                LICENSE_STATUS_EXPIRED;
            error =
                "license has expired";
            return false;
        }
    }

    status =
        LICENSE_STATUS_VALID;
    error = "";
    return true;
}

static void recoverLicenseFiles()
{
    bool hasMain =
        LittleFS.exists(
            LICENSE_PATH
        );

    bool hasBackup =
        LittleFS.exists(
            LICENSE_BAK_PATH
        );

    if (!hasMain && hasBackup) {
        LittleFS.rename(
            LICENSE_BAK_PATH,
            LICENSE_PATH
        );

        hasMain =
            LittleFS.exists(
                LICENSE_PATH
            );
    }

    if (LittleFS.exists(LICENSE_TMP_PATH)) {
        LittleFS.remove(
            LICENSE_TMP_PATH
        );
    }

    if (
        hasMain &&
        LittleFS.exists(LICENSE_BAK_PATH)
    ) {
        LittleFS.remove(
            LICENSE_BAK_PATH
        );
    }
}

static bool readInstalledLicense(
    String &text,
    String &error
)
{
    text = "";
    error = "";

    if (!LittleFS.exists(LICENSE_PATH))
        return true;

    File file =
        LittleFS.open(
            LICENSE_PATH,
            FILE_READ
        );

    if (!file) {
        error =
            "cannot open internal license file";
        return false;
    }

    size_t size =
        file.size();

    if (
        size == 0 ||
        size > LICENSE_MAX_CODE_BYTES
    ) {
        file.close();
        error =
            "internal activation code has invalid size";
        return false;
    }

    text.reserve(size + 1);

    while (file.available()) {
        int value = file.read();

        if (value < 0)
            break;

        text += (char)value;
    }

    file.close();

    if (text.length() != size) {
        error =
            "internal activation code could not be read completely";
        return false;
    }

    return true;
}

static bool writeInstalledLicenseAtomic(
    const String &text,
    String &error
)
{
    error = "";

    LittleFS.remove(
        LICENSE_TMP_PATH
    );

    File file =
        LittleFS.open(
            LICENSE_TMP_PATH,
            FILE_WRITE
        );

    if (!file) {
        error =
            "cannot create internal license temporary file";
        return false;
    }

    size_t written =
        file.write(
            (const uint8_t *)text.c_str(),
            text.length()
        );

    file.flush();
    file.close();

    if (written != text.length()) {
        LittleFS.remove(
            LICENSE_TMP_PATH
        );
        error =
            "internal license write was incomplete";
        return false;
    }

    bool hadOld =
        LittleFS.exists(
            LICENSE_PATH
        );

    LittleFS.remove(
        LICENSE_BAK_PATH
    );

    if (hadOld) {
        if (!LittleFS.rename(
                LICENSE_PATH,
                LICENSE_BAK_PATH
            )) {
            LittleFS.remove(
                LICENSE_TMP_PATH
            );
            error =
                "cannot prepare existing license for replacement";
            return false;
        }
    }

    if (!LittleFS.rename(
            LICENSE_TMP_PATH,
            LICENSE_PATH
        )) {
        if (hadOld) {
            LittleFS.rename(
                LICENSE_BAK_PATH,
                LICENSE_PATH
            );
        }

        LittleFS.remove(
            LICENSE_TMP_PATH
        );
        error =
            "cannot activate new internal license";
        return false;
    }

    LittleFS.remove(
        LICENSE_BAK_PATH
    );

    return true;
}

static String formatLicenseId(
    const uint8_t *bytes
)
{
    static const char hex[] =
        "0123456789ABCDEF";

    String result;
    result.reserve(36);

    for (uint8_t i = 0; i < 16; ++i) {
        if (
            i == 4 ||
            i == 6 ||
            i == 8 ||
            i == 10
        ) {
            result += '-';
        }

        result +=
            hex[(bytes[i] >> 4) & 0x0F];
        result +=
            hex[bytes[i] & 0x0F];
    }

    return result;
}

static String formatEpochDay(uint32_t days)
{
    if (days == 0)
        return String();

    uint64_t seconds =
        (uint64_t)days *
        86400ULL;

    time_t epoch =
        (time_t)seconds;

    struct tm utcTime;

    if (!gmtime_r(
            &epoch,
            &utcTime
        )) {
        return String();
    }

    char buffer[16];

    if (!strftime(
            buffer,
            sizeof(buffer),
            "%Y-%m-%d",
            &utcTime
        )) {
        return String();
    }

    return String(buffer);
}

} // namespace

void licenseBegin()
{
    clearLicenseData();

    if (!buildHardwareIdentity()) {
        currentStatus =
            LICENSE_STATUS_STORAGE_ERROR;

        Serial.println(
            "License: factory hardware identity unavailable"
        );
        return;
    }

    licenseReload();

    Serial.printf(
        "License: status=%s | hardware_id=%s\n",
        licenseStatusName(),
        hardwareIdText.c_str()
    );
}

bool licenseReload()
{
    recoverLicenseFiles();

    if (!LittleFS.exists(LICENSE_PATH)) {
        clearLicenseData();
        currentStatus =
            LICENSE_STATUS_MISSING;
        return true;
    }

    String text;
    String readError;

    if (!readInstalledLicense(
            text,
            readError
        )) {
        clearLicenseData();
        currentStatus =
            LICENSE_STATUS_STORAGE_ERROR;
        return false;
    }

    LicenseData candidate;
    LicenseStatus status =
        LICENSE_STATUS_INVALID_FORMAT;
    String validationError;

    bool valid =
        evaluateActivationCode(
            text,
            candidate,
            status,
            validationError
        );

    currentLicense = candidate;
    currentStatus = status;

    return
        valid ||
        status != LICENSE_STATUS_STORAGE_ERROR;
}

LicenseStatus licenseStatus()
{
    return currentStatus;
}

const char *licenseStatusName(LicenseStatus status)
{
    switch (status) {
        case LICENSE_STATUS_VALID:
            return "VALID";
        case LICENSE_STATUS_MISSING:
            return "MISSING";
        case LICENSE_STATUS_INVALID_FORMAT:
            return "INVALID_FORMAT";
        case LICENSE_STATUS_INVALID_SIGNATURE:
            return "INVALID_SIGNATURE";
        case LICENSE_STATUS_WRONG_DEVICE:
            return "WRONG_DEVICE";
        case LICENSE_STATUS_WRONG_PRODUCT:
            return "WRONG_PRODUCT";
        case LICENSE_STATUS_UNSUPPORTED_VERSION:
            return "UNSUPPORTED_VERSION";
        case LICENSE_STATUS_EXPIRED:
            return "EXPIRED";
        case LICENSE_STATUS_TIME_UNAVAILABLE:
            return "TIME_UNAVAILABLE";
        case LICENSE_STATUS_STORAGE_ERROR:
            return "STORAGE_ERROR";
        default:
            return "UNKNOWN";
    }
}

const char *licenseStatusName()
{
    return licenseStatusName(currentStatus);
}

bool licenseIsValid()
{
    return
        currentStatus ==
        LICENSE_STATUS_VALID;
}

LicenseEdition licenseEdition()
{
    if (!licenseIsValid())
        return LICENSE_EDITION_UNLICENSED;

    return currentLicense.edition;
}

const char *licenseEditionName()
{
    switch (licenseEdition()) {
        case LICENSE_EDITION_FULL:
            return "FULL";
        case LICENSE_EDITION_EVALUATION:
            return "EVALUATION";
        case LICENSE_EDITION_SERVICE:
            return "SERVICE";
        case LICENSE_EDITION_UNLICENSED:
        default:
            return "UNLICENSED";
    }
}

uint32_t licenseFeatureFlags()
{
    if (!licenseIsValid())
        return 0;

    return currentLicense.featureFlags;
}

bool licenseFeatureEnabled(uint32_t featureMask)
{
    if (!licenseIsValid())
        return false;

    return
        (
            currentLicense.featureFlags &
            featureMask
        ) == featureMask;
}

String licenseHardwareId()
{
    if (!buildHardwareIdentity())
        return "UNAVAILABLE";

    return hardwareIdText;
}

String licenseId()
{
    if (!licenseIsValid())
        return String();

    return
        formatLicenseId(
            currentLicense.licenseId
        );
}

String licenseIssuedDateText()
{
    if (!licenseIsValid())
        return String();

    return
        formatEpochDay(
            currentLicense.issuedDays
        );
}

String licenseExpiryDateText()
{
    if (!licenseIsValid())
        return String();

    if (currentLicense.expiresDays == 0)
        return "never";

    return
        formatEpochDay(
            currentLicense.expiresDays
        );
}

bool licenseInstallCode(
    const String &activationCode,
    String &error,
    LicenseStatus *resultStatus
)
{
    String code =
        activationCode;
    code.trim();

    LicenseData candidate;
    LicenseStatus status =
        LICENSE_STATUS_INVALID_FORMAT;

    if (!evaluateActivationCode(
            code,
            candidate,
            status,
            error
        )) {
        if (resultStatus)
            *resultStatus = status;
        return false;
    }

    if (status != LICENSE_STATUS_VALID) {
        if (resultStatus)
            *resultStatus = status;
        if (!error.length())
            error = "license is not valid";
        return false;
    }

    if (!writeInstalledLicenseAtomic(
            code,
            error
        )) {
        if (resultStatus)
            *resultStatus = LICENSE_STATUS_STORAGE_ERROR;
        return false;
    }

    currentLicense = candidate;
    currentStatus =
        LICENSE_STATUS_VALID;

    if (resultStatus)
        *resultStatus = LICENSE_STATUS_VALID;

    return true;
}

bool licenseRemove(String &error)
{
    error = "";

    const char *paths[] = {
        LICENSE_PATH,
        LICENSE_TMP_PATH,
        LICENSE_BAK_PATH
    };

    for (const char *path : paths) {
        if (
            LittleFS.exists(path) &&
            !LittleFS.remove(path)
        ) {
            error =
                "cannot remove internal license file";
            return false;
        }
    }

    clearLicenseData();
    currentStatus =
        LICENSE_STATUS_MISSING;

    return true;
}
