#pragma once

// SensorForge LEGACY SF1 license verification public key.
// DEVELOPMENT/TEST KEY. SF2 uses license_keyring.h. This key remains only for
// beta/migration compatibility and must be disabled for the commercial build
// by SENSORFORGE_LICENSE_ACCEPT_LEGACY_SF1=0 in license_keyring.h.
// Algorithm: ECDSA P-256 / SHA-256
// Public-key SHA-256 fingerprint (DER):
// 9a54183dba1cec60e09b59d274c5491a113d99926ed33050081495e25961b20e
//
// The corresponding PRIVATE key must never be stored in firmware, on the SD
// card, in the firmware repository, or on deployed devices.
static const char SENSORFORGE_LICENSE_PUBLIC_KEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEGt/5hlcHyk5nHGNyxXAaZQeF6Fih\n"
    "qHnEa8zhKCaDl12HALhw51AXNPIKS/Im8L/K6dUSq/10zy4NHs00Bvn0Mw==\n"
    "-----END PUBLIC KEY-----\n";
