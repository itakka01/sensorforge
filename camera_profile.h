#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "camera_tuning.h"

// Cluster camera profile (phase 2): the shared image settings of a reference
// camera, distributed by the coordinator to every node with the same sensor.
// Takes effect only in Drone Mode (cluster capture operation); mounting
// settings (rotation, mirror, crop) stay per node. Wire/NVS text:
//   "P1,<version hex>,<pid hex>,<WxH>,<quality>,<aec>,<ae_level>,<tuning text>"
// where tuning text is the camera_tuning.h format ("1,...").
namespace CameraProfile {

struct Profile {
    uint32_t version = 0;       // 0 = no profile
    uint32_t pid = 0;           // sensor PID the profile was made for
    uint16_t width = 0, height = 0;
    int quality = 12;           // JPEG quality 4..63 (lower = better)
    int autoExposure = 1;       // 0/1
    int aeLevel = 0;            // -2..2
    CameraTuning::Values tuning;
};

enum Mode : uint8_t { ModeSession = 0, ModePersistent = 1, ModeClear = 2 };

// Node answer to a profile job.
enum Result : uint8_t {
    ResultApplied = 0,        // stored and active
    ResultWaiting = 1,        // stored; takes effect in Drone Mode
    ResultSensorMismatch = 2, // node has another sensor; not stored
    ResultInvalid = 3,
    ResultBusy = 4,           // cluster capture planned or camera owned; not stored
    ResultSaveFailed = 5,     // active for this session only
    ResultCameraFailed = 6,   // stored, camera restart failed
    ResultCleared = 7,
};

inline const char *resultName(uint8_t r)
{
    switch (r) {
    case ResultApplied: return "applied";
    case ResultWaiting: return "waiting_drone";
    case ResultSensorMismatch: return "sensor_mismatch";
    case ResultInvalid: return "invalid";
    case ResultBusy: return "busy";
    case ResultSaveFailed: return "save_failed";
    case ResultCameraFailed: return "camera_failed";
    case ResultCleared: return "cleared";
    default: return "unknown";
    }
}

// Resolutions a profile may carry (the project's supported camera sizes).
inline bool resolutionSupported(uint16_t w, uint16_t h)
{
    static const uint16_t kSizes[][2] = {{160, 120}, {320, 240}, {640, 480}, {800, 600},
                                         {1024, 768}, {1280, 1024}, {1600, 1200}, {2048, 1536}};
    for (const auto &s : kSizes)
        if (s[0] == w && s[1] == h)
            return true;
    return false;
}

inline bool valid(const Profile &p)
{
    return p.version != 0 && p.version <= 0x7FFFFFFFu && p.pid != 0 && p.pid <= 0xFFFF &&
           resolutionSupported(p.width, p.height) && p.quality >= 4 && p.quality <= 63 &&
           (p.autoExposure == 0 || p.autoExposure == 1) && p.aeLevel >= -2 && p.aeLevel <= 2 &&
           p.tuning.set && CameraTuning::valid(p.tuning);
}

// Writes the text form; empty string for an invalid profile.
inline void format(const Profile &p, char *out, size_t size)
{
    if (!size)
        return;
    out[0] = '\0';
    if (!valid(p))
        return;
    char tuning[80];
    CameraTuning::format(p.tuning, tuning, sizeof(tuning));
    snprintf(out, size, "P1,%lx,%lx,%ux%u,%d,%d,%d,%s", (unsigned long)p.version, (unsigned long)p.pid,
             (unsigned)p.width, (unsigned)p.height, p.quality, p.autoExposure, p.aeLevel, tuning);
}

inline bool parse(const char *text, Profile &out)
{
    out = Profile{};
    if (!text || strncmp(text, "P1,", 3) != 0)
        return false;
    Profile p;
    unsigned long version = 0, pid = 0;
    unsigned w = 0, h = 0;
    int used = 0;
    if (sscanf(text + 3, "%lx,%lx,%ux%u,%d,%d,%d,%n", &version, &pid, &w, &h, &p.quality,
               &p.autoExposure, &p.aeLevel, &used) != 7 || !used)
        return false;
    p.version = (uint32_t)version;
    p.pid = (uint32_t)pid;
    p.width = (uint16_t)w;
    p.height = (uint16_t)h;
    if (w > 0xFFFF || h > 0xFFFF || version > 0x7FFFFFFFul || pid > 0xFFFFul)
        return false;
    const char *tuningText = text + 3 + used;
    if (!*tuningText || !CameraTuning::parse(tuningText, p.tuning) || !valid(p))
        return false;
    out = p;
    return true;
}

inline bool sameSettings(const Profile &a, const Profile &b)
{
    const CameraTuning::Values &x = a.tuning, &y = b.tuning;
    return a.pid == b.pid && a.width == b.width && a.height == b.height && a.quality == b.quality &&
           a.autoExposure == b.autoExposure && a.aeLevel == b.aeLevel && x.set == y.set &&
           x.aecValue == y.aecValue && x.autoGain == y.autoGain && x.agcGain == y.agcGain &&
           x.gainCeiling == y.gainCeiling && x.awb == y.awb && x.wbMode == y.wbMode &&
           x.awbGain == y.awbGain && x.brightness == y.brightness && x.contrast == y.contrast &&
           x.saturation == y.saturation;
}

} // namespace CameraProfile
