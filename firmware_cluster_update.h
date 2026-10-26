#pragma once

#include <Arduino.h>

// Firmware update triggered by the coordinator (signed SFJU1 job, cluster.cpp).
// The job only carries the manifest URL: the node picks the highest release for
// its own board, downloads the signed .sfw, verifies signature, board and
// version (firmware_security.h) and only then selects it for the next boot and
// restarts. The coordinator rolls the cluster out one device at a time
// (cluster_firmware_rollout.h). loopTask only.
namespace FirmwareClusterUpdate {

// Answer codes of the node (SFJUA1), also used for the coordinator itself.
enum Code : uint8_t {
    Accepted = 0,          // job taken, update starts in the loop
    Selected = 1,          // verified image selected, restarting now
    NoUpdate = 2,          // no newer release for this board in the manifest
    Busy = 10,             // OTA admission refused (recording, capture, no Drone Mode, ...)
    IsCoordinator = 11,    // a coordinator never takes the remote job
    Invalid = 12,          // URL refused
    CheckFailed = 13,      // manifest not readable/valid
    StageFailed = 14,      // download, signature, board or version check failed
    ActivationFailed = 15, // boot partition not selectable
    InProgress = 16,       // another update is already running
};

const char *codeName(uint8_t code);
inline bool codeIsFailure(uint8_t code) { return code >= Busy; }

// Node: store a job (from the UDP packet handler); the work runs in service().
// Returns Accepted or the reason it was refused.
uint8_t requestFromCoordinator(const String &manifestUrl, bool allowOlder, uint32_t coordBoot,
                               uint32_t epoch, uint32_t seq);
// Coordinator: update itself the same way (last step of a rollout).
uint8_t requestLocal(const String &manifestUrl, bool allowOlder);

// Called from loop(): runs a pending update (blocks for the download, then
// restarts on success).
void service();

bool busy();

// Result of the last update of this device itself (requestLocal); false if none.
bool lastLocalResult(uint8_t &code, String &version, String &text, uint32_t &ageMs);

} // namespace FirmwareClusterUpdate
