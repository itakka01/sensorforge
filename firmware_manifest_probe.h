#pragma once
#include <Arduino.h>

// Read-only, synchronous HTTPS probe. Does not authorize firmware installation.
// A successful result proves HTTPS transport and basic JSON structure only;
// release authenticity must be verified independently before any OTA action.
bool firmwareManifestProbe(const String &url, String &result);

// A metadata-only candidate. Neither a trusted manifest nor an install permit.
struct FirmwareManifestCandidate {
    String version;
    String packageUrl;
    String sha256Hex;
    uint32_t packageSize = 0;
    bool available = false;
    // Explicit operator choice (cluster update "Downgrade erlauben"): the
    // signed package may be older than the running firmware.
    bool allowOlder = false;
};

// Read and select an unambiguous newer release for the compiled board
// (manifest via https:// or http:// to a private IPv4).
// Returns false for transport/manifest errors or ambiguous best candidates.
// Even on success, this is not permission to install: verify the .sfw signature.
// allowOlder: pick the highest release for this board even if it is not newer
// (explicit downgrade); the signed package is checked again on download.
bool firmwareManifestFindCandidate(const String &url, FirmwareManifestCandidate &candidate, String &result,
                                   bool allowOlder = false);

// Coordinator overview: the valid releases of a manifest as JSON array
// [{board, version, channel, size}]; false on download/schema errors or
// invalid entries.
bool firmwareManifestList(const String &url, String &json, String &result);
