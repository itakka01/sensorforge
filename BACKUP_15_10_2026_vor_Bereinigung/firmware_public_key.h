#pragma once

// SensorForge firmware-update verification public key.
// DEVELOPMENT/TEST KEY: replace with an offline-generated production key before
// production deployment. The matching private key must never be stored on a
// deployed device, SD card, or in the firmware source repository.
// Algorithm: ECDSA P-256 / SHA-256
// Public-key SHA-256 fingerprint (DER):
// 1fb53e5c8dae5ed3c5ee6409e019112d2a646e8711ce3882b7cc4d7063a1f225
static const char SENSORFORGE_FIRMWARE_PUBLIC_KEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEkpSDXpw/wlI921TJ2LDBD+DwDYYF\n"
    "F8srRh1ZvtRKKaWgTbJZKA+kXJ7Y4cIsgGw1pm2clUsFR6CgfXNNVp8X6A==\n"
    "-----END PUBLIC KEY-----\n";
