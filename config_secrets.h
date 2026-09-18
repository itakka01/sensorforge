#pragma once

#include <Arduino.h>

// SensorForge encrypted config-secret envelope.
//
// Persistent representation:
//   SFSEC1:<base64url(nonce || ciphertext || gcm_tag)>
//
// The encryption key is derived from the board's SensorForge eFuse HMAC root
// using a dedicated domain that is independent from video-storage keys.
// The field name is authenticated as AAD, so ciphertext cannot be moved between
// wifi_pass / hotspot_password / web_password without authentication failure.

bool configSecretIsEncrypted(const String &value);

bool configSecretEncrypt(
    const char *fieldName,
    const String &plaintext,
    String &storedValue,
    String &error
);

// Accepts both legacy plaintext and SFSEC1. For legacy plaintext, plaintext is
// returned unchanged and wasEncrypted=false. SFSEC1 is authenticated and
// decrypted with the board-bound hardware key.
bool configSecretDecode(
    const char *fieldName,
    const String &storedValue,
    String &plaintext,
    bool &wasEncrypted,
    String &error
);
