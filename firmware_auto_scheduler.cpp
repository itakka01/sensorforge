#include "firmware_auto_scheduler.h"
#include "firmware_auto_runner.h"
#include "webconfig_firmware_auto.h"
#include "cluster.h"
#include "drone_mode.h"
#include <Arduino.h>
#include <WiFi.h>

namespace {
constexpr uint32_t kFirstCheckAfterMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kCheckIntervalMs = 24UL * 60UL * 60UL * 1000UL;
uint32_t lastCheckMs = 0;
bool checkedOnce = false;
}

void firmwareAutoSchedulerLoop() {
#if SENSORFORGE_UNATTENDED_OTA_VERIFIED
    // Called only from the normal loop task. Never perform OTA as an HTTP
    // callback, an ISR, or on the Coordinator. Drone mode is a hard condition.
    if (!droneModeEnabled() || WiFi.status() != WL_CONNECTED ||
        !clusterRuntimeActive() || clusterLocalIsCoordinator()) return;

    const uint32_t now = millis();
    if (!checkedOnce) {
        if (now < kFirstCheckAfterMs) return;
    } else if (static_cast<uint32_t>(now - lastCheckMs) < kCheckIntervalMs) {
        return;
    }
    bool enabled = false;
    String source;
    if (!webFirmwareAutoRead(enabled, source) || !enabled) return;
    checkedOnce = true;
    lastCheckMs = now;

    Serial.println("[OTA-AUTO] Updatepruefung gestartet (nicht installiert).");
    String detail;
    const FirmwareAutoRebootStatus result = firmwareAutoStageSelectAndRebootOnce(detail);
    // This line is only reached if no reboot happened.
    Serial.print("[OTA-AUTO] Kein bestaetigtes Update: ");
    Serial.println(detail);
    (void)result;
#else
    // Fails closed in all distributed firmware builds until explicitly verified.
#endif
}
