#pragma once
// Automatic node OTA polling. TEST ENABLEMENT: unattended OTA is allowed
// only when the node setting is enabled and all runtime guards pass.
// ESP32 builds, rollback validation and hardware OTA tests remain outstanding.
#ifndef SENSORFORGE_UNATTENDED_OTA_VERIFIED
#define SENSORFORGE_UNATTENDED_OTA_VERIFIED 1
#endif
void firmwareAutoSchedulerLoop();
