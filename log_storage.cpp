#include "log_storage.h"

#include "board_config.h"
#include "recording_crypto.h"

#include <esp_system.h>
#include <string.h>

namespace {

static const uint8_t SFLOG_FILE_MAGIC[8] = {
    'S','F','L','O','G','1','\r','\n'
};
static const uint8_t SFLOG_RECORD_MAGIC[8] = {
    'S','F','L','O','G','R','1','\n'
};

static const uint8_t SFLOG_VERSION = 1;
static const uint16_t SFLOG_HEADER_BYTES = 48;
static const size_t SFLOG_FILE_NONCE_BYTES = 16;
static const size_t SFLOG_RECORD_NONCE_BYTES = 12;
static const size_t SFLOG_TAG_BYTES = 16;
static const size_t SFLOG_RECORD_HEADER_BYTES = 16;
static const size_t SFLOG_MAX_RECORD_PLAINTEXT = 1024;
static const size_t SFLOG_MAX_RECORD_TOTAL =
    SFLOG_RECORD_HEADER_BYTES +
    SFLOG_MAX_RECORD_PLAINTEXT +
    SFLOG_TAG_BYTES;

static const char SFLOG_AAD_DOMAIN[] =
    "SENSORFORGE-SFLOG1-RECORD-V1";

static void secureWipe(void *pointer, size_t length)
{
    if (!pointer)
        return;

    volatile uint8_t *p =
        static_cast<volatile uint8_t *>(pointer);

    while (length--)
        *p++ = 0;
}

static void writeU16Le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void writeU32Le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFU);
    out[1] = (uint8_t)((value >> 8) & 0xFFU);
    out[2] = (uint8_t)((value >> 16) & 0xFFU);
    out[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void writeU64Le(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        out[i] = (uint8_t)(value & 0xFFU);
        value >>= 8;
    }
}

static bool deriveRecordNonce(
    const uint8_t fileNonce[SFLOG_FILE_NONCE_BYTES],
    uint64_t recordOffset,
    uint8_t nonce[SFLOG_RECORD_NONCE_BYTES]
)
{
    uint8_t input[SFLOG_FILE_NONCE_BYTES + 8] = {};
    memcpy(input, fileNonce, SFLOG_FILE_NONCE_BYTES);
    writeU64Le(input + SFLOG_FILE_NONCE_BYTES, recordOffset);

    uint8_t digest[32] = {};
    size_t digestLength = 0;

    psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256,
        input,
        sizeof(input),
        digest,
        sizeof(digest),
        &digestLength
    );

    secureWipe(input, sizeof(input));

    if (
        status != PSA_SUCCESS ||
        digestLength < SFLOG_RECORD_NONCE_BYTES
    ) {
        secureWipe(digest, sizeof(digest));
        return false;
    }

    memcpy(nonce, digest, SFLOG_RECORD_NONCE_BYTES);
    secureWipe(digest, sizeof(digest));
    return true;
}

static uint16_t readU16Le(const uint8_t *in)
{
    return
        (uint16_t)in[0] |
        ((uint16_t)in[1] << 8);
}

static uint32_t readU32Le(const uint8_t *in)
{
    return
        (uint32_t)in[0] |
        ((uint32_t)in[1] << 8) |
        ((uint32_t)in[2] << 16) |
        ((uint32_t)in[3] << 24);
}

static bool writeAll(
    File &file,
    const uint8_t *data,
    size_t length
)
{
    size_t offset = 0;

    while (offset < length) {
        size_t written =
            file.write(
                data + offset,
                length - offset
            );

        if (written == 0)
            return false;

        offset += written;
    }

    return true;
}

static bool readAll(
    File &file,
    uint8_t *data,
    size_t length
)
{
    size_t offset = 0;

    while (offset < length) {
        size_t got =
            file.read(
                data + offset,
                length - offset
            );

        if (got == 0)
            return false;

        offset += got;
    }

    return true;
}

static bool importLogKey(psa_key_id_t &keyId)
{
    keyId = 0;

    uint8_t key[32] = {};

    if (!recordingCryptoDeriveLogKey(key)) {
        secureWipe(key, sizeof(key));
        return false;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_usage_flags(
        &attributes,
        PSA_KEY_USAGE_ENCRYPT |
        PSA_KEY_USAGE_DECRYPT
    );

    psa_status_t status =
        psa_import_key(
            &attributes,
            key,
            sizeof(key),
            &keyId
        );

    psa_reset_key_attributes(&attributes);
    secureWipe(key, sizeof(key));

    return
        status == PSA_SUCCESS &&
        keyId != 0;
}

static String hexBytes(
    const uint8_t *data,
    size_t length
)
{
    static const char HEX_CHARS[] = "0123456789abcdef";
    String out;
    out.reserve(length * 2U);

    for (size_t i = 0; i < length; ++i) {
        out += HEX_CHARS[(data[i] >> 4) & 0x0F];
        out += HEX_CHARS[data[i] & 0x0F];
    }

    return out;
}

static bool readHeader(
    File &file,
    bool &encrypted,
    uint8_t fileNonce[SFLOG_FILE_NONCE_BYTES],
    String &generation,
    String &error
)
{
    encrypted = false;
    generation = "";
    error = "";
    memset(fileNonce, 0, SFLOG_FILE_NONCE_BYTES);

    uint64_t size = file.size();

    if (size == 0)
        return true;

    if (size < sizeof(SFLOG_FILE_MAGIC)) {
        // A very small legacy plaintext generation is still valid plaintext.
        if (!file.seek(0)) {
            error = "cannot rewind plaintext log";
            return false;
        }
        return true;
    }

    uint8_t magic[8] = {};

    if (!file.seek(0) || !readAll(file, magic, sizeof(magic))) {
        error = "cannot read log header";
        return false;
    }

    if (memcmp(magic, SFLOG_FILE_MAGIC, sizeof(magic)) != 0) {
        // Legacy plaintext generation.
        if (!file.seek(0)) {
            error = "cannot rewind plaintext log";
            return false;
        }
        return true;
    }

    if (size < SFLOG_HEADER_BYTES) {
        error = "truncated SFLOG1 header";
        return false;
    }

    uint8_t header[SFLOG_HEADER_BYTES] = {};

    if (!file.seek(0) || !readAll(file, header, sizeof(header))) {
        error = "cannot read SFLOG1 header";
        return false;
    }

    if (
        memcmp(header, SFLOG_FILE_MAGIC, sizeof(SFLOG_FILE_MAGIC)) != 0 ||
        header[8] != SFLOG_VERSION ||
        readU16Le(header + 10) != SFLOG_HEADER_BYTES
    ) {
        error = "unsupported SFLOG1 header";
        return false;
    }

    memcpy(
        fileNonce,
        header + 12,
        SFLOG_FILE_NONCE_BYTES
    );

    uint8_t currentKeyId[16] = {};

    if (!recordingCryptoGetKeyId(currentKeyId)) {
        secureWipe(currentKeyId, sizeof(currentKeyId));
        error = "SFLOG1 hardware key unavailable";
        return false;
    }

    if (memcmp(currentKeyId, header + 28, 16) != 0) {
        secureWipe(currentKeyId, sizeof(currentKeyId));
        error = "SFLOG1 belongs to a different hardware key";
        return false;
    }

    secureWipe(currentKeyId, sizeof(currentKeyId));

    encrypted = true;
    generation = "SFLOG1-" + hexBytes(fileNonce, 8);
    return true;
}

static bool createEncryptedFile(
    const String &path,
    String &error
)
{
    error = "";

    if (!recordingCryptoReady()) {
        error =
            "hardware log key unavailable: " +
            String(recordingCryptoKeyStatusName());
        return false;
    }

    File file = STORAGE.open(path.c_str(), FILE_WRITE);

    if (!file) {
        error = "cannot create SFLOG1 file: " + path;
        return false;
    }

    uint8_t header[SFLOG_HEADER_BYTES] = {};
    memcpy(header, SFLOG_FILE_MAGIC, sizeof(SFLOG_FILE_MAGIC));
    header[8] = SFLOG_VERSION;
    header[9] = 0;
    writeU16Le(header + 10, SFLOG_HEADER_BYTES);

    // Same random-source policy already used for SFENC1 file nonces. The file
    // nonce is public; uniqueness is what matters. Per-record GCM nonces are
    // derived deterministically from this 128-bit file nonce and the physical
    // record offset, so no fresh RNG call is required for every log line.
    esp_fill_random(
        header + 12,
        SFLOG_FILE_NONCE_BYTES
    );

    uint8_t keyId[16] = {};

    if (!recordingCryptoGetKeyId(keyId)) {
        secureWipe(keyId, sizeof(keyId));
        secureWipe(header, sizeof(header));
        file.close();
        STORAGE.remove(path.c_str());
        error = "cannot obtain SFLOG1 hardware key id";
        return false;
    }

    memcpy(header + 28, keyId, sizeof(keyId));
    secureWipe(keyId, sizeof(keyId));

    bool ok = writeAll(file, header, sizeof(header));
    file.flush();
    file.close();
    secureWipe(header, sizeof(header));

    if (!ok) {
        error = "cannot write SFLOG1 header: " + path;
        return false;
    }

    return true;
}

static bool repairEncryptedTerminalTailBeforeAppend(
    LogStorageWriter &writer,
    String &error
);


static bool openEncryptedWriterDirect(
    const String &path,
    LogStorageWriter &writer,
    String &error
)
{
    writer = LogStorageWriter();
    error = "";

    File probe = STORAGE.open(path.c_str(), FILE_READ);

    if (!probe) {
        if (!createEncryptedFile(path, error))
            return false;

        probe = STORAGE.open(path.c_str(), FILE_READ);

        if (!probe) {
            error = "cannot reopen SFLOG1 header: " + path;
            return false;
        }
    }

    bool encrypted = false;
    String generation;

    if (!readHeader(
            probe,
            encrypted,
            writer.fileNonce,
            generation,
            error
        )) {
        probe.close();
        return false;
    }

    probe.close();

    if (!encrypted) {
        error = "log generation is not SFLOG1: " + path;
        return false;
    }

    if (!importLogKey(writer.keyId)) {
        error =
            "cannot import hardware log key: " +
            String(recordingCryptoKeyStatusName());
        return false;
    }

    writer.path = path;
    writer.encrypted = true;

    // A power loss can leave the final SFLOG1 record physically incomplete or
    // authentication-damaged before the firmware gets a chance to execute the
    // runtime rollback path. Repair only such terminal damage before appending
    // new records. Older mid-file recovery gaps that already have later valid
    // records are preserved for the reader instead of discarding good history.
    String repairError;

    if (!repairEncryptedTerminalTailBeforeAppend(
            writer,
            repairError
        )) {
        psa_destroy_key(writer.keyId);
        writer.keyId = 0;
        writer.path = "";
        writer.encrypted = false;
        error =
            "cannot repair SFLOG1 terminal tail before append: " +
            repairError;
        return false;
    }

    // A successful terminal-tail repair already reopens the writer at the
    // authenticated boundary. Otherwise open the untouched generation now.
    if (!writer.file) {
        writer.file = STORAGE.open(path.c_str(), FILE_APPEND);
    }

    if (!writer.file) {
        psa_destroy_key(writer.keyId);
        writer.keyId = 0;
        writer.path = "";
        writer.encrypted = false;
        error = "cannot open SFLOG1 for append: " + path;
        return false;
    }

    return true;
}

static bool openPlainWriterDirect(
    const String &path,
    LogStorageWriter &writer,
    String &error
)
{
    writer = LogStorageWriter();
    error = "";

    writer.file = STORAGE.open(path.c_str(), FILE_APPEND);

    if (!writer.file) {
        error = "cannot open plaintext log for append: " + path;
        return false;
    }

    writer.path = path;
    writer.encrypted = false;
    return true;
}

static bool rollbackEncryptedAppend(
    LogStorageWriter &writer,
    uint64_t recordOffset,
    String &error
);


static bool appendEncryptedRecord(
    LogStorageWriter &writer,
    const uint8_t *plaintext,
    size_t plaintextLength,
    uint64_t &physicalBytesWritten,
    String &error
)
{
    physicalBytesWritten = 0;
    error = "";

    if (
        !writer.file ||
        !writer.encrypted ||
        writer.keyId == 0 ||
        !plaintext ||
        plaintextLength == 0 ||
        plaintextLength > SFLOG_MAX_RECORD_PLAINTEXT
    ) {
        error = "invalid SFLOG1 append state";
        return false;
    }

    uint64_t recordOffset = writer.file.size();

    uint8_t recordHeader[SFLOG_RECORD_HEADER_BYTES] = {};
    memcpy(
        recordHeader,
        SFLOG_RECORD_MAGIC,
        sizeof(SFLOG_RECORD_MAGIC)
    );

    uint32_t totalLength =
        (uint32_t)(
            SFLOG_RECORD_HEADER_BYTES +
            plaintextLength +
            SFLOG_TAG_BYTES
        );

    writeU32Le(recordHeader + 8, totalLength);
    writeU32Le(
        recordHeader + 12,
        (uint32_t)plaintextLength
    );

    uint8_t nonce[SFLOG_RECORD_NONCE_BYTES] = {};

    if (!deriveRecordNonce(
            writer.fileNonce,
            recordOffset,
            nonce
        )) {
        error = "SFLOG1 nonce derivation failed";
        return false;
    }

    uint8_t aad[
        sizeof(SFLOG_AAD_DOMAIN) - 1 +
        SFLOG_FILE_NONCE_BYTES +
        8 +
        4 +
        4
    ] = {};

    size_t aadOffset = 0;
    memcpy(
        aad + aadOffset,
        SFLOG_AAD_DOMAIN,
        sizeof(SFLOG_AAD_DOMAIN) - 1
    );
    aadOffset += sizeof(SFLOG_AAD_DOMAIN) - 1;

    memcpy(
        aad + aadOffset,
        writer.fileNonce,
        SFLOG_FILE_NONCE_BYTES
    );
    aadOffset += SFLOG_FILE_NONCE_BYTES;

    writeU64Le(aad + aadOffset, recordOffset);
    aadOffset += 8;
    writeU32Le(aad + aadOffset, totalLength);
    aadOffset += 4;
    writeU32Le(aad + aadOffset, (uint32_t)plaintextLength);
    aadOffset += 4;

    uint8_t ciphertext[SFLOG_MAX_RECORD_PLAINTEXT + SFLOG_TAG_BYTES] = {};
    size_t encryptedLength = 0;

    psa_status_t status =
        psa_aead_encrypt(
            writer.keyId,
            PSA_ALG_GCM,
            nonce,
            SFLOG_RECORD_NONCE_BYTES,
            aad,
            aadOffset,
            plaintext,
            plaintextLength,
            ciphertext,
            plaintextLength + SFLOG_TAG_BYTES,
            &encryptedLength
        );

    secureWipe(nonce, sizeof(nonce));

    if (
        status != PSA_SUCCESS ||
        encryptedLength != plaintextLength + SFLOG_TAG_BYTES
    ) {
        secureWipe(ciphertext, sizeof(ciphertext));
        error =
            "SFLOG1 encryption failed: " +
            String((int)status);
        return false;
    }

    bool ok =
        writeAll(
            writer.file,
            recordHeader,
            sizeof(recordHeader)
        ) &&
        writeAll(
            writer.file,
            ciphertext,
            encryptedLength
        );

    secureWipe(ciphertext, sizeof(ciphertext));

    if (!ok) {
        String rollbackError;

        bool rollbackOk =
            rollbackEncryptedAppend(
                writer,
                recordOffset,
                rollbackError
            );

        if (rollbackOk) {
            error =
                "SFLOG1 physical write failed; partial record rolled back";
        } else {
            error =
                "SFLOG1 physical write failed; rollback failed: " +
                rollbackError;
        }

        return false;
    }

    physicalBytesWritten = totalLength;
    return true;
}

static String migrationTempPath(const String &path)
{
    return path + ".sflog.tmp";
}

static String migrationBackupPath(const String &path)
{
    return path + ".sflog.migrate";
}


static bool reopenWriterAfterRollbackFailure(
    LogStorageWriter &writer
)
{
    if (!writer.path.length())
        return false;

    writer.file =
        STORAGE.open(
            writer.path.c_str(),
            FILE_APPEND
        );

    return (bool)writer.file;
}


static bool rollbackEncryptedAppend(
    LogStorageWriter &writer,
    uint64_t recordOffset,
    String &error
)
{
    error = "";

    if (!writer.path.length()) {
        error = "log writer path unavailable";
        return false;
    }

    // SensorForge rotates logs near 5 MiB. Keep the rollback bounded to the
    // same 32-bit file-offset range used throughout the existing storage code
    // and fail closed on an impossible/corrupt writer state.
    if (recordOffset > 0xFFFFFFFFULL) {
        error = "rollback offset out of range";
        return false;
    }

    if (writer.file) {
        writer.file.flush();
        writer.file.close();
    }

    String temp =
        migrationTempPath(writer.path);

    String backup =
        migrationBackupPath(writer.path);

    if (STORAGE.exists(temp.c_str()))
        STORAGE.remove(temp.c_str());

    if (STORAGE.exists(backup.c_str()))
        STORAGE.remove(backup.c_str());

    File source =
        STORAGE.open(
            writer.path.c_str(),
            FILE_READ
        );

    if (!source) {
        error = "cannot open damaged SFLOG1 file for rollback";
        reopenWriterAfterRollbackFailure(writer);
        return false;
    }

    if ((uint64_t)source.size() < recordOffset) {
        source.close();
        error = "damaged SFLOG1 file is shorter than rollback offset";
        reopenWriterAfterRollbackFailure(writer);
        return false;
    }

    File target =
        STORAGE.open(
            temp.c_str(),
            FILE_WRITE
        );

    if (!target) {
        source.close();
        error = "cannot create SFLOG1 rollback file";
        reopenWriterAfterRollbackFailure(writer);
        return false;
    }

    uint8_t buffer[512];
    uint64_t remaining =
        recordOffset;

    bool copyOk = true;

    while (remaining > 0) {
        size_t wanted =
            remaining > sizeof(buffer)
            ? sizeof(buffer)
            : (size_t)remaining;

        size_t got =
            source.read(
                buffer,
                wanted
            );

        if (got != wanted ||
            !writeAll(
                target,
                buffer,
                got
            )) {
            copyOk = false;
            break;
        }

        remaining -= got;
    }

    secureWipe(buffer, sizeof(buffer));
    target.flush();
    target.close();
    source.close();

    if (!copyOk || remaining != 0) {
        STORAGE.remove(temp.c_str());
        error = "cannot copy authenticated SFLOG1 prefix for rollback";
        reopenWriterAfterRollbackFailure(writer);
        return false;
    }

    File verify =
        STORAGE.open(
            temp.c_str(),
            FILE_READ
        );

    if (!verify ||
        (uint64_t)verify.size() != recordOffset) {
        if (verify)
            verify.close();

        STORAGE.remove(temp.c_str());
        error = "SFLOG1 rollback prefix verification failed";
        reopenWriterAfterRollbackFailure(writer);
        return false;
    }

    verify.close();

    // Reuse the same crash-recovery artifacts as plaintext->SFLOG1 migration.
    // If power fails after staging the original away, recoverMigrationArtifacts()
    // will promote the complete prefix temp file on the next open/boot.
    if (!STORAGE.rename(
            writer.path.c_str(),
            backup.c_str()
        )) {
        STORAGE.remove(temp.c_str());
        error = "cannot stage damaged SFLOG1 file for rollback";
        reopenWriterAfterRollbackFailure(writer);
        return false;
    }

    if (!STORAGE.rename(
            temp.c_str(),
            writer.path.c_str()
        )) {
        bool restored =
            STORAGE.rename(
                backup.c_str(),
                writer.path.c_str()
            );

        if (restored && STORAGE.exists(temp.c_str()))
            STORAGE.remove(temp.c_str());

        error = "cannot commit SFLOG1 rollback";
        reopenWriterAfterRollbackFailure(writer);
        return false;
    }

    STORAGE.remove(backup.c_str());

    writer.file =
        STORAGE.open(
            writer.path.c_str(),
            FILE_APPEND
        );

    if (!writer.file) {
        error = "cannot reopen SFLOG1 file after rollback";
        return false;
    }

    if ((uint64_t)writer.file.size() != recordOffset) {
        writer.file.close();
        error = "SFLOG1 rollback size verification failed";
        return false;
    }

    return true;
}


static bool recoverMigrationArtifacts(
    const String &path,
    String &error
)
{
    error = "";

    String temp = migrationTempPath(path);
    String backup = migrationBackupPath(path);

    if (STORAGE.exists(path.c_str())) {
        if (STORAGE.exists(temp.c_str()))
            STORAGE.remove(temp.c_str());

        if (STORAGE.exists(backup.c_str()))
            STORAGE.remove(backup.c_str());

        return true;
    }

    bool tempExists =
        STORAGE.exists(temp.c_str());

    bool backupExists =
        STORAGE.exists(backup.c_str());

    // A missing target with no migration artifacts is a normal empty state,
    // for example immediately after WebConfig deliberately cleared the log.
    // The writer will create a fresh plaintext/SFLOG1 generation afterwards.
    if (!tempExists && !backupExists)
        return true;

    if (tempExists) {
        if (STORAGE.rename(temp.c_str(), path.c_str())) {
            if (backupExists)
                STORAGE.remove(backup.c_str());
            return true;
        }
    }

    if (backupExists) {
        if (STORAGE.rename(backup.c_str(), path.c_str())) {
            if (tempExists)
                STORAGE.remove(temp.c_str());
            return true;
        }
    }

    if (STORAGE.exists(temp.c_str()))
        STORAGE.remove(temp.c_str());

    error = "cannot recover interrupted log migration: " + path;
    return false;
}

static bool detectPathFormat(
    const String &path,
    bool &exists,
    bool &encrypted,
    String &error
)
{
    exists = STORAGE.exists(path.c_str());
    encrypted = false;
    error = "";

    if (!exists)
        return true;

    File file = STORAGE.open(path.c_str(), FILE_READ);

    if (!file) {
        error = "cannot open log generation: " + path;
        return false;
    }

    uint8_t nonce[SFLOG_FILE_NONCE_BYTES] = {};
    String generation;

    bool ok = readHeader(
        file,
        encrypted,
        nonce,
        generation,
        error
    );

    file.close();
    return ok;
}

static bool migratePlaintextFile(
    const String &path,
    String &error
)
{
    error = "";

    File source = STORAGE.open(path.c_str(), FILE_READ);

    if (!source) {
        error = "cannot open plaintext log for migration: " + path;
        return false;
    }

    String temp = migrationTempPath(path);
    String backup = migrationBackupPath(path);

    if (STORAGE.exists(temp.c_str()))
        STORAGE.remove(temp.c_str());

    if (STORAGE.exists(backup.c_str()))
        STORAGE.remove(backup.c_str());

    if (!createEncryptedFile(temp, error)) {
        source.close();
        return false;
    }

    LogStorageWriter target;

    if (!openEncryptedWriterDirect(temp, target, error)) {
        source.close();
        STORAGE.remove(temp.c_str());
        return false;
    }

    uint8_t buffer[SFLOG_MAX_RECORD_PLAINTEXT];
    bool ok = true;

    while (source.available()) {
        size_t got =
            source.read(
                buffer,
                sizeof(buffer)
            );

        if (got == 0)
            break;

        uint64_t written = 0;

        if (!logStorageAppend(
                target,
                buffer,
                got,
                written,
                error
            )) {
            ok = false;
            break;
        }
    }

    source.close();
    logStorageFlushWriter(target);
    logStorageCloseWriter(target);
    secureWipe(buffer, sizeof(buffer));

    if (!ok) {
        STORAGE.remove(temp.c_str());
        return false;
    }

    if (!STORAGE.rename(path.c_str(), backup.c_str())) {
        STORAGE.remove(temp.c_str());
        error = "cannot stage plaintext log migration: " + path;
        return false;
    }

    if (!STORAGE.rename(temp.c_str(), path.c_str())) {
        STORAGE.rename(backup.c_str(), path.c_str());
        error = "cannot commit SFLOG1 migration: " + path;
        return false;
    }

    STORAGE.remove(backup.c_str());
    return true;
}

static bool ensureEncryptedGeneration(
    const String &path,
    String &error
)
{
    bool exists = false;
    bool encrypted = false;

    if (!recoverMigrationArtifacts(path, error))
        return false;

    if (!detectPathFormat(path, exists, encrypted, error))
        return false;

    if (!exists)
        return true;

    if (encrypted)
        return true;

    return migratePlaintextFile(path, error);
}

static bool findNextRecordMagic(
    File &file,
    uint64_t startOffset,
    uint64_t fileSize,
    uint64_t &foundOffset
)
{
    foundOffset = fileSize;

    if (startOffset >= fileSize)
        return false;

    uint8_t window[sizeof(SFLOG_RECORD_MAGIC)] = {};
    size_t filled = 0;

    if (!file.seek((uint32_t)startOffset))
        return false;

    uint64_t position = startOffset;

    while (position < fileSize) {
        int value = file.read();

        if (value < 0)
            break;

        if (filled < sizeof(window)) {
            window[filled++] = (uint8_t)value;
        } else {
            memmove(window, window + 1, sizeof(window) - 1);
            window[sizeof(window) - 1] = (uint8_t)value;
        }

        ++position;

        if (
            filled == sizeof(window) &&
            memcmp(
                window,
                SFLOG_RECORD_MAGIC,
                sizeof(window)
            ) == 0
        ) {
            foundOffset = position - sizeof(window);
            return true;
        }
    }

    return false;
}

static bool decryptRecordAt(
    File &file,
    psa_key_id_t keyId,
    const uint8_t fileNonce[SFLOG_FILE_NONCE_BYTES],
    uint64_t recordOffset,
    uint64_t fileSize,
    uint8_t plaintext[SFLOG_MAX_RECORD_PLAINTEXT],
    size_t &plaintextLength,
    uint64_t &nextOffset,
    bool &incomplete,
    String &error
)
{
    plaintextLength = 0;
    nextOffset = recordOffset;
    incomplete = false;
    error = "";

    if (
        fileSize - recordOffset <
        SFLOG_RECORD_HEADER_BYTES
    ) {
        incomplete = true;
        return false;
    }

    uint8_t header[SFLOG_RECORD_HEADER_BYTES] = {};

    if (
        !file.seek((uint32_t)recordOffset) ||
        !readAll(file, header, sizeof(header))
    ) {
        incomplete = true;
        return false;
    }

    if (
        memcmp(
            header,
            SFLOG_RECORD_MAGIC,
            sizeof(SFLOG_RECORD_MAGIC)
        ) != 0
    ) {
        error = "SFLOG1 record magic mismatch";
        return false;
    }

    uint32_t totalLength = readU32Le(header + 8);
    uint32_t declaredPlaintext = readU32Le(header + 12);

    if (
        declaredPlaintext == 0 ||
        declaredPlaintext > SFLOG_MAX_RECORD_PLAINTEXT ||
        totalLength !=
            SFLOG_RECORD_HEADER_BYTES +
            declaredPlaintext +
            SFLOG_TAG_BYTES
    ) {
        error = "invalid SFLOG1 record length";
        return false;
    }

    if (
        recordOffset + totalLength >
        fileSize
    ) {
        incomplete = true;
        return false;
    }

    uint8_t ciphertext[SFLOG_MAX_RECORD_PLAINTEXT + SFLOG_TAG_BYTES] = {};
    size_t ciphertextLength =
        declaredPlaintext +
        SFLOG_TAG_BYTES;

    if (!readAll(file, ciphertext, ciphertextLength)) {
        secureWipe(ciphertext, sizeof(ciphertext));
        incomplete = true;
        return false;
    }

    uint8_t aad[
        sizeof(SFLOG_AAD_DOMAIN) - 1 +
        SFLOG_FILE_NONCE_BYTES +
        8 +
        4 +
        4
    ] = {};

    size_t aadOffset = 0;
    memcpy(
        aad + aadOffset,
        SFLOG_AAD_DOMAIN,
        sizeof(SFLOG_AAD_DOMAIN) - 1
    );
    aadOffset += sizeof(SFLOG_AAD_DOMAIN) - 1;

    memcpy(
        aad + aadOffset,
        fileNonce,
        SFLOG_FILE_NONCE_BYTES
    );
    aadOffset += SFLOG_FILE_NONCE_BYTES;

    writeU64Le(aad + aadOffset, recordOffset);
    aadOffset += 8;
    writeU32Le(aad + aadOffset, totalLength);
    aadOffset += 4;
    writeU32Le(aad + aadOffset, declaredPlaintext);
    aadOffset += 4;

    uint8_t nonce[SFLOG_RECORD_NONCE_BYTES] = {};

    if (!deriveRecordNonce(
            fileNonce,
            recordOffset,
            nonce
        )) {
        secureWipe(ciphertext, sizeof(ciphertext));
        error = "SFLOG1 nonce derivation failed";
        return false;
    }

    size_t outputLength = 0;

    psa_status_t status =
        psa_aead_decrypt(
            keyId,
            PSA_ALG_GCM,
            nonce,
            SFLOG_RECORD_NONCE_BYTES,
            aad,
            aadOffset,
            ciphertext,
            ciphertextLength,
            plaintext,
            SFLOG_MAX_RECORD_PLAINTEXT,
            &outputLength
        );

    secureWipe(ciphertext, sizeof(ciphertext));
    secureWipe(nonce, sizeof(nonce));

    if (
        status != PSA_SUCCESS ||
        outputLength != declaredPlaintext
    ) {
        error =
            "SFLOG1 authentication/decryption failed: " +
            String((int)status);
        return false;
    }

    plaintextLength = outputLength;
    nextOffset = recordOffset + totalLength;
    return true;
}


static bool repairEncryptedTerminalTailBeforeAppend(
    LogStorageWriter &writer,
    String &error
)
{
    error = "";

    if (
        !writer.path.length() ||
        !writer.encrypted ||
        writer.keyId == 0
    ) {
        error = "invalid SFLOG1 repair state";
        return false;
    }

    File file =
        STORAGE.open(
            writer.path.c_str(),
            FILE_READ
        );

    if (!file) {
        error = "cannot open SFLOG1 file for terminal-tail scan";
        return false;
    }

    uint64_t physicalSize =
        file.size();

    if (physicalSize <= SFLOG_HEADER_BYTES) {
        file.close();
        return true;
    }

    // A torn append can affect only the terminal record being written. Scan a
    // small bounded tail instead of decrypting the complete (up to 5 MiB) log
    // on every logger open. Eight maximum-sized records leave ample room for a
    // valid predecessor plus false magic bytes inside encrypted payload.
    static const uint64_t TAIL_SCAN_BYTES =
        (uint64_t)SFLOG_MAX_RECORD_TOTAL * 8ULL;

    uint64_t scanStart =
        physicalSize >
            SFLOG_HEADER_BYTES + TAIL_SCAN_BYTES
        ? physicalSize - TAIL_SCAN_BYTES
        : SFLOG_HEADER_BYTES;

    if (!file.seek((uint32_t)scanStart)) {
        file.close();
        error = "cannot seek SFLOG1 terminal tail";
        return false;
    }

    static const size_t MAX_TAIL_MAGIC_CANDIDATES = 24;
    uint64_t candidates[MAX_TAIL_MAGIC_CANDIDATES] = {};
    size_t candidateCount = 0;

    uint8_t window[sizeof(SFLOG_RECORD_MAGIC)] = {};
    size_t filled = 0;
    uint64_t position = scanStart;

    while (position < physicalSize) {
        int value = file.read();

        if (value < 0)
            break;

        if (filled < sizeof(window)) {
            window[filled++] = (uint8_t)value;
        } else {
            memmove(
                window,
                window + 1,
                sizeof(window) - 1U
            );
            window[sizeof(window) - 1U] =
                (uint8_t)value;
        }

        ++position;

        if (
            filled == sizeof(window) &&
            memcmp(
                window,
                SFLOG_RECORD_MAGIC,
                sizeof(window)
            ) == 0
        ) {
            uint64_t candidate =
                position - sizeof(window);

            if (candidateCount < MAX_TAIL_MAGIC_CANDIDATES) {
                candidates[candidateCount++] = candidate;
            } else {
                memmove(
                    candidates,
                    candidates + 1,
                    sizeof(candidates) - sizeof(candidates[0])
                );
                candidates[MAX_TAIL_MAGIC_CANDIDATES - 1U] =
                    candidate;
            }
        }
    }

    // Work backwards. The first successfully authenticated candidate is the
    // latest trustworthy record in the file. If it already ends at physical
    // EOF the tail is clean. Otherwise everything after its authenticated end
    // is a terminal torn/corrupt append and can be removed safely.
    for (size_t i = candidateCount; i > 0; --i) {
        uint64_t candidate =
            candidates[i - 1U];

        uint8_t plaintext[SFLOG_MAX_RECORD_PLAINTEXT] = {};
        size_t plaintextLength = 0;
        uint64_t afterRecord = candidate;
        bool incomplete = false;
        String recordError;

        bool recordOk =
            decryptRecordAt(
                file,
                writer.keyId,
                writer.fileNonce,
                candidate,
                physicalSize,
                plaintext,
                plaintextLength,
                afterRecord,
                incomplete,
                recordError
            );

        secureWipe(plaintext, sizeof(plaintext));

        if (!recordOk)
            continue;

        file.close();

        if (afterRecord == physicalSize)
            return true;

        if (afterRecord < physicalSize) {
            return
                rollbackEncryptedAppend(
                    writer,
                    afterRecord,
                    error
                );
        }

        error = "authenticated SFLOG1 tail extends beyond file size";
        return false;
    }

    file.close();

    // If the file contains at most one possible record after the authenticated
    // header, failure to authenticate any candidate can only describe a torn
    // first append. Resetting to the header is therefore safe and prevents the
    // first good record after reboot from being written behind corrupt bytes.
    if (
        physicalSize <=
            (uint64_t)SFLOG_HEADER_BYTES +
            (uint64_t)SFLOG_MAX_RECORD_TOTAL
    ) {
        return
            rollbackEncryptedAppend(
                writer,
                SFLOG_HEADER_BYTES,
                error
            );
    }

    // Otherwise no authenticated candidate was found in the bounded tail. Do
    // not make a destructive guess. The v36+ reader can still resynchronize
    // around damage, and a later explicit recovery can diagnose the generation.
    return true;
}


} // namespace

String logStorageRotatedPath(const String &path)
{
    int dot = path.lastIndexOf('.');

    if (dot > path.lastIndexOf('/')) {
        return
            path.substring(0, dot) +
            ".1" +
            path.substring(dot);
    }

    return path + ".1";
}

bool logStoragePrepareGenerations(
    const String &path,
    bool encryptionEnabled,
    String &error
)
{
    error = "";

    if (!recoverMigrationArtifacts(path, error))
        return false;

    String backup = logStorageRotatedPath(path);

    if (!recoverMigrationArtifacts(backup, error))
        return false;

    if (encryptionEnabled) {
        if (!recordingCryptoBegin() || !recordingCryptoReady()) {
            error =
                "hardware log key unavailable: " +
                String(recordingCryptoKeyStatusName());
            return false;
        }

        if (!ensureEncryptedGeneration(backup, error))
            return false;

        if (!ensureEncryptedGeneration(path, error))
            return false;

        return true;
    }

    // If encryption was deliberately disabled while the current generation is
    // SFLOG1, preserve that encrypted generation as .1 and start a fresh
    // plaintext current log. Never append plaintext into an SFLOG1 container.
    bool exists = false;
    bool encrypted = false;

    if (!detectPathFormat(path, exists, encrypted, error))
        return false;

    if (exists && encrypted) {
        if (STORAGE.exists(backup.c_str()))
            STORAGE.remove(backup.c_str());

        if (!STORAGE.rename(path.c_str(), backup.c_str())) {
            error =
                "cannot rotate encrypted log before plaintext mode: " +
                path;
            return false;
        }
    }

    return true;
}

bool logStorageOpenWriter(
    const String &path,
    bool encryptionEnabled,
    LogStorageWriter &writer,
    String &error
)
{
    writer = LogStorageWriter();
    error = "";

    if (!logStoragePrepareGenerations(
            path,
            encryptionEnabled,
            error
        )) {
        return false;
    }

    if (encryptionEnabled) {
        return openEncryptedWriterDirect(
            path,
            writer,
            error
        );
    }

    return openPlainWriterDirect(
        path,
        writer,
        error
    );
}

void logStorageCloseWriter(LogStorageWriter &writer)
{
    if (writer.file) {
        writer.file.flush();
        writer.file.close();
    }

    if (writer.keyId != 0) {
        psa_destroy_key(writer.keyId);
        writer.keyId = 0;
    }

    secureWipe(writer.fileNonce, sizeof(writer.fileNonce));
    writer.path = "";
    writer.encrypted = false;
}

void logStorageFlushWriter(LogStorageWriter &writer)
{
    if (writer.file)
        writer.file.flush();
}

bool logStorageAppend(
    LogStorageWriter &writer,
    const uint8_t *plaintext,
    size_t plaintextLength,
    uint64_t &physicalBytesWritten,
    String &error
)
{
    physicalBytesWritten = 0;
    error = "";

    if (!writer.file) {
        error = "log writer is not open";
        return false;
    }

    if (plaintextLength == 0)
        return true;

    if (!plaintext) {
        error = "invalid log plaintext buffer";
        return false;
    }

    if (!writer.encrypted) {
        if (!writeAll(writer.file, plaintext, plaintextLength)) {
            error = "plaintext log write failed";
            return false;
        }

        physicalBytesWritten = plaintextLength;
        return true;
    }

    size_t offset = 0;

    while (offset < plaintextLength) {
        size_t chunk = plaintextLength - offset;

        if (chunk > SFLOG_MAX_RECORD_PLAINTEXT)
            chunk = SFLOG_MAX_RECORD_PLAINTEXT;

        uint64_t recordBytes = 0;

        if (!appendEncryptedRecord(
                writer,
                plaintext + offset,
                chunk,
                recordBytes,
                error
            )) {
            return false;
        }

        physicalBytesWritten += recordBytes;
        offset += chunk;
    }

    return true;
}

uint64_t logStorageWriterSize(LogStorageWriter &writer)
{
    return writer.file
        ? writer.file.size()
        : 0;
}

bool logStorageGetInfo(
    const String &path,
    uint64_t &physicalSize,
    bool &encrypted,
    String &generation,
    String &error
)
{
    physicalSize = 0;
    encrypted = false;
    generation = "";
    error = "";

    File file = STORAGE.open(path.c_str(), FILE_READ);

    if (!file) {
        error = "log file not found: " + path;
        return false;
    }

    physicalSize = file.size();
    uint8_t nonce[SFLOG_FILE_NONCE_BYTES] = {};

    bool ok = readHeader(
        file,
        encrypted,
        nonce,
        generation,
        error
    );

    file.close();
    secureWipe(nonce, sizeof(nonce));
    return ok;
}

bool logStorageReadChunk(
    const String &path,
    uint64_t cursor,
    size_t maxPlaintextBytes,
    String &body,
    uint64_t &nextCursor,
    uint64_t &physicalSize,
    bool &more,
    bool &reset,
    bool &encrypted,
    String &generation,
    bool &recoveredTornRecord,
    String &error
)
{
    body = "";
    nextCursor = 0;
    physicalSize = 0;
    more = false;
    reset = false;
    encrypted = false;
    generation = "";
    recoveredTornRecord = false;
    error = "";

    File file = STORAGE.open(path.c_str(), FILE_READ);

    if (!file) {
        error = "log file not found: " + path;
        return false;
    }

    physicalSize = file.size();

    uint8_t fileNonce[SFLOG_FILE_NONCE_BYTES] = {};

    if (!readHeader(
            file,
            encrypted,
            fileNonce,
            generation,
            error
        )) {
        file.close();
        return false;
    }

    if (!encrypted) {
        if (cursor > physicalSize) {
            cursor = 0;
            reset = true;
        }

        if (!file.seek((uint32_t)cursor)) {
            file.close();
            error = "cannot seek plaintext log";
            return false;
        }

        uint64_t remaining = physicalSize - cursor;
        size_t wanted =
            remaining > maxPlaintextBytes
            ? maxPlaintextBytes
            : (size_t)remaining;

        body.reserve(wanted + 1U);
        char buffer[512];
        size_t totalRead = 0;

        while (totalRead < wanted) {
            size_t request = wanted - totalRead;
            if (request > sizeof(buffer))
                request = sizeof(buffer);

            size_t got = file.readBytes(buffer, request);
            if (got == 0)
                break;

            body.concat(buffer, got);
            totalRead += got;
        }

        file.close();

        nextCursor = cursor + totalRead;
        more = nextCursor < physicalSize;
        return true;
    }

    if (maxPlaintextBytes == 0) {
        file.close();
        nextCursor = cursor;
        more = cursor < physicalSize;
        return true;
    }

    if (
        cursor == 0 ||
        cursor < SFLOG_HEADER_BYTES ||
        cursor > physicalSize
    ) {
        cursor = SFLOG_HEADER_BYTES;
        reset = cursor != nextCursor;
    }

    psa_key_id_t keyId = 0;

    if (!importLogKey(keyId)) {
        file.close();
        error =
            "hardware log key unavailable: " +
            String(recordingCryptoKeyStatusName());
        return false;
    }

    body.reserve(maxPlaintextBytes + 1U);
    uint64_t current = cursor;

    while (
        current < physicalSize &&
        body.length() < maxPlaintextBytes
    ) {
        uint64_t recordOffset = current;
        uint8_t plaintext[SFLOG_MAX_RECORD_PLAINTEXT] = {};
        size_t plaintextLength = 0;
        uint64_t afterRecord = recordOffset;
        bool incomplete = false;
        String recordError;

        bool recordOk = decryptRecordAt(
            file,
            keyId,
            fileNonce,
            recordOffset,
            physicalSize,
            plaintext,
            plaintextLength,
            afterRecord,
            incomplete,
            recordError
        );

        if (!recordOk) {
            secureWipe(plaintext, sizeof(plaintext));

            uint64_t nextMagic = physicalSize;

            if (findNextRecordMagic(
                    file,
                    recordOffset + 1U,
                    physicalSize,
                    nextMagic
                )) {
                recoveredTornRecord = true;

                // Older logger versions could split a text line across two
                // independently authenticated SFLOG1 records. If one of those
                // records is damaged and we resynchronize to a later valid
                // record, concatenate-without-separator would manufacture a
                // false log line from two unrelated fragments. Insert an
                // explicit line break at every recovery boundary. This also
                // makes already existing affected logs readable without
                // pretending that the missing bytes can be reconstructed.
                body += "\r\n";

                current = nextMagic;
                continue;
            }

            if (incomplete) {
                // Power loss while the last record was being appended. Ignore
                // only this incomplete tail. A future append starts at the
                // current physical EOF and the live cursor will resume there.
                recoveredTornRecord = true;
                current = physicalSize;
                break;
            }

            // A complete-looking final record can still be damaged (for example
            // a torn SD-sector write where the header/declared length survived
            // but the AES-GCM ciphertext/tag did not). The file header has
            // already verified that this generation belongs to the current
            // board key, and decryptRecordAt() never releases unauthenticated
            // plaintext. If no later SFLOG1 record marker exists, preserve the
            // authenticated prefix and discard only this unreadable terminal
            // tail instead of making the entire log unavailable.
            //
            // If valid records are appended later, their first record starts at
            // the previous physical EOF; a live reader cursor parked there can
            // therefore continue normally on the next request.
            recoveredTornRecord = true;
            body += "\r\n";
            current = physicalSize;
            break;
        }

        if (
            body.length() + plaintextLength >
            maxPlaintextBytes
        ) {
            secureWipe(plaintext, sizeof(plaintext));
            break;
        }

        body.concat(
            reinterpret_cast<const char *>(plaintext),
            plaintextLength
        );

        secureWipe(plaintext, sizeof(plaintext));
        current = afterRecord;
    }

    psa_destroy_key(keyId);
    file.close();

    nextCursor = current;
    more = current < physicalSize;
    return true;
}
