#pragma once

#include <Arduino.h>
#include <FS.h>
#include <psa/crypto.h>

// Transparent SensorForge log-storage layer.
//
// Plaintext generations remain readable for backward compatibility. When
// recording encryption is enabled, current and rotated log generations are
// migrated to the authenticated SFLOG1 format before new writes are appended.
// The logical plaintext stream exposed to WebConfig remains unchanged.

struct LogStorageWriter {
    File file;
    String path;
    bool encrypted = false;
    uint8_t fileNonce[16] = {};
    psa_key_id_t keyId = 0;
};

String logStorageRotatedPath(const String &path);

// Prepare current + .1 generation for the requested policy.
// encryptionEnabled=true migrates any existing plaintext generations to
// SFLOG1. encryptionEnabled=false starts a new plaintext generation if the
// current generation is SFLOG1; the encrypted previous generation is retained
// as the .1 backup.
bool logStoragePrepareGenerations(
    const String &path,
    bool encryptionEnabled,
    String &error
);

bool logStorageOpenWriter(
    const String &path,
    bool encryptionEnabled,
    LogStorageWriter &writer,
    String &error
);

void logStorageCloseWriter(LogStorageWriter &writer);
void logStorageFlushWriter(LogStorageWriter &writer);

bool logStorageAppend(
    LogStorageWriter &writer,
    const uint8_t *plaintext,
    size_t plaintextLength,
    uint64_t &physicalBytesWritten,
    String &error
);

uint64_t logStorageWriterSize(LogStorageWriter &writer);

// Return physical file information. generation is stable for one SFLOG1 file
// generation and changes after rotation. Legacy plaintext logs return an empty
// generation string.
bool logStorageGetInfo(
    const String &path,
    uint64_t &physicalSize,
    bool &encrypted,
    String &generation,
    String &error
);

// Read up to maxPlaintextBytes from the logical plaintext stream. cursor is a
// physical cursor returned by a previous call (0 starts from the beginning).
// This keeps live-tail reads efficient without repeatedly decrypting old data.
// A torn final record is ignored. If a valid later record exists after a torn
// record, the reader resynchronizes to it and sets recoveredTornRecord=true.
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
);
