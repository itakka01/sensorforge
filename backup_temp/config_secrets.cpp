#include "config_secrets.h"
#include "recording_crypto.h"

#include <esp_system.h>
#include <psa/crypto.h>
#include <string.h>

namespace {

static const char *SFSEC_PREFIX = "SFSEC1:";
static const size_t SFSEC_NONCE_BYTES = 12;
static const size_t SFSEC_TAG_BYTES = 16;
static const size_t SFSEC_MAX_PLAINTEXT_BYTES = 255;

static const char BASE64URL_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void secureWipe(void *pointer, size_t length)
{
    if (!pointer)
        return;

    volatile uint8_t *p =
        static_cast<volatile uint8_t *>(pointer);

    while (length--)
        *p++ = 0;
}

static bool fieldSupported(const char *fieldName)
{
    if (!fieldName)
        return false;

    return
        strcmp(fieldName, "wifi_pass") == 0 ||
        strcmp(fieldName, "hotspot_password") == 0 ||
        strcmp(fieldName, "web_password") == 0;
}

static bool importAesGcmKey(
    const uint8_t key[32],
    psa_key_id_t &keyId
)
{
    keyId = 0;

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_usage_flags(
        &attributes,
        PSA_KEY_USAGE_ENCRYPT |
        PSA_KEY_USAGE_DECRYPT
    );

    psa_status_t status = psa_import_key(
        &attributes,
        key,
        32,
        &keyId
    );

    psa_reset_key_attributes(&attributes);

    return status == PSA_SUCCESS && keyId != 0;
}

static String base64UrlEncode(
    const uint8_t *data,
    size_t length
)
{
    String output;

    if (!data && length)
        return output;

    output.reserve(((length + 2U) / 3U) * 4U);

    size_t i = 0;

    while (i + 3U <= length) {
        uint32_t value =
            ((uint32_t)data[i] << 16) |
            ((uint32_t)data[i + 1U] << 8) |
            ((uint32_t)data[i + 2U]);

        output += BASE64URL_ALPHABET[(value >> 18) & 0x3FU];
        output += BASE64URL_ALPHABET[(value >> 12) & 0x3FU];
        output += BASE64URL_ALPHABET[(value >> 6) & 0x3FU];
        output += BASE64URL_ALPHABET[value & 0x3FU];

        i += 3U;
    }

    size_t remaining = length - i;

    if (remaining == 1U) {
        uint32_t value =
            ((uint32_t)data[i] << 16);

        output += BASE64URL_ALPHABET[(value >> 18) & 0x3FU];
        output += BASE64URL_ALPHABET[(value >> 12) & 0x3FU];

    } else if (remaining == 2U) {
        uint32_t value =
            ((uint32_t)data[i] << 16) |
            ((uint32_t)data[i + 1U] << 8);

        output += BASE64URL_ALPHABET[(value >> 18) & 0x3FU];
        output += BASE64URL_ALPHABET[(value >> 12) & 0x3FU];
        output += BASE64URL_ALPHABET[(value >> 6) & 0x3FU];
    }

    return output;
}

static int base64UrlValue(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';

    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;

    if (c >= '0' && c <= '9')
        return c - '0' + 52;

    if (c == '-')
        return 62;

    if (c == '_')
        return 63;

    return -1;
}

static bool base64UrlDecode(
    const String &encoded,
    uint8_t *output,
    size_t outputCapacity,
    size_t &outputLength
)
{
    outputLength = 0;

    if (!output)
        return false;

    size_t length = encoded.length();

    if ((length % 4U) == 1U)
        return false;

    uint32_t accumulator = 0;
    int bits = 0;

    for (size_t i = 0; i < length; ++i) {
        int value = base64UrlValue(encoded[i]);

        if (value < 0)
            return false;

        accumulator =
            (accumulator << 6) |
            (uint32_t)value;

        bits += 6;

        if (bits >= 8) {
            bits -= 8;

            if (outputLength >= outputCapacity)
                return false;

            output[outputLength++] =
                (uint8_t)((accumulator >> bits) & 0xFFU);
        }
    }

    // Unpadded base64url may leave 2 or 4 insignificant low bits. Reject any
    // non-zero tail so alternative malformed encodings cannot be accepted.
    if (bits > 0) {
        uint32_t mask =
            ((uint32_t)1U << bits) - 1U;

        if ((accumulator & mask) != 0U)
            return false;
    }

    return true;
}

static String makeAad(const char *fieldName)
{
    String aad = "SFSEC1|";
    aad += fieldName;
    return aad;
}

} // namespace

bool configSecretIsEncrypted(const String &value)
{
    return value.startsWith(SFSEC_PREFIX);
}

bool configSecretEncrypt(
    const char *fieldName,
    const String &plaintext,
    String &storedValue,
    String &error
)
{
    storedValue = "";
    error = "";

    if (!fieldSupported(fieldName)) {
        error = "unsupported config secret field";
        return false;
    }

    size_t plaintextLength = plaintext.length();

    if (plaintextLength > SFSEC_MAX_PLAINTEXT_BYTES) {
        error = "config secret too long";
        return false;
    }

    if (!recordingCryptoEnsureProvisioned()) {
        error =
            "hardware secret key provisioning unavailable: " +
            String(recordingCryptoKeyStatusName());
        return false;
    }

    uint8_t configKey[32] = {};

    if (!recordingCryptoDeriveConfigSecretKey(configKey)) {
        error =
            "hardware secret key unavailable: " +
            String(recordingCryptoKeyStatusName());
        secureWipe(configKey, sizeof(configKey));
        return false;
    }

    uint8_t nonce[SFSEC_NONCE_BYTES] = {};
    esp_fill_random(nonce, sizeof(nonce));

    uint8_t ciphertextAndTag[
        SFSEC_MAX_PLAINTEXT_BYTES + SFSEC_TAG_BYTES
    ] = {};

    String aad = makeAad(fieldName);

    psa_key_id_t keyId = 0;

    if (!importAesGcmKey(configKey, keyId)) {
        error = "cannot import config secret AES key";
        secureWipe(configKey, sizeof(configKey));
        return false;
    }

    size_t encryptedLength = 0;

    psa_status_t status = psa_aead_encrypt(
        keyId,
        PSA_ALG_GCM,
        nonce,
        sizeof(nonce),
        reinterpret_cast<const uint8_t *>(aad.c_str()),
        aad.length(),
        reinterpret_cast<const uint8_t *>(plaintext.c_str()),
        plaintextLength,
        ciphertextAndTag,
        sizeof(ciphertextAndTag),
        &encryptedLength
    );

    psa_destroy_key(keyId);
    secureWipe(configKey, sizeof(configKey));

    if (
        status != PSA_SUCCESS ||
        encryptedLength != plaintextLength + SFSEC_TAG_BYTES
    ) {
        error =
            "config secret encryption failed: " +
            String((int)status);
        secureWipe(ciphertextAndTag, sizeof(ciphertextAndTag));
        return false;
    }

    uint8_t packed[
        SFSEC_NONCE_BYTES +
        SFSEC_MAX_PLAINTEXT_BYTES +
        SFSEC_TAG_BYTES
    ] = {};

    memcpy(packed, nonce, sizeof(nonce));
    memcpy(
        packed + sizeof(nonce),
        ciphertextAndTag,
        encryptedLength
    );

    storedValue = SFSEC_PREFIX;
    storedValue += base64UrlEncode(
        packed,
        sizeof(nonce) + encryptedLength
    );

    secureWipe(nonce, sizeof(nonce));
    secureWipe(ciphertextAndTag, sizeof(ciphertextAndTag));
    secureWipe(packed, sizeof(packed));

    if (!storedValue.length()) {
        error = "config secret encoding failed";
        return false;
    }

    return true;
}

bool configSecretDecode(
    const char *fieldName,
    const String &storedValue,
    String &plaintext,
    bool &wasEncrypted,
    String &error
)
{
    plaintext = "";
    wasEncrypted = false;
    error = "";

    if (!fieldSupported(fieldName)) {
        error = "unsupported config secret field";
        return false;
    }

    if (!configSecretIsEncrypted(storedValue)) {
        plaintext = storedValue;
        return true;
    }

    wasEncrypted = true;

    String encoded = storedValue.substring(strlen(SFSEC_PREFIX));

    uint8_t packed[
        SFSEC_NONCE_BYTES +
        SFSEC_MAX_PLAINTEXT_BYTES +
        SFSEC_TAG_BYTES
    ] = {};

    size_t packedLength = 0;

    if (!base64UrlDecode(
            encoded,
            packed,
            sizeof(packed),
            packedLength
        )) {
        error = "invalid SFSEC1 base64url";
        secureWipe(packed, sizeof(packed));
        return false;
    }

    if (
        packedLength <
        SFSEC_NONCE_BYTES + SFSEC_TAG_BYTES
    ) {
        error = "invalid SFSEC1 payload length";
        secureWipe(packed, sizeof(packed));
        return false;
    }

    const uint8_t *nonce = packed;
    const uint8_t *ciphertextAndTag =
        packed + SFSEC_NONCE_BYTES;

    size_t ciphertextAndTagLength =
        packedLength - SFSEC_NONCE_BYTES;

    size_t expectedPlaintextLength =
        ciphertextAndTagLength - SFSEC_TAG_BYTES;

    if (expectedPlaintextLength > SFSEC_MAX_PLAINTEXT_BYTES) {
        error = "SFSEC1 plaintext length out of range";
        secureWipe(packed, sizeof(packed));
        return false;
    }

    uint8_t configKey[32] = {};

    if (!recordingCryptoDeriveConfigSecretKey(configKey)) {
        error =
            "hardware secret key unavailable: " +
            String(recordingCryptoKeyStatusName());
        secureWipe(configKey, sizeof(configKey));
        secureWipe(packed, sizeof(packed));
        return false;
    }

    uint8_t plaintextBytes[SFSEC_MAX_PLAINTEXT_BYTES + 1U] = {};
    String aad = makeAad(fieldName);

    psa_key_id_t keyId = 0;

    if (!importAesGcmKey(configKey, keyId)) {
        error = "cannot import config secret AES key";
        secureWipe(configKey, sizeof(configKey));
        secureWipe(packed, sizeof(packed));
        return false;
    }

    size_t decryptedLength = 0;

    psa_status_t status = psa_aead_decrypt(
        keyId,
        PSA_ALG_GCM,
        nonce,
        SFSEC_NONCE_BYTES,
        reinterpret_cast<const uint8_t *>(aad.c_str()),
        aad.length(),
        ciphertextAndTag,
        ciphertextAndTagLength,
        plaintextBytes,
        SFSEC_MAX_PLAINTEXT_BYTES,
        &decryptedLength
    );

    psa_destroy_key(keyId);
    secureWipe(configKey, sizeof(configKey));
    secureWipe(packed, sizeof(packed));

    if (
        status != PSA_SUCCESS ||
        decryptedLength != expectedPlaintextLength
    ) {
        error =
            "SFSEC1 authentication/decryption failed: " +
            String((int)status);
        secureWipe(plaintextBytes, sizeof(plaintextBytes));
        return false;
    }

    plaintextBytes[decryptedLength] = 0;

    plaintext = String(
        reinterpret_cast<const char *>(plaintextBytes)
    );

    // Config secrets are textual values. Embedded NUL bytes are never valid
    // and would otherwise truncate the Arduino String constructor above.
    if (plaintext.length() != decryptedLength) {
        error = "SFSEC1 plaintext contains invalid NUL byte";
        plaintext = "";
        secureWipe(plaintextBytes, sizeof(plaintextBytes));
        return false;
    }

    secureWipe(plaintextBytes, sizeof(plaintextBytes));
    return true;
}
