#pragma once

// SensorForge license verification public key.
// DEVELOPMENT/TEST KEY: replace this public key with a key generated offline
// before the first commercial production release. Do not use the matching
// private development key as the long-term production signing master.
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
