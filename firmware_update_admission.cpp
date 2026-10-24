#include "firmware_update_admission.h"
#include "drone_mode.h"
#include "recorder.h"
#include "cluster.h"
#include "cluster_hw_time_gpio_lease.h"
#include "web_sd_maintenance.h"
#include "storage_guard.h"
#include "sync_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>

extern bool recording;
extern bool recordingLoadTestIsActive();

namespace FirmwareUpdateAdmission {
namespace {
  // THREAD SAFETY: held is atomic to allow safe reads from active() without mutex.
  // active() is called from multiple threads (e.g., cluster_capture.cpp, recorder.cpp)
  // without holding admissionMutex, so held must be atomic to avoid data race.
  std::atomic<bool> held(false);
  RecordingBlockGuard* recordingGuard = nullptr;
  // THREAD SAFETY: Mutex protects Check-Then-Act sequence in tryAcquire().
  // See LOCK_SEMANTICS_ANALYSIS.md Problem 2 for race condition proof.
  static SemaphoreHandle_t admissionMutex = nullptr;
}

void init() {
  if (admissionMutex == nullptr) {
    admissionMutex = xSemaphoreCreateMutex();
  }
}

bool active() { return held; }

bool tryAcquire(String &reason) {
  reason = "";

  // THREAD SAFETY: Mutex protects entire Check-Then-Act sequence
  if (!admissionMutex) {
    reason = "Admission system not initialized";
    return false;
  }

  if (xSemaphoreTake(admissionMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    reason = "Admission mutex timeout";
    return false;
  }

  // SAFETY: Check both held flag and guard pointer for consistency.
  if (held || recordingGuard != nullptr) {
    xSemaphoreGive(admissionMutex);
    reason = "OTA already reserved";
    return false;
  }

  if (!droneModeEnabled()) {
    xSemaphoreGive(admissionMutex);
    reason = "Drone Mode required";
    return false;
  }

  if (recording || recorderIsOpen() || recordingLoadTestIsActive()) {
    xSemaphoreGive(admissionMutex);
    reason = "Recording active";
    return false;
  }

  if (g_recordingStartBlocked || g_storageLocked || webSdMaintenanceBusy() ||
      webSdAdmissionController().busy() || syncApiExclusiveActive()) {
    xSemaphoreGive(admissionMutex);
    reason = "Storage or exclusive operation busy";
    return false;
  }

  if (clusterCaptureProtectionActive() ||
      ClusterHwTimeGpioLease::active()) {
    xSemaphoreGive(admissionMutex);
    reason = "Cluster capture or time measurement active";
    return false;
  }

  // Cooperative reservation: ordinary recording starts check this flag.
  // Capture jobs additionally check active() at acceptance.
  // SAFETY: RAII guard replaces manual save/restore pattern. Reference counter
  // g_recordingBlockDepth is automatically managed.
  recordingGuard = new RecordingBlockGuard();

  // Recheck after reserving in case a service changed state meanwhile.
  if (recording || recorderIsOpen() || recordingLoadTestIsActive() ||
      g_storageLocked || webSdMaintenanceBusy() ||
      webSdAdmissionController().busy() || syncApiExclusiveActive() ||
      clusterCaptureProtectionActive() ||
      ClusterHwTimeGpioLease::active() || !droneModeEnabled()) {
    delete recordingGuard;
    recordingGuard = nullptr;
    xSemaphoreGive(admissionMutex);
    reason = "System became busy during reservation";
    return false;
  }

  held = true;
  xSemaphoreGive(admissionMutex);
  return true;
}
void release() {
  if (!admissionMutex) return;

  xSemaphoreTake(admissionMutex, portMAX_DELAY);

  if (!held) {
    xSemaphoreGive(admissionMutex);
    return;
  }

  held = false;
  // SAFETY: RAII guard destructor restores g_recordingStartBlocked via
  // reference counter (decrements g_recordingBlockDepth).
  delete recordingGuard;
  recordingGuard = nullptr;

  xSemaphoreGive(admissionMutex);
}
} // namespace FirmwareUpdateAdmission
