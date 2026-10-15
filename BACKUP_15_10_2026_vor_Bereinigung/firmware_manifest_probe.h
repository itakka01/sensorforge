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
};

// Read and select an unambiguous newer beta release for the compiled board.
// Returns false for transport/manifest errors or ambiguous best candidates.
// Even on success, this is not permission to install: verify the .sfw signature.
bool firmwareManifestFindCandidate(const String &url, FirmwareManifestCandidate &candidate, String &result);
