#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// SensorForge recording-encryption root-key state.
//
// The production write key is a random 256-bit secret stored in one of the
// ESP32-S3 KEY0..KEY5 eFuse blocks with purpose HMAC_UP. The selected key slot
// is registered by one permanent SensorForge binding in EFUSE_BLK_USER_DATA so
// the same board can always recover the correct key after flash/LittleFS loss.
//
// ESP32-S3 USER_DATA is Reed-Solomon coded across the whole block and can only
// be burned once. SensorForge therefore never updates this metadata later; the
// binding itself is final and key readiness is determined from the bound slot.
//
// Provisioning is automatic and one-time when recording encryption is enabled.
// Existing non-SensorForge HMAC/key blocks are never adopted or overwritten.
enum RecordingCryptoKeyStatus {
    RECORDING_CRYPTO_KEY_UNKNOWN = 0,
    RECORDING_CRYPTO_KEY_UNPROVISIONED,
    RECORDING_CRYPTO_KEY_PROVISION_PENDING,
    RECORDING_CRYPTO_KEY_READY,
    RECORDING_CRYPTO_KEY_NO_FREE_KEY_BLOCK,
    RECORDING_CRYPTO_KEY_METADATA_UNAVAILABLE,
    RECORDING_CRYPTO_KEY_METADATA_CONFLICT,
    RECORDING_CRYPTO_KEY_KEY_CONFLICT,
    RECORDING_CRYPTO_KEY_PROVISION_FAILED,
    RECORDING_CRYPTO_KEY_HMAC_FAILED,
    RECORDING_CRYPTO_KEY_UNSUPPORTED
};

// Read/validate existing eFuse state only. This function never burns eFuses.
bool recordingCryptoBegin();

// Ensure a SensorForge hardware root key exists. If no key has been
// provisioned yet, this performs the one-time automatic eFuse provisioning.
// Calling it again after success is safe and does not burn anything again.
bool recordingCryptoEnsureProvisioned();

bool recordingCryptoReady();
RecordingCryptoKeyStatus recordingCryptoKeyStatus();
const char *recordingCryptoKeyStatusName();
int recordingCryptoKeySlot();
const char *recordingCryptoKeySourceName();

// Retained for compatibility with the current WebConfig code. New recordings
// never use the legacy development key.
bool recordingCryptoUsingDevelopmentKey();

// Stable 128-bit identifier/fingerprint of the active production storage
// master key. It is not secret and is stored in the SFENC1 header.
bool recordingCryptoGetKeyId(uint8_t output[16]);

// Derive the board-bound AES-256 key used for encrypted config secrets.
// This uses the same eFuse HMAC root as recording encryption but a completely
// separate domain, so config-secret and video-storage keys are cryptographically
// independent. The root key itself never leaves the eFuse/HMAC peripheral.
bool recordingCryptoDeriveConfigSecretKey(uint8_t output[32]);

// Derive the board-bound AES-256 key used for authenticated SFLOG1 log
// storage. This is a third independent domain beside SFENC1 video storage and
// SFSEC1 config secrets. The eFuse HMAC root itself never leaves hardware.
bool recordingCryptoDeriveLogKey(uint8_t output[32]);

// True if keyId belongs either to the current eFuse-backed production key or
// to the legacy integration-test key. Legacy support is READ ONLY so existing
// test recordings remain readable after migration to eFuse provisioning.
bool recordingCryptoKeyIdSupported(const uint8_t keyId[16]);

// Create a fresh per-file nonce. The nonce is public and stored in the file
// header. A production hardware key must already be ready.
bool recordingCryptoGenerateFileNonce(uint8_t output[16]);

// Derive independent AES-256 and HMAC-SHA256 keys for a NEW recording using
// the active eFuse-backed production master key.
bool recordingCryptoDeriveFileKeys(
    const uint8_t fileNonce[16],
    uint8_t aesKey[32],
    uint8_t hmacKey[32]
);

// Derive keys for reading an existing SFENC1 file identified by storedKeyId.
// Supports the current production key plus legacy development-test recordings.
bool recordingCryptoDeriveFileKeysForKeyId(
    const uint8_t storedKeyId[16],
    const uint8_t fileNonce[16],
    uint8_t aesKey[32],
    uint8_t hmacKey[32]
);
