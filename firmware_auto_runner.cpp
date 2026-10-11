#include "firmware_auto_runner.h"
#include "webconfig_firmware_auto.h"
#include "firmware_manifest_probe.h"
#include "firmware_package_fetch.h"
#include "firmware_update_admission.h"
#include "firmware_ota_activation.h"

namespace {
// Release reservation even if a future return is added to the scope.
class ScopedOtaAdmission {
public:
    ~ScopedOtaAdmission() { if (held_) FirmwareUpdateAdmission::release(); }
    bool acquire(String &reason) {
        held_ = FirmwareUpdateAdmission::tryAcquire(reason);
        return held_;
    }
    ScopedOtaAdmission(const ScopedOtaAdmission &) = delete;
    ScopedOtaAdmission &operator=(const ScopedOtaAdmission &) = delete;
    ScopedOtaAdmission() = default;
private:
    bool held_ = false;
};
}

FirmwareAutoPrepareStatus firmwareAutoPrepareOnce(String &detail) {
    detail = "";
    bool enabled = false;
    String manifestUrl;
    if (!webFirmwareAutoRead(enabled, manifestUrl) || !enabled) {
        detail = "Automatische Firmwareupdates sind nicht freigegeben.";
        return FirmwareAutoPrepareStatus::Disabled;
    }

    // Reserve before any potentially long network operation. Cluster guards
    // must reject competing work for the entire manifest+download window.
    ScopedOtaAdmission admission;
    if (!admission.acquire(detail)) {
        detail = "OTA-Reservierung abgelehnt: " + detail;
        return FirmwareAutoPrepareStatus::Busy;
    }

    // Defence in depth: never reuse a candidate returned by an earlier probe.
    FirmwareManifestCandidate candidate;
    if (!firmwareManifestFindCandidate(manifestUrl, candidate, detail)) {
        return FirmwareAutoPrepareStatus::CheckFailed;
    }
    if (!candidate.available) {
        detail = "Kein neuerer passender Firmware-Kandidat gefunden.";
        return FirmwareAutoPrepareStatus::NoUpdate;
    }

    // Stage implementation performs package SHA-256, board/image validation,
    // and independent .sfw signature verification. It does not select the
    // boot partition or reboot. This status must not be called installed.
    if (!firmwarePackageStageDownloadAdmitted(candidate, detail)) {
        return FirmwareAutoPrepareStatus::StageFailed;
    }
    detail = "Firmware " + candidate.version + " geprueft und in der inaktiven OTA-Partition bereitgestellt. Nicht installiert; kein Neustart.";
    return FirmwareAutoPrepareStatus::Staged;
}

// Explicit one-shot path for a future trusted update controller. Do not call
// from a web callback or periodically: this performs network I/O and OTA writes.
// Selecting a boot partition is NOT proof that the new firmware has booted.
FirmwareAutoActivateStatus firmwareAutoStageAndSelectOnce(String &detail) {
    detail = "";
    bool enabled = false;
    String manifestUrl;
    if (!webFirmwareAutoRead(enabled, manifestUrl) || !enabled) {
        detail = "Automatische Firmwareupdates sind nicht freigegeben.";
        return FirmwareAutoActivateStatus::Disabled;
    }

    ScopedOtaAdmission admission;
    if (!admission.acquire(detail)) {
        detail = "OTA-Reservierung abgelehnt: " + detail;
        return FirmwareAutoActivateStatus::Busy;
    }

    // Never activate a staged image left by another operation, even if its
    // partition hash still matches. A new signed package must be selected and
    // staged under THIS reservation.
    FirmwareOtaActivation::forget();
    FirmwareManifestCandidate candidate;
    if (!firmwareManifestFindCandidate(manifestUrl, candidate, detail))
        return FirmwareAutoActivateStatus::CheckFailed;
    if (!candidate.available) {
        detail = "Kein neuerer passender Firmware-Kandidat gefunden.";
        return FirmwareAutoActivateStatus::NoUpdate;
    }
    if (!firmwarePackageStageDownloadAdmitted(candidate, detail)) {
        FirmwareOtaActivation::forget();
        return FirmwareAutoActivateStatus::StageFailed;
    }

    // The activation module rechecks the partition fingerprint and the
    // stored auto-update permission before changing the boot partition.
    if (!FirmwareOtaActivation::selectForNextBoot(detail)) {
        FirmwareOtaActivation::forget();
        return FirmwareAutoActivateStatus::ActivationFailed;
    }
    // No ESP.restart() here. A controller must handle shutdown and reboot,
    // and separately validate the new version AFTER a successful boot.
    detail += " Neustart nicht ausgeloest; neue Firmware noch nicht bestaetigt.";
    return FirmwareAutoActivateStatus::BootSelected;
}

// Explicit maintenance-only entry point. May NOT return after a successful
// activation: the ESP restarts to boot the selected, signed OTA image.
// Call only from the Arduino loop task after the operator/controller has
// deliberately requested an update; never from an HTTP or UDP callback.
FirmwareAutoRebootStatus firmwareAutoStageSelectAndRebootOnce(String &detail) {
    const FirmwareAutoActivateStatus result = firmwareAutoStageAndSelectOnce(detail);
    if (result != FirmwareAutoActivateStatus::BootSelected) {
        return FirmwareAutoRebootStatus::NotStarted;
    }

    // The boot selection and persistent expected-partition record were saved
    // by selectForNextBoot(). This log is NOT a successful-installation report.
    Serial.println("[OTA] Signiertes Image gewaehlt; Neustart wird ausgeloest. Boot und 60-s-Pruefung stehen aus.");
    Serial.flush();
    delay(150);
    ESP.restart();
    // ESP.restart() normally does not return. Do not claim a successful boot
    // in the unlikely event the platform call returns.
    detail = "Neustart-Aufruf zur OTA-Aktivierung zurueckgekehrt; Boot nicht bestaetigt.";
    return FirmwareAutoRebootStatus::RestartReturned;
}
