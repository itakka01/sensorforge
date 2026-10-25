#pragma once

#include <Arduino.h>

#include "camera_profile.h"

// Node: the active cluster camera profile. A persistent profile lives in NVS
// (namespace "sf_camprof", never in config.txt), a session profile only in
// RAM. A session profile replaces the active one until reboot without touching
// NVS, so after a reboot a stored persistent profile is active again. Loaded
// lazily on first use; loopTask only.
namespace CameraProfileStore {

bool get(CameraProfile::Profile &out, bool &persistent);
void setSession(const CameraProfile::Profile &profile);
bool setPersistent(const CameraProfile::Profile &profile, String &error);
// Removes the session and the persistent profile.
bool clear(String &error);

} // namespace CameraProfileStore

// Outcome of bringing the running camera in line with the wanted profile
// (declared here: the Arduino IDE places .ino prototypes before .ino types).
enum CameraProfileServiceResult : uint8_t {
    CAMERA_PROFILE_IN_EFFECT,   // running camera matches (or camera is off)
    CAMERA_PROFILE_DEFERRED,    // restart due, camera currently busy
    CAMERA_PROFILE_RESTART_FAILED,
};

// Implemented in sensorforge.ino (owns the camera). Node side of a profile
// job (CameraProfile::Mode); returns a CameraProfile::Result.
uint8_t cameraProfileApplyJob(const CameraProfile::Profile &profile, uint8_t mode);
// Restarts the camera when the profile in effect differs from the running
// camera (Drone Mode on/off, new or removed profile). Called from loop().
void cameraProfileService();
