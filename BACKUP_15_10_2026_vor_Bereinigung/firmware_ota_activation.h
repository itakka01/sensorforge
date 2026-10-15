#pragma once
#include <Arduino.h>
#include <esp_partition.h>

// RAM-only provenance for a signed .sfw image already staged with esp_ota_end.
// Lost on restart; never persisted or reconstructed from untrusted metadata.
namespace FirmwareOtaActivation {
  // Reports only the verified running partition after 60s; never initiates an update.
  void reportStableBootOnce();
  void forget();
  bool remember(const esp_partition_t *partition, const String &version, String &reason);
  bool ready(String &version);
  // Explicit API only. Does not reboot. Requires an OTA admission reservation.
  // Never call on the basis of an unauthenticated manifest or HTTP request.
  bool selectForNextBoot(String &reason);
}
