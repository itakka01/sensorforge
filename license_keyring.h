#pragma once

#include <stdint.h>
#include <stddef.h>

// SensorForge SF2 license issuer keyring.
//
// SECURITY MODEL
// --------------
// - The web license server should only hold a key with EVALUATION capability.
// - FULL/SERVICE keys remain offline (or later inside a non-exportable HSM/KMS).
// - A separate RECOVERY key should also remain offline and be embedded here by
//   public key only from the first commercial release onward.
// - Firmware signing uses a completely separate trust domain/keypair.
//
// This checked-in beta keyring intentionally reuses the existing DEVELOPMENT
// public key so SF2 can be exercised before production keys are generated.
// It is NOT production-ready. Before the first commercial build, run
// sensorforge_license_keygen.py on a trusted/offline computer and replace this
// header with the generated one. The generated production header disables
// legacy SF1 acceptance by default.

static constexpr uint8_t SENSORFORGE_LICENSE_KEY_CAP_EVALUATION = 1U << 0;
static constexpr uint8_t SENSORFORGE_LICENSE_KEY_CAP_FULL       = 1U << 1;
static constexpr uint8_t SENSORFORGE_LICENSE_KEY_CAP_SERVICE    = 1U << 2;
static constexpr uint8_t SENSORFORGE_LICENSE_KEY_CAP_ALL =
    SENSORFORGE_LICENSE_KEY_CAP_EVALUATION |
    SENSORFORGE_LICENSE_KEY_CAP_FULL |
    SENSORFORGE_LICENSE_KEY_CAP_SERVICE;

struct SensorForgeLicenseKeyEntry {
    uint8_t keyId;
    uint8_t capabilities;
    bool enabled;
    const char *label;
    const char *publicKeyPem;
};

// Keep enabled only during the beta transition so already-issued SF1 DEV codes
// remain usable. The production key generator emits this as 0.
#define SENSORFORGE_LICENSE_ACCEPT_LEGACY_SF1 1
#define SENSORFORGE_LICENSE_KEYRING_PRODUCTION_READY 0

static const char SENSORFORGE_LICENSE_DEV_SF2_PUBLIC_KEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEGt/5hlcHyk5nHGNyxXAaZQeF6Fih\n"
    "qHnEa8zhKCaDl12HALhw51AXNPIKS/Im8L/K6dUSq/10zy4NHs00Bvn0Mw==\n"
    "-----END PUBLIC KEY-----\n";

static const SensorForgeLicenseKeyEntry SENSORFORGE_LICENSE_KEYRING[] = {
    {
        1U,
        SENSORFORGE_LICENSE_KEY_CAP_ALL,
        true,
        "DEV-BETA-ALL",
        SENSORFORGE_LICENSE_DEV_SF2_PUBLIC_KEY_PEM
    }
};

static constexpr size_t SENSORFORGE_LICENSE_KEYRING_COUNT =
    sizeof(SENSORFORGE_LICENSE_KEYRING) /
    sizeof(SENSORFORGE_LICENSE_KEYRING[0]);
