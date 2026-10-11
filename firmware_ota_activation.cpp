#include "firmware_ota_activation.h"
#include "firmware_update_admission.h"
#include "webconfig_firmware_auto.h"
#include "logger.h"
#include <esp_ota_ops.h>
#include <string.h>
#include <Preferences.h>

namespace {
constexpr const char *kBootNamespace = "sfota_boot";
constexpr const char *kPendingKey = "pending";
constexpr const char *kAddressKey = "target";
constexpr const char *kVersionKey = "version";

bool saveBootExpectation(const esp_partition_t *partition, const String &version) {
  Preferences prefs;
  if (!prefs.begin(kBootNamespace, false)) return false;
  // Clear stale record before a new one, even after an interrupted update.
  prefs.putBool(kPendingKey, false);
  const bool ok = prefs.putUInt(kAddressKey, partition->address) == sizeof(uint32_t) &&
                  prefs.putString(kVersionKey, version) == version.length() &&
                  prefs.putBool(kPendingKey, true) == 1U;
  if (!ok) prefs.putBool(kPendingKey, false);
  prefs.end();
  return ok;
}

void clearBootExpectation() {
  Preferences prefs;
  if (prefs.begin(kBootNamespace, false)) {
    prefs.putBool(kPendingKey, false);
    prefs.end();
  }
}

const esp_partition_t *stagedPartition = nullptr;
uint8_t stagedDigest[32] = {};
String stagedVersion;

bool hashMatches() {
  if (!stagedPartition || stagedVersion.isEmpty()) return false;
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (!next || next->address != stagedPartition->address ||
      next->size != stagedPartition->size || next->subtype != stagedPartition->subtype) return false;
  uint8_t current[32] = {};
  return esp_partition_get_sha256(next, current) == ESP_OK &&
         memcmp(current, stagedDigest, sizeof(current)) == 0;
}
}

namespace FirmwareOtaActivation {
void reportStableBootOnce() {
  static bool completed = false;
  static uint32_t lastAttemptMs = 0;
  static bool attempted = false;
  const uint32_t now = millis();
  if (completed || now < 60000UL) return;
  // Retry transient NVS and rollback-confirmation failures, but never spam
  // flash/NVS or the serial log on each fast Arduino loop iteration.
  if (attempted && static_cast<uint32_t>(now - lastAttemptMs) < 10000UL) return;
  attempted = true;
  lastAttemptMs = now;
  Preferences prefs;
  if (!prefs.begin(kBootNamespace, true)) {
    Serial.println("[OTA] Bootstatus derzeit nicht lesbar; Pruefung wird wiederholt.");
    return;
  }
  const bool pending = prefs.getBool(kPendingKey, false);
  const uint32_t target = prefs.getUInt(kAddressKey, 0);
  const String version = prefs.getString(kVersionKey, "");
  prefs.end();
  if (!pending) { completed = true; return; }
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running || !target || running->address != target) {
    Serial.println("[OTA] Erwartetes Image nicht gestartet; Update NICHT bestaetigt (Rollback/anderer Boot moeglich).");
    clearBootExpectation();
    completed = true;
    return;
  }
  // Confirm an ESP-IDF rollback-pending app only after 60 s of runtime.
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
      Serial.println("[OTA] Boot laeuft 60 s, aber Rollback-Bestaetigung fehlgeschlagen.");
      return; // keep persistent expectation for diagnostics/retry on next boot
    }
  }
  Serial.println("[OTA] Erwartete Partition gestartet und seit 60 s aktiv; Version laut Manifest: " + version + ". Kein Langzeit-Funktionstest.");
  clearBootExpectation();
  completed = true;
}

void forget() {
  stagedPartition = nullptr;
  memset(stagedDigest, 0, sizeof(stagedDigest));
  stagedVersion = "";
}

bool remember(const esp_partition_t *partition, const String &version, String &reason) {
  forget();
  if (!FirmwareUpdateAdmission::active() || !partition || version.isEmpty() ||
      esp_ota_get_next_update_partition(nullptr) != partition) {
    reason = "OTA-Bereitstellung ohne passende Reservierung oder Zielpartition.";
    return false;
  }
  if (esp_partition_get_sha256(partition, stagedDigest) != ESP_OK) {
    forget();
    reason = "Fingerabdruck der bereitgestellten OTA-Partition nicht lesbar.";
    return false;
  }
  stagedPartition = partition;
  stagedVersion = version;
  reason = "Signierte Firmware bereitgestellt und Partitionsfingerabdruck erfasst; nicht aktiviert.";
  return true;
}

bool ready(String &version) {
  version = "";
  if (!hashMatches()) { forget(); return false; }
  version = stagedVersion;
  return true;
}

bool selectForNextBoot(String &reason) {
  // Must be called explicitly while an exclusive reservation is held.
  if (!FirmwareUpdateAdmission::active()) {
    reason = "Keine exklusive OTA-Reservierung.";
    return false;
  }
  bool enabled = false;
  String url;
  if (!webFirmwareAutoRead(enabled, url) || !enabled) {
    reason = "Automatische Updates nicht freigegeben.";
    return false;
  }
  String version;
  if (!ready(version)) {
    reason = "Kein unveraendertes, geprueft bereitgestelltes Image vorhanden.";
    return false;
  }
  if (!firmwareInfoArmDirectOtaBoot()) {
    reason = "First-Boot-Schutz konnte nicht gespeichert werden.";
    return false;
  }
  if (!saveBootExpectation(stagedPartition, version)) {
    firmwareInfoCancelDirectOtaBoot();
    reason = "OTA-Booterwartung konnte nicht persistent gespeichert werden.";
    return false;
  }
  const esp_err_t err = esp_ota_set_boot_partition(stagedPartition);
  if (err != ESP_OK) {
    clearBootExpectation();
    firmwareInfoCancelDirectOtaBoot();
    reason = "OTA-Bootpartition konnte nicht gesetzt werden: " + String(esp_err_to_name(err));
    return false;
  }
  forget();
  reason = "OTA-Bootpartition fuer " + version + " ausgewaehlt. Kein Neustart ausgefuehrt; Booterfolg nicht bestaetigt.";
  return true;
}
}
