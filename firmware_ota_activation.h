#pragma once
#include <Arduino.h>
#include <esp_partition.h>

// RAM-only provenance for a signed .sfw image already staged with esp_ota_end.
// Lost on restart; never persisted or reconstructed from untrusted metadata.
namespace FirmwareOtaActivation {
  // After 60 s: confirms a new image (cancels the rollback) and reports the
  // expected partition; never initiates an update.
  void reportStableBootOnce();
  // Confirms a running image that is still pending verification (no-op
  // otherwise). Called before an intentional deep sleep, which would
  // otherwise count as a failed first boot.
  bool confirmRunningImage(const char *reason);
  void forget();
  bool remember(const esp_partition_t *partition, const String &version, String &reason);
  bool ready(String &version);
  // Explicit API only. Does not reboot. Requires an OTA admission reservation.
  // Never call on the basis of an unauthenticated manifest or HTTP request.
  // requireAutoSetting=false only for an authenticated coordinator job.
  bool selectForNextBoot(String &reason, bool requireAutoSetting = true);
}
