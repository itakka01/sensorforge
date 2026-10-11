#pragma once
#include "firmware_manifest_probe.h"

// Diagnostic-only HTTPS check: validates exact size, manifest SHA-256,
// signed .sfw payload, ESP image magic and board marker. Does NOT write to
// storage, install firmware, or reboot. Manifest itself is untrusted.
bool firmwarePackageVerifyDownload(const FirmwareManifestCandidate &candidate, String &result);

// Explicit, synchronous staging operation. Requires exclusive OTA admission.
// Streams signed .sfw image into inactive OTA slot, but never selects it as
// boot partition and never reboots. Caller must separately authorize install.
bool firmwarePackageStageDownload(const FirmwareManifestCandidate &candidate, String &result);

// Requires FirmwareUpdateAdmission::active(); does not release caller's reservation.
bool firmwarePackageStageDownloadAdmitted(const FirmwareManifestCandidate &candidate, String &result);
