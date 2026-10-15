#include "recording_crypto.h"

#include <bootloader_random.h>
#include <esp_efuse.h>
#include <esp_efuse_chip.h>
#include <esp_hmac.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <psa/crypto.h>
#include <string.h>

namespace {

struct KeySlot {
    int logicalId;
    esp_efuse_block_t block;
    hmac_key_id_t hmacId;
};

static const KeySlot KEY_SLOTS[] = {
    {0, EFUSE_BLK_KEY0, HMAC_KEY0},
    {1, EFUSE_BLK_KEY1, HMAC_KEY1},
    {2, EFUSE_BLK_KEY2, HMAC_KEY2},
    {3, EFUSE_BLK_KEY3, HMAC_KEY3},
    {4, EFUSE_BLK_KEY4, HMAC_KEY4},
    {5, EFUSE_BLK_KEY5, HMAC_KEY5},
};

static const size_t KEY_SLOT_COUNT =
    sizeof(KEY_SLOTS) / sizeof(KEY_SLOTS[0]);

// Prefer KEY5 because ESP32-S3 KEY5 cannot be used for XTS-AES flash
// encryption, then fall back through the remaining genuinely unused slots.
// This keeps SensorForge compatible with boards that already consumed one or
// more key blocks for other software/security features.
static const int KEY_PREFERENCE[] = {5, 4, 3, 2, 1, 0};

// SensorForge metadata binding inside EFUSE_BLK_USER_DATA.
//
// IMPORTANT ESP32-S3 eFuse rule:
// EFUSE_BLK_USER_DATA uses Reed-Solomon coding across the complete 256-bit
// block. Once ANY payload in that block has been burned, the block must not be
// programmed again, even if other payload bits still read as zero.
//
// Therefore SensorForge performs exactly ONE USER_DATA burn:
//   "this board's SensorForge storage root lives in KEYx"
//
// That binding is final as soon as it exists. There is deliberately no later
// RESERVED->READY state update. Power-loss recovery is handled by inspecting
// the bound KEYx slot:
//   - empty      -> resume key provisioning into the same slot
//   - HMAC_UP    -> use it immediately
//   - other use  -> fail closed with KEY_CONFLICT
//
// The parser still recognizes the earlier integration-test READY value 0x03
// so boards flashed with the short-lived previous revision remain recoverable.
static const uint16_t METADATA_OFFSETS_BITS[] = {128, 64, 0};
static const size_t METADATA_RECORD_BYTES = 8;
static const size_t METADATA_RECORD_BITS = 64;

static const uint8_t METADATA_MAGIC[5] = {
    'S', 'F', 'K', 'E', 'Y'
};
static const uint8_t METADATA_VERSION = 1;
static const uint8_t METADATA_SLOT_PREFIX = 0xA0U;
static const uint8_t METADATA_STATE_BOUND = 0x01U;
static const uint8_t METADATA_STATE_LEGACY_READY = 0x03U;

static const uint8_t STORAGE_MASTER_DOMAIN[] =
    "SENSORFORGE-STORAGE-MASTER-V1";

static const uint8_t STORAGE_KEY_ID_DOMAIN[] =
    "SENSORFORGE-STORAGE-KEY-ID-V1";

static const uint8_t CONFIG_SECRET_DOMAIN[] =
    "SENSORFORGE-CONFIG-SECRETS-V1";

static const uint8_t LOG_STORAGE_DOMAIN[] =
    "SENSORFORGE-LOG-STORAGE-V1";

// Read-only migration support for SFENC1 files created during the integration
// stage before eFuse provisioning existed. This seed is NEVER used for new
// recordings after this revision.
static const uint8_t LEGACY_DEVELOPMENT_SEED[32] = {
    0x7A, 0x19, 0xD4, 0xB2, 0x56, 0x8F, 0x31, 0xC7,
    0xA4, 0x0D, 0xE9, 0x63, 0x25, 0xB8, 0x71, 0x4E,
    0x92, 0xF0, 0x3C, 0xAD, 0x68, 0x15, 0xCB, 0x87,
    0x41, 0xDE, 0x22, 0x59, 0xB3, 0x06, 0xFC, 0x9A
};

struct MetadataRecord {
    bool empty = false;
    bool valid = false;
    uint16_t offsetBits = 0;
    int keySlot = -1;
    uint8_t state = 0;
};

static bool initialized = false;
static bool currentReady = false;
static RecordingCryptoKeyStatus currentStatus =
    RECORDING_CRYPTO_KEY_UNKNOWN;
static int currentKeySlot = -1;
static int currentMetadataIndex = -1;

static uint8_t storageMasterKey[32] = {};
static uint8_t storageKeyId[16] = {};

static bool legacyIdentityReady = false;
static uint8_t legacyKeyId[16] = {};

static void secureWipe(void *pointer, size_t length)
{
    if (!pointer)
        return;

    volatile uint8_t *p =
        static_cast<volatile uint8_t *>(pointer);

    while (length--)
        *p++ = 0;
}

static bool secureEqual(
    const uint8_t *a,
    const uint8_t *b,
    size_t length
)
{
    if (!a || !b)
        return false;

    uint8_t difference = 0;

    for (size_t i = 0; i < length; ++i)
        difference |= (uint8_t)(a[i] ^ b[i]);

    return difference == 0;
}

static const KeySlot *slotForLogicalId(int logicalId)
{
    for (size_t i = 0; i < KEY_SLOT_COUNT; ++i) {
        if (KEY_SLOTS[i].logicalId == logicalId)
            return &KEY_SLOTS[i];
    }

    return nullptr;
}

static bool importHmacKey(
    const uint8_t key[32],
    psa_key_id_t &keyId
)
{
    keyId = 0;

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_algorithm(
        &attributes,
        PSA_ALG_HMAC(PSA_ALG_SHA_256)
    );
    psa_set_key_usage_flags(
        &attributes,
        PSA_KEY_USAGE_SIGN_MESSAGE
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

static bool hmacSha256(
    const uint8_t key[32],
    const uint8_t *message,
    size_t messageLength,
    uint8_t output[32]
)
{
    if (!key || !message || !messageLength || !output)
        return false;

    psa_key_id_t keyId = 0;

    if (!importHmacKey(key, keyId))
        return false;

    size_t outputLength = 0;

    psa_status_t status = psa_mac_compute(
        keyId,
        PSA_ALG_HMAC(PSA_ALG_SHA_256),
        message,
        messageLength,
        output,
        32,
        &outputLength
    );

    psa_destroy_key(keyId);

    return
        status == PSA_SUCCESS &&
        outputLength == 32;
}

static bool deriveKeyIdFromMaster(
    const uint8_t masterKey[32],
    uint8_t output[16]
)
{
    uint8_t digest[32] = {};

    bool ok = hmacSha256(
        masterKey,
        STORAGE_KEY_ID_DOMAIN,
        sizeof(STORAGE_KEY_ID_DOMAIN) - 1,
        digest
    );

    if (ok)
        memcpy(output, digest, 16);

    secureWipe(digest, sizeof(digest));
    return ok;
}

static bool deriveLegacyDevelopmentMasterKey(uint8_t output[32])
{
    uint8_t factoryMac[6] = {};

    if (esp_efuse_mac_get_default(factoryMac) != ESP_OK)
        return false;

    static const uint8_t domain[] =
        "SENSORFORGE-STORAGE-DEV-MASTER-V1";

    uint8_t message[
        sizeof(domain) - 1 + sizeof(factoryMac)
    ] = {};

    memcpy(message, domain, sizeof(domain) - 1);
    memcpy(
        message + sizeof(domain) - 1,
        factoryMac,
        sizeof(factoryMac)
    );

    bool ok = hmacSha256(
        LEGACY_DEVELOPMENT_SEED,
        message,
        sizeof(message),
        output
    );

    secureWipe(message, sizeof(message));
    return ok;
}

static bool ensureLegacyIdentity()
{
    if (legacyIdentityReady)
        return true;

    uint8_t master[32] = {};

    bool ok =
        deriveLegacyDevelopmentMasterKey(master) &&
        deriveKeyIdFromMaster(master, legacyKeyId);

    secureWipe(master, sizeof(master));

    legacyIdentityReady = ok;
    return ok;
}

static bool deriveOneFileKeyFromMaster(
    const uint8_t masterKey[32],
    const char *domain,
    const uint8_t fileNonce[16],
    uint8_t output[32]
)
{
    if (!masterKey || !domain || !fileNonce || !output)
        return false;

    size_t domainLength = strlen(domain);

    if (domainLength > 64)
        return false;

    uint8_t message[80] = {};

    memcpy(message, domain, domainLength);
    memcpy(message + domainLength, fileNonce, 16);

    bool ok = hmacSha256(
        masterKey,
        message,
        domainLength + 16,
        output
    );

    secureWipe(message, sizeof(message));
    return ok;
}

static bool deriveFileKeysFromMaster(
    const uint8_t masterKey[32],
    const uint8_t fileNonce[16],
    uint8_t aesKey[32],
    uint8_t hmacKey[32]
)
{
    if (!deriveOneFileKeyFromMaster(
            masterKey,
            "SENSORFORGE-SD-ENC-V1",
            fileNonce,
            aesKey
        )) {
        return false;
    }

    if (!deriveOneFileKeyFromMaster(
            masterKey,
            "SENSORFORGE-SD-MAC-V1",
            fileNonce,
            hmacKey
        )) {
        secureWipe(aesKey, 32);
        return false;
    }

    return true;
}

static bool readMetadataRaw(
    uint16_t offsetBits,
    uint8_t raw[METADATA_RECORD_BYTES]
)
{
    memset(raw, 0, METADATA_RECORD_BYTES);

    return esp_efuse_read_block(
        EFUSE_BLK_USER_DATA,
        raw,
        offsetBits,
        METADATA_RECORD_BITS
    ) == ESP_OK;
}

static bool bytesAllZero(const uint8_t *data, size_t length)
{
    if (!data)
        return false;

    for (size_t i = 0; i < length; ++i) {
        if (data[i] != 0)
            return false;
    }

    return true;
}

static MetadataRecord inspectMetadataRecord(
    size_t metadataIndex
)
{
    MetadataRecord result;

    if (
        metadataIndex >=
        sizeof(METADATA_OFFSETS_BITS) /
            sizeof(METADATA_OFFSETS_BITS[0])
    ) {
        return result;
    }

    result.offsetBits =
        METADATA_OFFSETS_BITS[metadataIndex];

    uint8_t raw[METADATA_RECORD_BYTES] = {};

    if (!readMetadataRaw(result.offsetBits, raw))
        return result;

    result.empty =
        bytesAllZero(raw, sizeof(raw));

    if (result.empty)
        return result;

    if (
        memcmp(raw, METADATA_MAGIC, sizeof(METADATA_MAGIC)) != 0 ||
        raw[5] != METADATA_VERSION ||
        (raw[6] & 0xF8U) != METADATA_SLOT_PREFIX
    ) {
        return result;
    }

    int keySlot =
        (int)(raw[6] & 0x07U);

    if (!slotForLogicalId(keySlot))
        return result;

    if (
        raw[7] != METADATA_STATE_BOUND &&
        raw[7] != METADATA_STATE_LEGACY_READY
    ) {
        return result;
    }

    result.valid = true;
    result.keySlot = keySlot;
    result.state = raw[7];

    return result;
}

static bool writeMetadataBinding(
    int metadataIndex,
    int keySlot
)
{
    if (
        metadataIndex != 0 ||
        !slotForLogicalId(keySlot)
    ) {
        return false;
    }

    // ESP32-S3 USER_DATA is an RS-coded block and can only be burned once.
    // Do not attempt a partial write when ANY prior software has already used
    // this block, even if our own 64-bit record area still reads as zero.
    if (!esp_efuse_block_is_empty(EFUSE_BLK_USER_DATA))
        return false;

    uint8_t raw[METADATA_RECORD_BYTES] = {};

    memcpy(raw, METADATA_MAGIC, sizeof(METADATA_MAGIC));
    raw[5] = METADATA_VERSION;
    raw[6] = (uint8_t)(METADATA_SLOT_PREFIX | keySlot);
    raw[7] = METADATA_STATE_BOUND;

    esp_err_t result = esp_efuse_write_block(
        EFUSE_BLK_USER_DATA,
        raw,
        METADATA_OFFSETS_BITS[metadataIndex],
        METADATA_RECORD_BITS
    );

    if (result != ESP_OK)
        return false;

    MetadataRecord verify =
        inspectMetadataRecord((size_t)metadataIndex);

    return
        verify.valid &&
        verify.keySlot == keySlot &&
        (
            verify.state == METADATA_STATE_BOUND ||
            verify.state == METADATA_STATE_LEGACY_READY
        );
}

static bool scanMetadataBinding(
    int &bindingIndex,
    int &bindingKeySlot
)
{
    bindingIndex = -1;
    bindingKeySlot = -1;

    const size_t recordCount =
        sizeof(METADATA_OFFSETS_BITS) /
        sizeof(METADATA_OFFSETS_BITS[0]);

    for (size_t i = 0; i < recordCount; ++i) {
        MetadataRecord record =
            inspectMetadataRecord(i);

        if (!record.valid)
            continue;

        if (
            bindingIndex >= 0 &&
            bindingKeySlot != record.keySlot
        ) {
            currentStatus =
                RECORDING_CRYPTO_KEY_METADATA_CONFLICT;
            return false;
        }

        bindingIndex = (int)i;
        bindingKeySlot = record.keySlot;
    }

    return true;
}

static bool calculateEfuseHmac(
    const KeySlot &slot,
    const uint8_t *message,
    size_t messageLength,
    uint8_t output[32]
)
{
    if (!message || !messageLength || !output)
        return false;

    return esp_hmac_calculate(
        slot.hmacId,
        message,
        messageLength,
        output
    ) == ESP_OK;
}

static bool loadMasterFromEfuseSlot(int logicalSlot)
{
    const KeySlot *slot =
        slotForLogicalId(logicalSlot);

    if (!slot)
        return false;

    if (
        esp_efuse_get_key_purpose(slot->block) !=
        ESP_EFUSE_KEY_PURPOSE_HMAC_UP
    ) {
        return false;
    }

    uint8_t master1[32] = {};
    uint8_t master2[32] = {};

    bool ok =
        calculateEfuseHmac(
            *slot,
            STORAGE_MASTER_DOMAIN,
            sizeof(STORAGE_MASTER_DOMAIN) - 1,
            master1
        ) &&
        calculateEfuseHmac(
            *slot,
            STORAGE_MASTER_DOMAIN,
            sizeof(STORAGE_MASTER_DOMAIN) - 1,
            master2
        ) &&
        secureEqual(master1, master2, sizeof(master1));

    if (ok) {
        memcpy(
            storageMasterKey,
            master1,
            sizeof(storageMasterKey)
        );

        ok = deriveKeyIdFromMaster(
            storageMasterKey,
            storageKeyId
        );
    }

    secureWipe(master1, sizeof(master1));
    secureWipe(master2, sizeof(master2));

    if (!ok) {
        secureWipe(storageMasterKey, sizeof(storageMasterKey));
        secureWipe(storageKeyId, sizeof(storageKeyId));
    }

    return ok;
}

static int findPreferredUnusedKeySlot()
{
    for (size_t i = 0; i < sizeof(KEY_PREFERENCE) / sizeof(KEY_PREFERENCE[0]); ++i) {
        const KeySlot *slot =
            slotForLogicalId(KEY_PREFERENCE[i]);

        if (
            slot &&
            esp_efuse_key_block_unused(slot->block)
        ) {
            return slot->logicalId;
        }
    }

    return -1;
}

static bool fillStrongRandom(uint8_t *output, size_t length)
{
    if (!output || length == 0)
        return false;

    wifi_mode_t wifiMode = WIFI_MODE_NULL;

    bool rfEntropyAvailable =
        esp_wifi_get_mode(&wifiMode) == ESP_OK &&
        wifiMode != WIFI_MODE_NULL;

    if (!rfEntropyAvailable)
        bootloader_random_enable();

    esp_fill_random(output, length);

    if (!rfEntropyAvailable)
        bootloader_random_disable();

    // Defensive sanity check only; all-zero output is astronomically unlikely
    // for a healthy 256-bit RNG result.
    return !bytesAllZero(output, length);
}

static bool provisionKeyIntoSlot(int logicalSlot)
{
    const KeySlot *slot =
        slotForLogicalId(logicalSlot);

    if (!slot || !esp_efuse_key_block_unused(slot->block))
        return false;

    uint8_t rootKey[32] = {};

    if (!fillStrongRandom(rootKey, sizeof(rootKey))) {
        secureWipe(rootKey, sizeof(rootKey));
        return false;
    }

    esp_err_t result = esp_efuse_write_key(
        slot->block,
        ESP_EFUSE_KEY_PURPOSE_HMAC_UP,
        rootKey,
        sizeof(rootKey)
    );

    secureWipe(rootKey, sizeof(rootKey));

    if (result != ESP_OK)
        return false;

    return
        esp_efuse_get_key_purpose(slot->block) ==
            ESP_EFUSE_KEY_PURPOSE_HMAC_UP &&
        esp_efuse_get_key_dis_read(slot->block) &&
        esp_efuse_get_key_dis_write(slot->block) &&
        esp_efuse_get_keypurpose_dis_write(slot->block) &&
        loadMasterFromEfuseSlot(logicalSlot);
}

static void resetCurrentIdentity()
{
    currentReady = false;
    currentKeySlot = -1;
    currentMetadataIndex = -1;
    secureWipe(storageMasterKey, sizeof(storageMasterKey));
    secureWipe(storageKeyId, sizeof(storageKeyId));
}

static bool initializeFromExistingMetadata()
{
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
    currentStatus = RECORDING_CRYPTO_KEY_UNSUPPORTED;
    return false;
#else
    int bindingIndex = -1;
    int bindingSlot = -1;

    if (!scanMetadataBinding(
            bindingIndex,
            bindingSlot
        )) {
        return false;
    }

    if (bindingIndex < 0) {
        currentStatus = RECORDING_CRYPTO_KEY_UNPROVISIONED;
        return true;
    }

    const KeySlot *slot =
        slotForLogicalId(bindingSlot);

    if (!slot) {
        currentStatus = RECORDING_CRYPTO_KEY_KEY_CONFLICT;
        return false;
    }

    currentKeySlot = bindingSlot;
    currentMetadataIndex = bindingIndex;

    // The metadata binding is intentionally committed BEFORE the HMAC key.
    // If power disappears in that window, the next boot resumes provisioning
    // into exactly the same slot.
    if (esp_efuse_key_block_unused(slot->block)) {
        currentStatus =
            RECORDING_CRYPTO_KEY_PROVISION_PENDING;
        return true;
    }

    if (
        esp_efuse_get_key_purpose(slot->block) !=
            ESP_EFUSE_KEY_PURPOSE_HMAC_UP
    ) {
        currentStatus =
            RECORDING_CRYPTO_KEY_KEY_CONFLICT;
        return false;
    }

    if (!loadMasterFromEfuseSlot(bindingSlot)) {
        currentStatus =
            RECORDING_CRYPTO_KEY_HMAC_FAILED;
        return false;
    }

    currentReady = true;
    currentStatus = RECORDING_CRYPTO_KEY_READY;
    return true;
#endif
}

static bool selectMasterForKeyId(
    const uint8_t storedKeyId[16],
    uint8_t outputMaster[32]
)
{
    if (!storedKeyId || !outputMaster)
        return false;

    recordingCryptoBegin();

    if (
        currentReady &&
        secureEqual(storedKeyId, storageKeyId, 16)
    ) {
        memcpy(outputMaster, storageMasterKey, 32);
        return true;
    }

    if (
        ensureLegacyIdentity() &&
        secureEqual(storedKeyId, legacyKeyId, 16)
    ) {
        return deriveLegacyDevelopmentMasterKey(outputMaster);
    }

    return false;
}

} // namespace

bool recordingCryptoBegin()
{
    if (initialized)
        return currentStatus != RECORDING_CRYPTO_KEY_UNSUPPORTED;

    initialized = true;
    resetCurrentIdentity();

    if (psa_crypto_init() != PSA_SUCCESS) {
        currentStatus = RECORDING_CRYPTO_KEY_HMAC_FAILED;
        return false;
    }

    // Compute the legacy read-only key identity once. Failure here must not
    // prevent the production eFuse provider from operating.
    ensureLegacyIdentity();

    return initializeFromExistingMetadata();
}

bool recordingCryptoEnsureProvisioned()
{
    if (!recordingCryptoBegin())
        return false;

    if (currentReady)
        return true;

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
    currentStatus = RECORDING_CRYPTO_KEY_UNSUPPORTED;
    return false;
#else
    int bindingIndex = -1;
    int bindingSlot = -1;

    if (!scanMetadataBinding(
            bindingIndex,
            bindingSlot
        )) {
        return false;
    }

    if (bindingIndex < 0) {
        // USER_DATA is RS-coded on ESP32-S3 and is a one-shot block. If any
        // previous software has already used it, we must not try to append our
        // metadata into apparently-zero bits of the same block.
        if (!esp_efuse_block_is_empty(EFUSE_BLK_USER_DATA)) {
            currentStatus =
                RECORDING_CRYPTO_KEY_METADATA_UNAVAILABLE;
            return false;
        }

        int freeSlot = findPreferredUnusedKeySlot();

        if (freeSlot < 0) {
            currentStatus =
                RECORDING_CRYPTO_KEY_NO_FREE_KEY_BLOCK;
            return false;
        }

        // Commit the permanent board->slot binding first. This is the ONLY
        // SensorForge write to EFUSE_BLK_USER_DATA for the lifetime of the
        // device. No READY/ABANDONED update is performed later.
        if (!writeMetadataBinding(0, freeSlot)) {
            currentStatus =
                RECORDING_CRYPTO_KEY_PROVISION_FAILED;
            return false;
        }

        bindingIndex = 0;
        bindingSlot = freeSlot;

        Serial.printf(
            "RecordingCrypto provisioning | metadata_offset=%u | efuse_key=%d | binding=committed\n",
            (unsigned)METADATA_OFFSETS_BITS[bindingIndex],
            bindingSlot
        );
    }

    const KeySlot *slot =
        slotForLogicalId(bindingSlot);

    if (!slot) {
        currentStatus = RECORDING_CRYPTO_KEY_KEY_CONFLICT;
        return false;
    }

    if (esp_efuse_key_block_unused(slot->block)) {
        if (!provisionKeyIntoSlot(bindingSlot)) {
            // The binding is already permanent. Never jump to another key
            // block here: doing so would make recovery ambiguous after flash
            // loss. A damaged/failed eFuse burn therefore fails closed.
            currentStatus =
                RECORDING_CRYPTO_KEY_PROVISION_FAILED;
            return false;
        }

        Serial.printf(
            "RecordingCrypto provisioning | efuse_key=%d | purpose=HMAC_UP | key_material_readable=0\n",
            bindingSlot
        );
    } else if (
        esp_efuse_get_key_purpose(slot->block) ==
            ESP_EFUSE_KEY_PURPOSE_HMAC_UP
    ) {
        // This is the expected recovery path when the key was already burned
        // before a reset/power loss. The permanent metadata binding is enough;
        // there is no second USER_DATA state bit to commit.
        Serial.printf(
            "RecordingCrypto provisioning | efuse_key=%d | existing_hmac_binding=1\n",
            bindingSlot
        );
    } else {
        currentStatus = RECORDING_CRYPTO_KEY_KEY_CONFLICT;
        return false;
    }

    if (!loadMasterFromEfuseSlot(bindingSlot)) {
        currentStatus = RECORDING_CRYPTO_KEY_HMAC_FAILED;
        return false;
    }

    currentKeySlot = bindingSlot;
    currentMetadataIndex = bindingIndex;
    currentReady = true;
    currentStatus = RECORDING_CRYPTO_KEY_READY;

    Serial.printf(
        "RecordingCrypto provisioning | READY | efuse_key=%d | metadata_offset=%u | additional_user_data_burns=0\n",
        currentKeySlot,
        (unsigned)METADATA_OFFSETS_BITS[currentMetadataIndex]
    );

    return true;
#endif
}

bool recordingCryptoReady()
{
    recordingCryptoBegin();
    return currentReady;
}

RecordingCryptoKeyStatus recordingCryptoKeyStatus()
{
    recordingCryptoBegin();
    return currentStatus;
}

const char *recordingCryptoKeyStatusName()
{
    switch (recordingCryptoKeyStatus()) {
        case RECORDING_CRYPTO_KEY_UNPROVISIONED:
            return "UNPROVISIONED";
        case RECORDING_CRYPTO_KEY_PROVISION_PENDING:
            return "PROVISION_PENDING";
        case RECORDING_CRYPTO_KEY_READY:
            return "READY";
        case RECORDING_CRYPTO_KEY_NO_FREE_KEY_BLOCK:
            return "NO_FREE_KEY_BLOCK";
        case RECORDING_CRYPTO_KEY_METADATA_UNAVAILABLE:
            return "METADATA_UNAVAILABLE";
        case RECORDING_CRYPTO_KEY_METADATA_CONFLICT:
            return "METADATA_CONFLICT";
        case RECORDING_CRYPTO_KEY_KEY_CONFLICT:
            return "KEY_CONFLICT";
        case RECORDING_CRYPTO_KEY_PROVISION_FAILED:
            return "PROVISION_FAILED";
        case RECORDING_CRYPTO_KEY_HMAC_FAILED:
            return "HMAC_FAILED";
        case RECORDING_CRYPTO_KEY_UNSUPPORTED:
            return "UNSUPPORTED";
        case RECORDING_CRYPTO_KEY_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

int recordingCryptoKeySlot()
{
    recordingCryptoBegin();
    return currentReady ? currentKeySlot : -1;
}

const char *recordingCryptoKeySourceName()
{
    return currentReady
        ? "esp32s3-efuse-hmac"
        : "unavailable";
}

bool recordingCryptoUsingDevelopmentKey()
{
    return false;
}

bool recordingCryptoGetKeyId(uint8_t output[16])
{
    if (
        !output ||
        !recordingCryptoReady()
    ) {
        return false;
    }

    memcpy(output, storageKeyId, 16);
    return true;
}

bool recordingCryptoDeriveConfigSecretKey(uint8_t output[32])
{
    if (!output)
        return false;

    // Decryption must never provision a new board key merely because an
    // SFSEC1 value was encountered (for example after copying a config from
    // another device). Provisioning is performed explicitly by encryption /
    // migration callers before they request this derived key.
    if (
        !recordingCryptoBegin() ||
        !currentReady
    ) {
        return false;
    }

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
    return false;
#else
    const KeySlot *slot =
        slotForLogicalId(currentKeySlot);

    if (!slot)
        return false;

    return calculateEfuseHmac(
        *slot,
        CONFIG_SECRET_DOMAIN,
        sizeof(CONFIG_SECRET_DOMAIN) - 1,
        output
    );
#endif
}

bool recordingCryptoDeriveLogKey(uint8_t output[32])
{
    if (!output)
        return false;

    // Log decryption must also remain read-only with respect to eFuse state.
    // Provisioning is owned by the recording-encryption activation path.
    if (
        !recordingCryptoBegin() ||
        !currentReady
    ) {
        return false;
    }

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
    return false;
#else
    const KeySlot *slot =
        slotForLogicalId(currentKeySlot);

    if (!slot)
        return false;

    return calculateEfuseHmac(
        *slot,
        LOG_STORAGE_DOMAIN,
        sizeof(LOG_STORAGE_DOMAIN) - 1,
        output
    );
#endif
}

bool recordingCryptoKeyIdSupported(const uint8_t keyId[16])
{
    if (!keyId)
        return false;

    recordingCryptoBegin();

    if (
        currentReady &&
        secureEqual(keyId, storageKeyId, 16)
    ) {
        return true;
    }

    return
        ensureLegacyIdentity() &&
        secureEqual(keyId, legacyKeyId, 16);
}

bool recordingCryptoGenerateFileNonce(uint8_t output[16])
{
    if (
        !output ||
        !recordingCryptoEnsureProvisioned()
    ) {
        return false;
    }

    // File nonces need uniqueness, not long-term secrecy. Once a production
    // eFuse key exists, esp_fill_random() is sufficient for nonce generation;
    // boot provisioning itself already used guaranteed entropy.
    esp_fill_random(output, 16);
    return true;
}

bool recordingCryptoDeriveFileKeys(
    const uint8_t fileNonce[16],
    uint8_t aesKey[32],
    uint8_t hmacKey[32]
)
{
    if (
        !fileNonce ||
        !aesKey ||
        !hmacKey ||
        !recordingCryptoEnsureProvisioned()
    ) {
        return false;
    }

    return deriveFileKeysFromMaster(
        storageMasterKey,
        fileNonce,
        aesKey,
        hmacKey
    );
}

bool recordingCryptoDeriveFileKeysForKeyId(
    const uint8_t storedKeyId[16],
    const uint8_t fileNonce[16],
    uint8_t aesKey[32],
    uint8_t hmacKey[32]
)
{
    if (
        !storedKeyId ||
        !fileNonce ||
        !aesKey ||
        !hmacKey
    ) {
        return false;
    }

    uint8_t master[32] = {};

    if (!selectMasterForKeyId(storedKeyId, master))
        return false;

    bool ok = deriveFileKeysFromMaster(
        master,
        fileNonce,
        aesKey,
        hmacKey
    );

    secureWipe(master, sizeof(master));
    return ok;
}
