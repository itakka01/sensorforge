#include "firmware_update_admission.h"
#include "drone_mode.h"
#include "recorder.h"
#include "cluster.h"
#include "cluster_hw_time_pulse.h"
#include "cluster_hw_time_gpio_lease.h"
#include "web_sd_maintenance.h"
#include "storage_guard.h"
#include "sync_api.h"

extern bool recording;
extern bool recordingLoadTestIsActive();

namespace FirmwareUpdateAdmission {
namespace {
  bool held = false;
  bool previousRecordingBlock = false;
}
bool active() { return held; }

bool tryAcquire(String &reason) {
  reason = "";
  if (held) { reason = "OTA already reserved"; return false; }
  if (!droneModeEnabled()) { reason = "Drone Mode required"; return false; }
  if (recording || recorderIsOpen() || recordingLoadTestIsActive()) {
    reason = "Recording active"; return false;
  }
  if (g_recordingStartBlocked || g_storageLocked || webSdMaintenanceBusy() ||
      webSdAdmissionController().busy() || syncApiExclusiveActive()) {
    reason = "Storage or exclusive operation busy"; return false;
  }
  if (clusterCaptureProtectionActive() || clusterHwTimePulseActive() ||
      ClusterHwTimeGpioLease::active()) {
    reason = "Cluster capture or time measurement active"; return false;
  }
  // Cooperative reservation: ordinary recording starts check this flag.
  // Capture jobs additionally check active() at acceptance.
  previousRecordingBlock = g_recordingStartBlocked;
  g_recordingStartBlocked = true;
  // Recheck after reserving in case a service changed state meanwhile.
  if (recording || recorderIsOpen() || recordingLoadTestIsActive() ||
      g_storageLocked || webSdMaintenanceBusy() ||
      webSdAdmissionController().busy() || syncApiExclusiveActive() ||
      clusterCaptureProtectionActive() || clusterHwTimePulseActive() ||
      ClusterHwTimeGpioLease::active() || !droneModeEnabled()) {
    g_recordingStartBlocked = previousRecordingBlock;
    reason = "System became busy during reservation";
    return false;
  }
  held = true;
  return true;
}
void release() {
  if (!held) return;
  held = false;
  g_recordingStartBlocked = previousRecordingBlock;
}
} // namespace FirmwareUpdateAdmission
