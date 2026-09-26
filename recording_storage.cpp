#include "recording_storage.h"

#include "board_config.h"
#include "recording_crypto.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

namespace {

static const uint8_t FILE_MAGIC[8] = {
    'S', 'F', 'E', 'N', 'C', '1', '\r', '\n'
};

static const uint8_t CHUNK_MAGIC[4] = {
    'S', 'F', 'C', '1'
};

static const uint16_t FORMAT_VERSION = 1;
static const uint16_t ALGORITHM_AES256_CTR_HMAC_SHA256 = 1;
static const uint16_t FLAG_FINALIZED = 0x0001U;

static const uint32_t HEADER_BYTES = 128U;
static const uint32_t HEADER_AUTH_BYTES = 96U;
static const uint32_t HEADER_TAG_BYTES = 32U;

static const uint32_t CHUNK_BYTES = 32U * 1024U;
static const uint32_t CHUNK_HEADER_BYTES = 16U;
static const uint32_t CHUNK_TAG_BYTES = 32U;
static const uint32_t CHUNK_RECORD_BYTES =
    CHUNK_HEADER_BYTES + CHUNK_BYTES + CHUNK_TAG_BYTES;

// PSA multipart cipher implementations are allowed to require output capacity
// beyond the exact logical input length for update/finalize bookkeeping.
// The standalone SensorForge ESP32-S3 benchmark used 64 bytes of spare output
// capacity and passed on Arduino-ESP32 3.3.11 / ESP-IDF 5.5.5. Keep the same
// margin here without changing the on-disk SFENC1 record size.
static const uint32_t CIPHER_OUTPUT_EXTRA_BYTES = 64U;

// Keep hardware-crypto and SD I/O transactions small enough that the full
// SensorForge firmware does not need another 32 KiB temporary DMA allocation.
// This does not change the on-disk chunk size or cryptographic stream.
static const uint32_t CRYPTO_SLICE_BYTES = 4U * 1024U;

// Physical storage timing is sampled only while the explicit Recording Load
// Test brackets one recorderAddFrame() call. It deliberately measures the
// actual Arduino FS File::write()/seek() calls beneath RecordingStorageFile so
// plain and encrypted recordings can be compared on the same I/O boundary.
static const uint32_t DIAG_SLOW_WRITE_US = 20000UL;
static bool storageFrameDiagEnabled = false;
static bool storageFrameDiagActive = false;
static RecordingStorageFrameDiagnostics storageFrameDiag = {};

static uint32_t diagElapsedUs(uint64_t startUs)
{
    uint64_t nowUs = (uint64_t)esp_timer_get_time();
    uint64_t elapsed = nowUs >= startUs ? nowUs - startUs : 0ULL;
    return elapsed > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (uint32_t)elapsed;
}

static size_t storageFileWrite(
    File &file,
    const uint8_t *buffer,
    size_t length
)
{
    if (!storageFrameDiagEnabled || !storageFrameDiagActive)
        return file.write(buffer, length);

    uint64_t startUs = (uint64_t)esp_timer_get_time();
    size_t written = file.write(buffer, length);
    uint32_t elapsedUs = diagElapsedUs(startUs);

    storageFrameDiag.valid = true;
    storageFrameDiag.writeCalls++;
    storageFrameDiag.writeBytes += (uint64_t)written;
    storageFrameDiag.writeTotalUs += (uint64_t)elapsedUs;

    if (elapsedUs > storageFrameDiag.writeMaxUs) {
        storageFrameDiag.writeMaxUs = elapsedUs;
        storageFrameDiag.writeMaxBytes =
            length > 0xFFFFFFFFULL
            ? 0xFFFFFFFFUL
            : (uint32_t)length;
    }

    if (elapsedUs >= DIAG_SLOW_WRITE_US)
        storageFrameDiag.slowWriteCalls++;

    return written;
}

static bool storageFileSeek(
    File &file,
    uint32_t position
)
{
    if (!storageFrameDiagEnabled || !storageFrameDiagActive)
        return file.seek(position);

    uint64_t startUs = (uint64_t)esp_timer_get_time();
    bool ok = file.seek(position);
    uint32_t elapsedUs = diagElapsedUs(startUs);

    storageFrameDiag.valid = true;
    storageFrameDiag.seekCalls++;
    storageFrameDiag.seekTotalUs += (uint64_t)elapsedUs;
    if (elapsedUs > storageFrameDiag.seekMaxUs)
        storageFrameDiag.seekMaxUs = elapsedUs;

    return ok;
}

static uint16_t readU16LE(const uint8_t *p)
{
    return
        (uint16_t)p[0] |
        ((uint16_t)p[1] << 8);
}

static uint32_t readU32LE(const uint8_t *p)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static uint64_t readU64LE(const uint8_t *p)
{
    uint64_t value = 0;

    for (int i = 7; i >= 0; --i) {
        value =
            (value << 8) |
            p[i];
    }

    return value;
}

static void putU16LE(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void putU32LE(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFUL);
    p[1] = (uint8_t)((value >> 8) & 0xFFUL);
    p[2] = (uint8_t)((value >> 16) & 0xFFUL);
    p[3] = (uint8_t)((value >> 24) & 0xFFUL);
}

static void putU64LE(uint8_t *p, uint64_t value)
{
    for (uint8_t i = 0; i < 8; ++i) {
        p[i] = (uint8_t)(value & 0xFFULL);
        value >>= 8;
    }
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

    for (size_t i = 0; i < length; ++i) {
        difference |=
            (uint8_t)(a[i] ^ b[i]);
    }

    return difference == 0;
}

static uint64_t physicalOffsetForChunk(uint32_t chunkIndex)
{
    return
        (uint64_t)HEADER_BYTES +
        (uint64_t)chunkIndex *
            (uint64_t)CHUNK_RECORD_BYTES;
}

static uint32_t expectedPlainLengthForChunk(
    uint64_t logicalSize,
    uint32_t chunkIndex
)
{
    uint64_t start =
        (uint64_t)chunkIndex *
        (uint64_t)CHUNK_BYTES;

    if (start >= logicalSize)
        return 0;

    uint64_t remaining =
        logicalSize - start;

    return
        remaining >= CHUNK_BYTES
        ? CHUNK_BYTES
        : (uint32_t)remaining;
}

static bool importHmacKeyForHeader(
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

static bool computeHeaderMac(
    psa_key_id_t keyId,
    const uint8_t header[HEADER_BYTES],
    uint8_t output[32]
)
{
    size_t outputLength = 0;

    psa_status_t status = psa_mac_compute(
        keyId,
        PSA_ALG_HMAC(PSA_ALG_SHA_256),
        header,
        HEADER_AUTH_BYTES,
        output,
        32,
        &outputLength
    );

    return
        status == PSA_SUCCESS &&
        outputLength == 32;
}

static bool parseHeaderSmall(
    const uint8_t header[HEADER_BYTES],
    uint64_t &logicalSize,
    uint8_t nonce[16],
    uint8_t keyId[16],
    bool &finalized
)
{
    if (memcmp(header, FILE_MAGIC, sizeof(FILE_MAGIC)) != 0)
        return false;

    if (
        readU16LE(header + 8) != FORMAT_VERSION ||
        readU16LE(header + 10) != HEADER_BYTES ||
        readU16LE(header + 12) != ALGORITHM_AES256_CTR_HMAC_SHA256 ||
        readU32LE(header + 16) != CHUNK_BYTES ||
        readU32LE(header + 20) != CHUNK_RECORD_BYTES
    ) {
        return false;
    }

    uint16_t flags =
        readU16LE(header + 14);

    finalized =
        (flags & FLAG_FINALIZED) != 0;

    logicalSize =
        readU64LE(header + 24);

    memcpy(nonce, header + 32, 16);
    memcpy(keyId, header + 48, 16);

    return true;
}

static void logStorageError(
    const String &path,
    const char *operation
)
{
    Serial.printf(
        "RecordingStorage ERROR | %s | %s\n",
        path.length() ? path.c_str() : "<no-path>",
        operation ? operation : "unknown"
    );
}

static const char *psaStatusName(psa_status_t status)
{
    switch (status) {
        case PSA_SUCCESS:                   return "PSA_SUCCESS";
        case PSA_ERROR_NOT_SUPPORTED:       return "PSA_ERROR_NOT_SUPPORTED";
        case PSA_ERROR_INVALID_ARGUMENT:    return "PSA_ERROR_INVALID_ARGUMENT";
        case PSA_ERROR_BUFFER_TOO_SMALL:    return "PSA_ERROR_BUFFER_TOO_SMALL";
        case PSA_ERROR_INSUFFICIENT_MEMORY: return "PSA_ERROR_INSUFFICIENT_MEMORY";
        case PSA_ERROR_BAD_STATE:           return "PSA_ERROR_BAD_STATE";
        case PSA_ERROR_NOT_PERMITTED:       return "PSA_ERROR_NOT_PERMITTED";
        case PSA_ERROR_CORRUPTION_DETECTED: return "PSA_ERROR_CORRUPTION_DETECTED";
        default:                            return "PSA_ERROR_OTHER";
    }
}

static void logPsaCipherError(
    const String &path,
    const char *stage,
    psa_status_t status
)
{
    Serial.printf(
        "RecordingStorage PSA ERROR | %s | stage=%s | status=%ld | %s\n",
        path.length() ? path.c_str() : "<no-path>",
        stage ? stage : "unknown",
        (long)status,
        psaStatusName(status)
    );
}

static bool verifyEncryptedHeaderSmall(
    const uint8_t header[HEADER_BYTES],
    uint64_t &logicalSize
)
{
    uint8_t nonce[16] = {};
    uint8_t storedKeyId[16] = {};
    bool finalized = false;

    if (!parseHeaderSmall(
            header,
            logicalSize,
            nonce,
            storedKeyId,
            finalized
        )) {
        return false;
    }

    if (!finalized)
        return false;

    if (!recordingCryptoKeyIdSupported(storedKeyId))
        return false;

    uint8_t aesKey[32] = {};
    uint8_t hmacKey[32] = {};

    if (!recordingCryptoDeriveFileKeysForKeyId(
            storedKeyId,
            nonce,
            aesKey,
            hmacKey
        )) {
        return false;
    }

    memset(aesKey, 0, sizeof(aesKey));

    psa_key_id_t hmacKeyId = 0;

    if (!importHmacKeyForHeader(
            hmacKey,
            hmacKeyId
        )) {
        memset(hmacKey, 0, sizeof(hmacKey));
        return false;
    }

    memset(hmacKey, 0, sizeof(hmacKey));

    uint8_t calculated[32] = {};

    bool ok =
        computeHeaderMac(
            hmacKeyId,
            header,
            calculated
        ) &&
        secureEqual(
            calculated,
            header + HEADER_AUTH_BYTES,
            HEADER_TAG_BYTES
        );

    psa_destroy_key(hmacKeyId);
    memset(calculated, 0, sizeof(calculated));

    return ok;
}

} // namespace

void recordingStorageSetFrameDiagnosticsEnabled(bool enabled)
{
    storageFrameDiagEnabled = enabled;
    storageFrameDiagActive = false;
    storageFrameDiag = {};
}

void recordingStorageBeginFrameDiagnostics()
{
    storageFrameDiag = {};
    storageFrameDiagActive = storageFrameDiagEnabled;
    storageFrameDiag.valid = storageFrameDiagActive;
}

bool recordingStorageGetFrameDiagnostics(
    RecordingStorageFrameDiagnostics &diagnostics
)
{
    diagnostics = storageFrameDiag;
    bool valid = storageFrameDiagEnabled && storageFrameDiag.valid;
    storageFrameDiagActive = false;
    return valid;
}

RecordingStorageFile::RecordingStorageFile()
{
}

RecordingStorageFile::~RecordingStorageFile()
{
    close();
}

void RecordingStorageFile::clearLastError()
{
    lastError_[0] = '\0';
}

void RecordingStorageFile::setLastError(const char *format, ...)
{
    if (!format) {
        lastError_[0] = '\0';
        return;
    }

    va_list args;
    va_start(args, format);
    vsnprintf(lastError_, sizeof(lastError_), format, args);
    va_end(args);

    Serial.printf(
        "RecordingStorage ERROR | %s | %s\n",
        path_.length() ? path_.c_str() : "<no-path>",
        lastError_
    );
}

bool RecordingStorageFile::allocateCryptoBuffers()
{
    if (
        plainChunk_ &&
        physicalChunk_ &&
        cryptoInput_ &&
        cryptoOutput_
    ) {
        return true;
    }

    // Large caches are not passed directly to the AES/SHA hardware. Prefer
    // PSRAM so camera, WiFi, WebConfig and the crypto DMA engine keep their
    // scarce internal/DMA heap. Boards without PSRAM fall back to internal RAM.
    bool chunkBuffersInPsram = false;

    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        plainChunk_ =
            (uint8_t *)heap_caps_malloc(
                CHUNK_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );

        physicalChunk_ =
            (uint8_t *)heap_caps_malloc(
                CHUNK_RECORD_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );

        chunkBuffersInPsram =
            plainChunk_ && physicalChunk_;

        if (!chunkBuffersInPsram) {
            if (plainChunk_) {
                heap_caps_free(plainChunk_);
                plainChunk_ = nullptr;
            }

            if (physicalChunk_) {
                heap_caps_free(physicalChunk_);
                physicalChunk_ = nullptr;
            }
        }
    }

    if (!plainChunk_) {
        plainChunk_ =
            (uint8_t *)heap_caps_malloc(
                CHUNK_BYTES,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
            );
    }

    if (!physicalChunk_) {
        physicalChunk_ =
            (uint8_t *)heap_caps_malloc(
                CHUNK_RECORD_BYTES,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
            );
    }

    // These are the only buffers handed directly to PSA hardware crypto and
    // to chunked physical I/O, therefore make them explicitly DMA capable.
    cryptoInput_ =
        (uint8_t *)heap_caps_malloc(
            CRYPTO_SLICE_BYTES,
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_DMA |
            MALLOC_CAP_8BIT
        );

    cryptoOutput_ =
        (uint8_t *)heap_caps_malloc(
            CRYPTO_SLICE_BYTES + CIPHER_OUTPUT_EXTRA_BYTES,
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_DMA |
            MALLOC_CAP_8BIT
        );

    if (
        !plainChunk_ ||
        !physicalChunk_ ||
        !cryptoInput_ ||
        !cryptoOutput_
    ) {
        setLastError(
            "crypto buffer allocation failed | internal_free=%u | dma_free=%u | dma_largest=%u | psram_free=%u | psram_largest=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        );

        releaseCryptoBuffers();
        return false;
    }

    static bool memoryLayoutReported = false;

    if (!memoryLayoutReported) {
        memoryLayoutReported = true;

        Serial.printf(
            "RecordingStorage MEMORY | chunk_buffers=%s | crypto_slice=%u | internal_free=%u | dma_free=%u | dma_largest=%u | psram_free=%u\n",
            chunkBuffersInPsram ? "psram" : "internal",
            (unsigned)CRYPTO_SLICE_BYTES,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        );
    }

    return true;
}

void RecordingStorageFile::releaseCryptoBuffers()
{
    if (plainChunk_) {
        memset(plainChunk_, 0, CHUNK_BYTES);
        heap_caps_free(plainChunk_);
        plainChunk_ = nullptr;
    }

    if (physicalChunk_) {
        memset(physicalChunk_, 0, CHUNK_RECORD_BYTES);
        heap_caps_free(physicalChunk_);
        physicalChunk_ = nullptr;
    }

    if (cryptoInput_) {
        memset(cryptoInput_, 0, CRYPTO_SLICE_BYTES);
        heap_caps_free(cryptoInput_);
        cryptoInput_ = nullptr;
    }

    if (cryptoOutput_) {
        memset(
            cryptoOutput_,
            0,
            CRYPTO_SLICE_BYTES + CIPHER_OUTPUT_EXTRA_BYTES
        );
        heap_caps_free(cryptoOutput_);
        cryptoOutput_ = nullptr;
    }
}

bool RecordingStorageFile::importFileKeys()
{
    if (aesKeyId_ || hmacKeyId_)
        return true;

    uint8_t aesKey[32] = {};
    uint8_t hmacKey[32] = {};

    if (!recordingCryptoDeriveFileKeysForKeyId(
            keyId_,
            fileNonce_,
            aesKey,
            hmacKey
        )) {
        return false;
    }

    psa_key_attributes_t aesAttributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&aesAttributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&aesAttributes, 256);
    psa_set_key_algorithm(&aesAttributes, PSA_ALG_CTR);
    psa_set_key_usage_flags(
        &aesAttributes,
        (psa_key_usage_t)(
            PSA_KEY_USAGE_ENCRYPT |
            PSA_KEY_USAGE_DECRYPT
        )
    );

    psa_status_t aesStatus = psa_import_key(
        &aesAttributes,
        aesKey,
        sizeof(aesKey),
        &aesKeyId_
    );

    psa_reset_key_attributes(&aesAttributes);
    memset(aesKey, 0, sizeof(aesKey));

    if (aesStatus != PSA_SUCCESS || aesKeyId_ == 0) {
        memset(hmacKey, 0, sizeof(hmacKey));
        aesKeyId_ = 0;
        return false;
    }

    psa_key_attributes_t hmacAttributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&hmacAttributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&hmacAttributes, 256);
    psa_set_key_algorithm(
        &hmacAttributes,
        PSA_ALG_HMAC(PSA_ALG_SHA_256)
    );
    psa_set_key_usage_flags(
        &hmacAttributes,
        PSA_KEY_USAGE_SIGN_MESSAGE
    );

    psa_status_t hmacStatus = psa_import_key(
        &hmacAttributes,
        hmacKey,
        sizeof(hmacKey),
        &hmacKeyId_
    );

    psa_reset_key_attributes(&hmacAttributes);
    memset(hmacKey, 0, sizeof(hmacKey));

    if (hmacStatus != PSA_SUCCESS || hmacKeyId_ == 0) {
        psa_destroy_key(aesKeyId_);
        aesKeyId_ = 0;
        hmacKeyId_ = 0;
        return false;
    }

    return true;
}

void RecordingStorageFile::destroyFileKeys()
{
    if (aesKeyId_) {
        psa_destroy_key(aesKeyId_);
        aesKeyId_ = 0;
    }

    if (hmacKeyId_) {
        psa_destroy_key(hmacKeyId_);
        hmacKeyId_ = 0;
    }
}

bool RecordingStorageFile::computeMac(
    const uint8_t *data,
    size_t length,
    uint8_t output[32]
) const
{
    if (
        !hmacKeyId_ ||
        !data ||
        !output ||
        !cryptoInput_
    ) {
        return false;
    }

    psa_mac_operation_t operation = PSA_MAC_OPERATION_INIT;

    psa_status_t status =
        psa_mac_sign_setup(
            &operation,
            hmacKeyId_,
            PSA_ALG_HMAC(PSA_ALG_SHA_256)
        );

    if (status != PSA_SUCCESS) {
        logPsaCipherError(path_, "hmac_setup", status);
        psa_mac_abort(&operation);
        return false;
    }

    size_t offset = 0;

    while (offset < length) {
        size_t take = length - offset;

        if (take > CRYPTO_SLICE_BYTES)
            take = CRYPTO_SLICE_BYTES;

        memcpy(
            cryptoInput_,
            data + offset,
            take
        );

        status = psa_mac_update(
            &operation,
            cryptoInput_,
            take
        );

        if (status != PSA_SUCCESS) {
            logPsaCipherError(path_, "hmac_update", status);
            psa_mac_abort(&operation);
            return false;
        }

        offset += take;
    }

    size_t outputLength = 0;

    status = psa_mac_sign_finish(
        &operation,
        output,
        32,
        &outputLength
    );

    if (status != PSA_SUCCESS) {
        logPsaCipherError(path_, "hmac_finish", status);
        psa_mac_abort(&operation);
        return false;
    }

    return outputLength == 32;
}

bool RecordingStorageFile::verifyMac(
    const uint8_t *data,
    size_t length,
    const uint8_t expected[32]
) const
{
    uint8_t calculated[32] = {};

    bool ok =
        computeMac(
            data,
            length,
            calculated
        ) &&
        secureEqual(
            calculated,
            expected,
            32
        );

    memset(calculated, 0, sizeof(calculated));
    return ok;
}

bool RecordingStorageFile::cryptChunk(
    bool encrypt,
    uint32_t chunkIndex,
    const uint8_t *input,
    uint8_t *output
)
{
    if (
        !aesKeyId_ ||
        !input ||
        !output ||
        !cryptoInput_ ||
        !cryptoOutput_
    ) {
        return false;
    }

    uint8_t iv[16] = {};

    memcpy(iv, fileNonce_, 8);

    uint64_t value = chunkIndex;

    for (uint8_t i = 0; i < 8; ++i) {
        iv[15 - i] =
            (uint8_t)(value & 0xFFULL);
        value >>= 8;
    }

    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;

    psa_status_t status =
        encrypt
        ? psa_cipher_encrypt_setup(
            &operation,
            aesKeyId_,
            PSA_ALG_CTR
        )
        : psa_cipher_decrypt_setup(
            &operation,
            aesKeyId_,
            PSA_ALG_CTR
        );

    if (status != PSA_SUCCESS) {
        logPsaCipherError(path_, "setup", status);
        psa_cipher_abort(&operation);
        return false;
    }

    status = psa_cipher_set_iv(
        &operation,
        iv,
        sizeof(iv)
    );

    if (status != PSA_SUCCESS) {
        logPsaCipherError(path_, "set_iv", status);
        psa_cipher_abort(&operation);
        return false;
    }

    size_t inputOffset = 0;
    size_t outputOffset = 0;

    while (inputOffset < CHUNK_BYTES) {
        size_t take =
            CHUNK_BYTES - inputOffset;

        if (take > CRYPTO_SLICE_BYTES)
            take = CRYPTO_SLICE_BYTES;

        memcpy(
            cryptoInput_,
            input + inputOffset,
            take
        );

        size_t updateLength = 0;

        status = psa_cipher_update(
            &operation,
            cryptoInput_,
            take,
            cryptoOutput_,
            CRYPTO_SLICE_BYTES + CIPHER_OUTPUT_EXTRA_BYTES,
            &updateLength
        );

        if (status != PSA_SUCCESS) {
            logPsaCipherError(path_, "update", status);
            psa_cipher_abort(&operation);
            return false;
        }

        if (
            outputOffset + updateLength >
            CHUNK_BYTES
        ) {
            logStorageError(path_, "cipher update length invalid");
            psa_cipher_abort(&operation);
            return false;
        }

        memcpy(
            output + outputOffset,
            cryptoOutput_,
            updateLength
        );

        inputOffset += take;
        outputOffset += updateLength;
    }

    size_t finishLength = 0;

    status = psa_cipher_finish(
        &operation,
        cryptoOutput_,
        CRYPTO_SLICE_BYTES + CIPHER_OUTPUT_EXTRA_BYTES,
        &finishLength
    );

    if (status != PSA_SUCCESS) {
        logPsaCipherError(path_, "finish", status);
        psa_cipher_abort(&operation);
        return false;
    }

    if (outputOffset + finishLength > CHUNK_BYTES) {
        logStorageError(path_, "cipher finish length invalid");
        return false;
    }

    if (finishLength > 0) {
        memcpy(
            output + outputOffset,
            cryptoOutput_,
            finishLength
        );

        outputOffset += finishLength;
    }

    if (outputOffset != CHUNK_BYTES) {
        Serial.printf(
            "RecordingStorage ERROR | %s | cipher length mismatch | actual=%u | expected=%u\n",
            path_.length() ? path_.c_str() : "<no-path>",
            (unsigned)outputOffset,
            (unsigned)CHUNK_BYTES
        );
        return false;
    }

    return true;
}

bool RecordingStorageFile::writeHeader(bool finalized)
{
    if (!encrypted_ || !writing_ || !file_)
        return false;

    uint8_t header[HEADER_BYTES] = {};

    memcpy(header, FILE_MAGIC, sizeof(FILE_MAGIC));
    putU16LE(header + 8, FORMAT_VERSION);
    putU16LE(header + 10, HEADER_BYTES);
    putU16LE(
        header + 12,
        ALGORITHM_AES256_CTR_HMAC_SHA256
    );
    putU16LE(
        header + 14,
        finalized
        ? FLAG_FINALIZED
        : 0
    );
    putU32LE(header + 16, CHUNK_BYTES);
    putU32LE(header + 20, CHUNK_RECORD_BYTES);
    putU64LE(header + 24, logicalSize_);
    memcpy(header + 32, fileNonce_, 16);
    memcpy(header + 48, keyId_, 16);

    if (!computeMac(
            header,
            HEADER_AUTH_BYTES,
            header + HEADER_AUTH_BYTES
        )) {
        setLastError("header HMAC failed");
        return false;
    }

    if (!storageFileSeek(file_, 0)) {
        setLastError("header seek failed");
        return false;
    }

    if (storageFileWrite(file_,
            header,
            sizeof(header)
        ) != sizeof(header)) {
        setLastError("header write failed");
        return false;
    }

    return true;
}

bool RecordingStorageFile::readAndValidateHeader()
{
    uint8_t header[HEADER_BYTES] = {};

    if (!storageFileSeek(file_, 0))
        return false;

    if (file_.read(
            header,
            sizeof(header)
        ) != sizeof(header)) {
        return false;
    }

    bool finalized = false;

    if (!parseHeaderSmall(
            header,
            logicalSize_,
            fileNonce_,
            keyId_,
            finalized
        )) {
        return false;
    }

    if (!finalized)
        return false;

    if (!recordingCryptoKeyIdSupported(keyId_))
        return false;

    if (!importFileKeys())
        return false;

    if (!verifyMac(
            header,
            HEADER_AUTH_BYTES,
            header + HEADER_AUTH_BYTES
        )) {
        return false;
    }

    uint64_t chunkCount =
        logicalSize_ == 0
        ? 0
        : (
            logicalSize_ +
            CHUNK_BYTES - 1ULL
        ) / CHUNK_BYTES;

    uint64_t expectedPhysical =
        HEADER_BYTES +
        chunkCount *
            (uint64_t)CHUNK_RECORD_BYTES;

    return
        (uint64_t)file_.size() >=
        expectedPhysical;
}

bool RecordingStorageFile::openEncryptedRead()
{
    encrypted_ = true;
    writing_ = false;

    if (!recordingCryptoBegin())
        return false;

    if (!allocateCryptoBuffers())
        return false;

    if (!readAndValidateHeader())
        return false;

    logicalPosition_ = 0;
    cachedChunkIndex_ = -1;
    cachedPlainLength_ = 0;
    cacheDirty_ = false;

    return true;
}

bool RecordingStorageFile::openEncryptedWrite()
{
    encrypted_ = true;
    writing_ = true;

    if (!recordingCryptoEnsureProvisioned()) {
        setLastError(
            "hardware key unavailable | status=%s",
            recordingCryptoKeyStatusName()
        );
        return false;
    }

    if (!recordingCryptoGenerateFileNonce(fileNonce_)) {
        setLastError("file nonce generation failed");
        return false;
    }

    if (!recordingCryptoGetKeyId(keyId_)) {
        setLastError("storage key id unavailable");
        return false;
    }

    if (!importFileKeys()) {
        setLastError("file key import failed");
        return false;
    }

    if (!allocateCryptoBuffers())
        return false;

    logicalPosition_ = 0;
    logicalSize_ = 0;
    cachedChunkIndex_ = -1;
    cachedPlainLength_ = 0;
    cacheDirty_ = false;

    if (!writeHeader(false))
        return false;

    file_.flush();
    return true;
}

bool RecordingStorageFile::openRead(const String &path)
{
    close();
    clearLastError();

    path_ = path;

    file_ = STORAGE.open(
        path.c_str(),
        FILE_READ
    );

    if (!file_) {
        path_ = "";
        return false;
    }

    directory_ = file_.isDirectory();

    if (directory_) {
        open_ = true;
        return true;
    }

    uint8_t magic[sizeof(FILE_MAGIC)] = {};

    size_t got = file_.read(
        magic,
        sizeof(magic)
    );

    if (!storageFileSeek(file_, 0)) {
        close();
        return false;
    }

    if (
        got == sizeof(magic) &&
        memcmp(
            magic,
            FILE_MAGIC,
            sizeof(FILE_MAGIC)
        ) == 0
    ) {
        if (!openEncryptedRead()) {
            failed_ = true;
            close();
            return false;
        }
    } else {
        encrypted_ = false;
        writing_ = false;
        logicalPosition_ = 0;
        logicalSize_ =
            (uint64_t)file_.size();
    }

    open_ = true;
    return true;
}

bool RecordingStorageFile::openWrite(
    const String &path,
    bool encrypt
)
{
    close();
    clearLastError();

    path_ = path;

    // Encrypted files require read/write access while recording. Container
    // finalization (AVI header patches and MKV size/duration patches) seeks back
    // into already-written logical chunks, verifies/decrypts them, modifies the
    // plaintext bytes, then re-encrypts the chunk in place. Arduino-ESP32
    // FILE_WRITE is "w" (write-only), so encrypted files must use "w+".
    // Plain recordings retain the original FILE_WRITE behavior unchanged.
    file_ = STORAGE.open(
        path.c_str(),
        encrypt ? "w+" : FILE_WRITE
    );

    if (!file_) {
        setLastError("filesystem open for write failed");
        path_ = "";
        return false;
    }

    directory_ = false;
    encrypted_ = encrypt;
    writing_ = true;

    bool ok = true;

    if (encrypt) {
        ok = openEncryptedWrite();
    } else {
        logicalPosition_ = 0;
        logicalSize_ = 0;
    }

    if (!ok) {
        failed_ = true;

        if (file_)
            file_.close();

        STORAGE.remove(path.c_str());

        destroyFileKeys();
        releaseCryptoBuffers();

        path_ = "";
        encrypted_ = false;
        writing_ = false;
        failed_ = false;
        logicalPosition_ = 0;
        logicalSize_ = 0;
        memset(fileNonce_, 0, sizeof(fileNonce_));
        memset(keyId_, 0, sizeof(keyId_));
        cachedChunkIndex_ = -1;
        cachedPlainLength_ = 0;
        cacheDirty_ = false;

        return false;
    }

    open_ = true;
    return true;
}

bool RecordingStorageFile::loadChunk(uint32_t chunkIndex)
{
    if (!encrypted_ || !file_ || !plainChunk_ || !physicalChunk_)
        return false;

    if (cachedChunkIndex_ == (int64_t)chunkIndex)
        return true;

    if (writing_ && !flushCachedChunk())
        return false;

    uint32_t expectedPlain =
        expectedPlainLengthForChunk(
            logicalSize_,
            chunkIndex
        );

    if (expectedPlain == 0)
        return false;

    uint64_t physicalOffset =
        physicalOffsetForChunk(chunkIndex);

    if (physicalOffset > 0xFFFFFFFFULL)
        return false;

    if (!storageFileSeek(file_, (uint32_t)physicalOffset)) {
        setLastError("chunk read seek failed | chunk=%lu | physical_offset=%llu", (unsigned long)chunkIndex, (unsigned long long)physicalOffset);
        return false;
    }

    size_t physicalRead = 0;

    while (physicalRead < CHUNK_RECORD_BYTES) {
        size_t take =
            CHUNK_RECORD_BYTES - physicalRead;

        if (take > CRYPTO_SLICE_BYTES)
            take = CRYPTO_SLICE_BYTES;

        size_t got = file_.read(
            cryptoInput_,
            take
        );

        if (got != take) {
            setLastError("chunk read failed | chunk=%lu | physical_offset=%llu | read=%u | expected=%u", (unsigned long)chunkIndex, (unsigned long long)(physicalOffset + physicalRead), (unsigned)got, (unsigned)take);
            return false;
        }

        memcpy(
            physicalChunk_ + physicalRead,
            cryptoInput_,
            take
        );

        physicalRead += take;
    }

    if (
        memcmp(
            physicalChunk_,
            CHUNK_MAGIC,
            sizeof(CHUNK_MAGIC)
        ) != 0 ||
        readU32LE(physicalChunk_ + 4) != chunkIndex
    ) {
        setLastError("chunk header invalid | chunk=%lu", (unsigned long)chunkIndex);
        return false;
    }

    uint32_t storedPlainLength =
        readU32LE(physicalChunk_ + 8);

    if (
        storedPlainLength != expectedPlain ||
        storedPlainLength > CHUNK_BYTES
    ) {
        setLastError("chunk plaintext length mismatch | chunk=%lu | stored=%lu | expected=%lu", (unsigned long)chunkIndex, (unsigned long)storedPlainLength, (unsigned long)expectedPlain);
        return false;
    }

    const uint8_t *storedTag =
        physicalChunk_ +
        CHUNK_HEADER_BYTES +
        CHUNK_BYTES;

    if (!verifyMac(
            physicalChunk_,
            CHUNK_HEADER_BYTES + CHUNK_BYTES,
            storedTag
        )) {
        setLastError("chunk HMAC verification failed | chunk=%lu", (unsigned long)chunkIndex);
        return false;
    }

    if (!cryptChunk(
            false,
            chunkIndex,
            physicalChunk_ + CHUNK_HEADER_BYTES,
            plainChunk_
        )) {
        setLastError("chunk decrypt failed | chunk=%lu", (unsigned long)chunkIndex);
        return false;
    }

    cachedChunkIndex_ =
        (int64_t)chunkIndex;

    cachedPlainLength_ =
        storedPlainLength;

    cacheDirty_ = false;

    return true;
}

bool RecordingStorageFile::prepareWritableChunk(uint32_t chunkIndex)
{
    if (cachedChunkIndex_ == (int64_t)chunkIndex)
        return true;

    if (!flushCachedChunk())
        return false;

    uint64_t chunkStart =
        (uint64_t)chunkIndex *
        (uint64_t)CHUNK_BYTES;

    if (chunkStart < logicalSize_) {
        return loadChunk(chunkIndex);
    }

    memset(
        plainChunk_,
        0,
        CHUNK_BYTES
    );

    cachedChunkIndex_ =
        (int64_t)chunkIndex;

    cachedPlainLength_ = 0;
    cacheDirty_ = false;

    return true;
}

bool RecordingStorageFile::flushCachedChunk()
{
    if (!encrypted_ || !writing_)
        return true;

    if (!cacheDirty_ || cachedChunkIndex_ < 0)
        return true;

    uint32_t chunkIndex =
        (uint32_t)cachedChunkIndex_;

    memset(
        physicalChunk_,
        0,
        CHUNK_RECORD_BYTES
    );

    memcpy(
        physicalChunk_,
        CHUNK_MAGIC,
        sizeof(CHUNK_MAGIC)
    );

    putU32LE(
        physicalChunk_ + 4,
        chunkIndex
    );

    putU32LE(
        physicalChunk_ + 8,
        cachedPlainLength_
    );

    putU32LE(
        physicalChunk_ + 12,
        0
    );

    if (!cryptChunk(
            true,
            chunkIndex,
            plainChunk_,
            physicalChunk_ + CHUNK_HEADER_BYTES
        )) {
        setLastError("chunk encrypt failed | chunk=%lu", (unsigned long)chunkIndex);
        return false;
    }

    uint8_t *tag =
        physicalChunk_ +
        CHUNK_HEADER_BYTES +
        CHUNK_BYTES;

    if (!computeMac(
            physicalChunk_,
            CHUNK_HEADER_BYTES + CHUNK_BYTES,
            tag
        )) {
        setLastError("chunk HMAC generation failed | chunk=%lu", (unsigned long)chunkIndex);
        return false;
    }

    uint64_t physicalOffset =
        physicalOffsetForChunk(chunkIndex);

    if (physicalOffset > 0xFFFFFFFFULL) {
        setLastError(
            "chunk physical offset exceeds 32-bit seek range | chunk=%lu | physical_offset=%llu",
            (unsigned long)chunkIndex,
            (unsigned long long)physicalOffset
        );
        return false;
    }

    // Normal recording reaches chunks in strict append order. Avoid an
    // unnecessary fseek() when the stdio stream is already positioned at the
    // exact physical chunk offset. Random-access container patches still take
    // the seek path below, so SFENC1 semantics remain unchanged.
    size_t currentPhysicalPosition =
        file_.position();

    if (
        currentPhysicalPosition !=
        (size_t)physicalOffset
    ) {
        errno = 0;

        if (!storageFileSeek(file_, (uint32_t)physicalOffset)) {
            int seekErrno = errno;

            setLastError(
                "chunk write seek failed | chunk=%lu | offset=%llu | current=%u | errno=%d:%s",
                (unsigned long)chunkIndex,
                (unsigned long long)physicalOffset,
                (unsigned)currentPhysicalPosition,
                seekErrno,
                strerror(seekErrno)
            );
            return false;
        }
    }

    size_t physicalWritten = 0;

    while (physicalWritten < CHUNK_RECORD_BYTES) {
        size_t take =
            CHUNK_RECORD_BYTES - physicalWritten;

        if (take > CRYPTO_SLICE_BYTES)
            take = CRYPTO_SLICE_BYTES;

        memcpy(
            cryptoInput_,
            physicalChunk_ + physicalWritten,
            take
        );

        size_t positionBefore =
            file_.position();

        errno = 0;

        size_t written = storageFileWrite(file_,
            cryptoInput_,
            take
        );

        int writeErrno = errno;

        if (written != take) {
            size_t positionAfter =
                file_.position();
            size_t physicalSize =
                file_.size();

            setLastError(
                "chunk physical write failed | chunk=%lu | offset=%llu | written=%u/%u | errno=%d:%s | pos=%u->%u | size=%u",
                (unsigned long)chunkIndex,
                (unsigned long long)(physicalOffset + physicalWritten),
                (unsigned)written,
                (unsigned)take,
                writeErrno,
                strerror(writeErrno),
                (unsigned)positionBefore,
                (unsigned)positionAfter,
                (unsigned)physicalSize
            );
            return false;
        }

        physicalWritten += take;
    }

    cacheDirty_ = false;
    return true;
}

size_t RecordingStorageFile::read(
    uint8_t *buffer,
    size_t length
)
{
    if (
        !open_ ||
        failed_ ||
        writing_ ||
        directory_ ||
        !buffer ||
        length == 0
    ) {
        return 0;
    }

    if (!encrypted_) {
        size_t got =
            file_.read(buffer, length);

        logicalPosition_ +=
            (uint64_t)got;

        return got;
    }

    if (logicalPosition_ >= logicalSize_)
        return 0;

    size_t totalRead = 0;

    while (
        totalRead < length &&
        logicalPosition_ < logicalSize_
    ) {
        uint32_t chunkIndex =
            (uint32_t)(
                logicalPosition_ /
                CHUNK_BYTES
            );

        uint32_t chunkOffset =
            (uint32_t)(
                logicalPosition_ %
                CHUNK_BYTES
            );

        if (!loadChunk(chunkIndex)) {
            failed_ = true;
            break;
        }

        if (chunkOffset >= cachedPlainLength_) {
            failed_ = true;
            break;
        }

        size_t availableInChunk =
            cachedPlainLength_ -
            chunkOffset;

        size_t wanted =
            length - totalRead;

        if (wanted > availableInChunk)
            wanted = availableInChunk;

        uint64_t remainingLogical =
            logicalSize_ -
            logicalPosition_;

        if ((uint64_t)wanted > remainingLogical)
            wanted = (size_t)remainingLogical;

        memcpy(
            buffer + totalRead,
            plainChunk_ + chunkOffset,
            wanted
        );

        totalRead += wanted;
        logicalPosition_ += wanted;
    }

    return totalRead;
}

int RecordingStorageFile::read()
{
    uint8_t value = 0;

    return
        read(&value, 1) == 1
        ? (int)value
        : -1;
}

size_t RecordingStorageFile::write(
    const uint8_t *buffer,
    size_t length
)
{
    if (
        !open_ ||
        failed_ ||
        !writing_ ||
        directory_ ||
        !buffer ||
        length == 0
    ) {
        return 0;
    }

    if (!encrypted_) {
        size_t written =
            storageFileWrite(file_, buffer, length);

        logicalPosition_ +=
            (uint64_t)written;

        if (logicalPosition_ > logicalSize_)
            logicalSize_ = logicalPosition_;

        if (written != length) {
            setLastError(
                "plain physical write failed | written=%u | expected=%u | logical_offset=%llu",
                (unsigned)written,
                (unsigned)length,
                (unsigned long long)(logicalPosition_ - written)
            );
            failed_ = true;
        }

        return written;
    }

    size_t totalWritten = 0;

    while (totalWritten < length) {
        if (logicalPosition_ > 0xFFFFFFFFULL) {
            failed_ = true;
            break;
        }

        uint32_t chunkIndex =
            (uint32_t)(
                logicalPosition_ /
                CHUNK_BYTES
            );

        uint32_t chunkOffset =
            (uint32_t)(
                logicalPosition_ %
                CHUNK_BYTES
            );

        if (!prepareWritableChunk(chunkIndex)) {
            failed_ = true;
            break;
        }

        size_t room =
            CHUNK_BYTES -
            chunkOffset;

        size_t wanted =
            length - totalWritten;

        if (wanted > room)
            wanted = room;

        memcpy(
            plainChunk_ + chunkOffset,
            buffer + totalWritten,
            wanted
        );

        uint32_t endOffset =
            chunkOffset +
            (uint32_t)wanted;

        if (endOffset > cachedPlainLength_)
            cachedPlainLength_ = endOffset;

        logicalPosition_ += wanted;
        totalWritten += wanted;
        cacheDirty_ = true;

        if (logicalPosition_ > logicalSize_)
            logicalSize_ = logicalPosition_;

        if (endOffset == CHUNK_BYTES) {
            if (!flushCachedChunk()) {
                failed_ = true;
                break;
            }

            cachedChunkIndex_ = -1;
            cachedPlainLength_ = 0;
            cacheDirty_ = false;
        }
    }

    return totalWritten;
}

bool RecordingStorageFile::seek(uint32_t position)
{
    if (!open_ || failed_ || directory_)
        return false;

    if ((uint64_t)position > logicalSize_)
        return false;

    if (!encrypted_) {
        if (!storageFileSeek(file_, position))
            return false;

        logicalPosition_ = position;
        return true;
    }

    if (
        writing_ &&
        cachedChunkIndex_ >= 0
    ) {
        uint32_t targetChunk =
            position == logicalSize_ &&
            position % CHUNK_BYTES == 0
            ? UINT32_MAX
            : position / CHUNK_BYTES;

        if (
            targetChunk != UINT32_MAX &&
            cachedChunkIndex_ !=
                (int64_t)targetChunk
        ) {
            if (!flushCachedChunk()) {
                failed_ = true;
                return false;
            }

            cachedChunkIndex_ = -1;
            cachedPlainLength_ = 0;
            cacheDirty_ = false;
        }
    }

    logicalPosition_ = position;
    return true;
}

size_t RecordingStorageFile::position() const
{
    return
        logicalPosition_ > SIZE_MAX
        ? SIZE_MAX
        : (size_t)logicalPosition_;
}

size_t RecordingStorageFile::size() const
{
    return
        logicalSize_ > SIZE_MAX
        ? SIZE_MAX
        : (size_t)logicalSize_;
}

int RecordingStorageFile::available() const
{
    if (
        !open_ ||
        failed_ ||
        writing_ ||
        directory_ ||
        logicalPosition_ >= logicalSize_
    ) {
        return 0;
    }

    uint64_t remaining =
        logicalSize_ -
        logicalPosition_;

    return
        remaining > (uint64_t)INT_MAX
        ? INT_MAX
        : (int)remaining;
}

void RecordingStorageFile::flush()
{
    if (!open_ || failed_ || directory_)
        return;

    if (encrypted_ && writing_) {
        if (!flushCachedChunk()) {
            failed_ = true;
            return;
        }

        if (!writeHeader(false)) {
            failed_ = true;
            return;
        }
    }

    file_.flush();
}

bool RecordingStorageFile::closeChecked()
{
    bool ok = !failed_;

    if (file_) {
        if (
            open_ &&
            encrypted_ &&
            writing_ &&
            !failed_
        ) {
            if (!flushCachedChunk()) {
                failed_ = true;
            }

            if (!failed_ && !writeHeader(true)) {
                failed_ = true;
            }

            file_.flush();
        }

        file_.close();
    }

    ok = ok && !failed_;

    destroyFileKeys();
    releaseCryptoBuffers();

    path_ = "";
    open_ = false;
    directory_ = false;
    encrypted_ = false;
    writing_ = false;
    failed_ = false;
    logicalPosition_ = 0;
    logicalSize_ = 0;
    memset(fileNonce_, 0, sizeof(fileNonce_));
    memset(keyId_, 0, sizeof(keyId_));
    cachedChunkIndex_ = -1;
    cachedPlainLength_ = 0;
    cacheDirty_ = false;

    return ok;
}

void RecordingStorageFile::close()
{
    (void)closeChecked();
}

bool RecordingStorageFile::isOpen() const
{
    return open_ && (bool)file_;
}

bool RecordingStorageFile::isDirectory() const
{
    return directory_;
}

bool RecordingStorageFile::isEncrypted() const
{
    return encrypted_;
}

bool RecordingStorageFile::failed() const
{
    return failed_;
}

const char *RecordingStorageFile::lastError() const
{
    return lastError_;
}

String RecordingStorageFile::path() const
{
    return path_;
}

RecordingStorageFile::operator bool() const
{
    return
        open_ &&
        !failed_ &&
        (bool)file_;
}

bool recordingStorageLogicalSize(
    const String &path,
    uint64_t &logicalSize,
    bool *encrypted
)
{
    logicalSize = 0;

    if (encrypted)
        *encrypted = false;

    File file = STORAGE.open(
        path.c_str(),
        FILE_READ
    );

    if (!file || file.isDirectory()) {
        if (file)
            file.close();
        return false;
    }

    uint64_t physicalSize =
        (uint64_t)file.size();

    if (physicalSize < sizeof(FILE_MAGIC)) {
        logicalSize = physicalSize;
        file.close();
        return true;
    }

    uint8_t magic[sizeof(FILE_MAGIC)] = {};

    if (!file.seek(0) ||
        file.read(magic, sizeof(magic)) != sizeof(magic)) {
        file.close();
        return false;
    }

    if (memcmp(magic, FILE_MAGIC, sizeof(FILE_MAGIC)) != 0) {
        logicalSize = physicalSize;
        file.close();
        return true;
    }

    if (encrypted)
        *encrypted = true;

    if (physicalSize < HEADER_BYTES) {
        file.close();
        return false;
    }

    uint8_t header[HEADER_BYTES] = {};

    if (!file.seek(0) ||
        file.read(header, sizeof(header)) != sizeof(header)) {
        file.close();
        return false;
    }

    file.close();

    if (!verifyEncryptedHeaderSmall(
            header,
            logicalSize
        )) {
        return false;
    }

    uint64_t chunkCount =
        logicalSize == 0
        ? 0
        : (
            logicalSize +
            CHUNK_BYTES - 1ULL
        ) / CHUNK_BYTES;

    uint64_t expectedPhysical =
        HEADER_BYTES +
        chunkCount *
            (uint64_t)CHUNK_RECORD_BYTES;

    return physicalSize >= expectedPhysical;
}

bool recordingStorageIsEncrypted(
    const String &path,
    bool &encrypted
)
{
    uint64_t ignoredSize = 0;

    return recordingStorageLogicalSize(
        path,
        ignoredSize,
        &encrypted
    );
}
