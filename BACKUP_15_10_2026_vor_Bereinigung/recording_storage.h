#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>
#include <psa/crypto.h>


// Optional per-frame physical storage diagnostics used only by the explicit
// Recording Load Test. Normal production recording keeps this disabled.
struct RecordingStorageFrameDiagnostics {
    bool valid;
    uint32_t writeCalls;
    uint64_t writeBytes;
    uint64_t writeTotalUs;
    uint32_t writeMaxUs;
    uint32_t writeMaxBytes;
    uint32_t slowWriteCalls;
    uint32_t seekCalls;
    uint64_t seekTotalUs;
    uint32_t seekMaxUs;
};

void recordingStorageSetFrameDiagnosticsEnabled(bool enabled);
void recordingStorageBeginFrameDiagnostics();
bool recordingStorageGetFrameDiagnostics(RecordingStorageFrameDiagnostics &diagnostics);

// Transparent recording-file abstraction.
//
// Plain files are passed through unchanged.
// Encrypted files use the versioned SFENC1 container on SD while callers see the
// original logical AVI/MKV byte stream. Logical seek/read/write/size therefore
// operate on plaintext offsets.
class RecordingStorageFile {
public:
    RecordingStorageFile();
    ~RecordingStorageFile();

    RecordingStorageFile(const RecordingStorageFile &) = delete;
    RecordingStorageFile &operator=(const RecordingStorageFile &) = delete;

    bool openRead(const String &path);
    bool openWrite(const String &path, bool encrypt);

    size_t read(uint8_t *buffer, size_t length);
    int read();
    size_t write(const uint8_t *buffer, size_t length);

    bool seek(uint32_t position);
    size_t position() const;
    size_t size() const;
    int available() const;

    void flush();
    bool closeChecked();
    void close();

    bool isOpen() const;
    bool isDirectory() const;
    bool isEncrypted() const;
    bool failed() const;
    const char *lastError() const;
    String path() const;

    explicit operator bool() const;

private:
    void clearLastError();
    void setLastError(const char *format, ...);

    bool openEncryptedRead();
    bool openEncryptedWrite();
    bool allocateCryptoBuffers();
    bool importFileKeys();
    void destroyFileKeys();
    void releaseCryptoBuffers();

    bool readAndValidateHeader();
    bool writeHeader(bool finalized);

    bool loadChunk(uint32_t chunkIndex);
    bool prepareWritableChunk(uint32_t chunkIndex);
    bool flushCachedChunk();

    bool cryptChunk(
        bool encrypt,
        uint32_t chunkIndex,
        const uint8_t *input,
        uint8_t *output
    );

    bool computeMac(
        const uint8_t *data,
        size_t length,
        uint8_t output[32]
    ) const;

    bool verifyMac(
        const uint8_t *data,
        size_t length,
        const uint8_t expected[32]
    ) const;

    File file_;
    String path_;

    bool open_ = false;
    bool directory_ = false;
    bool encrypted_ = false;
    bool writing_ = false;
    bool failed_ = false;

    uint64_t logicalPosition_ = 0;
    uint64_t logicalSize_ = 0;

    uint8_t fileNonce_[16] = {};
    uint8_t keyId_[16] = {};

    psa_key_id_t aesKeyId_ = 0;
    psa_key_id_t hmacKeyId_ = 0;

    uint8_t *plainChunk_ = nullptr;
    uint8_t *physicalChunk_ = nullptr;

    // Small internal DMA-capable scratch buffers. The large 32 KiB chunk
    // caches live in PSRAM when available so the camera/Web/WiFi firmware
    // retains enough internal/DMA heap for the ESP32-S3 hardware crypto path.
    uint8_t *cryptoInput_ = nullptr;
    uint8_t *cryptoOutput_ = nullptr;

    int64_t cachedChunkIndex_ = -1;
    uint32_t cachedPlainLength_ = 0;
    bool cacheDirty_ = false;

    // Fixed-size diagnostic storage: no heap allocation is required when the
    // storage path is already under memory or I/O pressure. Cleared only when a
    // new file is opened so the reason survives close/cleanup for diagnostics.
    char lastError_[224] = {};
};

// Return the logical plaintext size of either a normal or SFENC1 file without
// allocating the large crypto read buffers. The header HMAC and board-key ID are
// verified for encrypted files.
bool recordingStorageLogicalSize(
    const String &path,
    uint64_t &logicalSize,
    bool *encrypted = nullptr
);

bool recordingStorageIsEncrypted(
    const String &path,
    bool &encrypted
);
