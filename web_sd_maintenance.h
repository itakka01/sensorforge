#pragma once

#include <Arduino.h>
#include <WebServer.h>

struct WebSdMaintenanceUiHooks {
    String (*htmlHeader)();
    String (*htmlFooter)();
    void (*scheduleReboot)(uint32_t delayMs);
};

// Registers the complete SD maintenance surface:
// status/recovery/benchmark plus wipe, format and secure erase actions.
void webSdMaintenanceRegisterRoutes(
    WebServer &server,
    const WebSdMaintenanceUiHooks &uiHooks
);

// Advances long-running secure erase work one small chunk per firmware loop.
// Call even while WebConfig is stopped so an accepted maintenance job cannot
// strand the global storage gate.
void webSdMaintenanceLoop();

// True while an asynchronous SD maintenance operation must keep WebConfig/WiFi
// alive. The underlying job state remains private to this module.
bool webSdMaintenanceBusy();
// Cluster SD wipe: enqueues one destructive request, never executes in UDP callback.
// Completion callback is invoked from the ordinary firmware loop.
using SdClusterWipeDone = void (*)(uint32_t coordinatorBoot, uint32_t coordinatorEpoch,
                                    uint32_t sequence, uint32_t nodeBoot, uint16_t result);
bool webSdQueueClusterWipe(uint32_t coordinatorBoot, uint32_t coordinatorEpoch,
                           uint32_t sequence, SdClusterWipeDone callback);

