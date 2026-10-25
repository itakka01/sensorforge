#pragma once
// Pure, host-testable camera transparency report (cluster phase 1).
//
// A node reports its sensor type, capabilities and the camera settings that
// are actually in effect. Nothing here changes camera settings. The report is
// carried in the authenticated SFK1 UDP side packet (never ESP-NOW).
//
// Prepared for phase 2 (central capture profile): every parameter group has a
// source field. In phase 1 all sources are Local; later Cluster (shared value
// from the coordinator profile) and Node (per-node override, e.g. rotation,
// mirror, crop) will appear without changing the wire layout.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace ClusterCamera {

static const int32_t kUnknown = -32768;   // value not known (e.g. camera off)

enum class CamState : uint8_t { Off = 0, On = 1, Standby = 2 };

// Parameter groups; source = 2 bits per group in Report::sourceMask.
enum class Group : uint8_t {
    Resolution = 0, Quality, Exposure, Gain, WhiteBalance, Image,
    Rotation, Mirror, Crop, Count
};
enum class Source : uint8_t { Local = 0, Cluster = 1, Node = 2 };

inline Source groupSource(uint32_t mask, Group g)
{
    return static_cast<Source>((mask >> (2 * static_cast<unsigned>(g))) & 3U);
}

// Capability bits: driver function present and not a known stub for this sensor.
enum Cap : uint32_t {
    CapManualExposure = 1u << 0,   // exposure ctrl + aec value
    CapAeLevel        = 1u << 1,
    CapAec2           = 1u << 2,
    CapManualGain     = 1u << 3,   // gain ctrl + agc gain
    CapGainCeiling    = 1u << 4,
    CapAutoWb         = 1u << 5,
    CapWbPresets      = 1u << 6,
    CapAwbGain        = 1u << 7,
    CapBrightness     = 1u << 8,
    CapContrast       = 1u << 9,
    CapSaturation     = 1u << 10,
    CapSharpness      = 1u << 11,
    CapDenoise        = 1u << 12,
    CapMirror         = 1u << 13,  // hmirror + vflip
    CapCrop           = 1u << 14,  // SensorForge sensor crop (currently OV3660 only)
    CapQuality        = 1u << 15,
    CapFramesize      = 1u << 16,
};

struct Report {
    // state
    int32_t camState;        // CamState
    int32_t drone;           // 0/1
    int32_t pid;             // sensor PID, kUnknown if never detected
    int32_t xclkMhz;
    int32_t fromSensor;      // 1: values from live driver status, 0: from config
    // capabilities
    int32_t maxFramesize;    // framesize_t index
    int32_t capMask;
    // image
    int32_t framesize;       // framesize_t index
    int32_t quality;
    int32_t brightness, contrast, saturation, sharpness, denoise;
    // exposure / gain
    int32_t aec, aec2, aecValue, aeLevel;
    int32_t agc, agcGain, gainCeiling;
    // white balance
    int32_t awb, awbGain, wbMode;
    // geometry (mount dependent)
    int32_t rotation;        // configured degrees (0/180)
    int32_t hmirror, vflip;  // effective sensor flags
    int32_t cropZoomX10;     // 10 = full frame, 15, 20
    int32_t cropX, cropY;    // 0..2
    // provenance
    int32_t sourceMask;      // 2 bits per Group
    // Cluster camera profile (phase 2): version in use (0 = none) and ProfileState.
    int32_t profileVersion;
    int32_t profileState;
};

// Node state of the cluster camera profile (camera_profile.h).
enum class ProfileState : uint8_t {
    None = 0,          // no profile stored
    Active = 1,        // applied to the camera (Drone Mode)
    Waiting = 2,       // stored, takes effect when Drone Mode is on
    SensorMismatch = 3,// stored for another sensor type, not applied
    Pending = 4,       // stored, camera restart still due (busy)
};

inline void reportClear(Report &r)
{
    int32_t *p = reinterpret_cast<int32_t *>(&r);
    for (size_t i = 0; i < sizeof(Report) / sizeof(int32_t); ++i)
        p[i] = kUnknown;
    r.sourceMask = 0;
    r.profileVersion = 0;
    r.profileState = 0;
}

inline bool reportEqual(const Report &a, const Report &b)
{
    return memcmp(&a, &b, sizeof(Report)) == 0;
}

// ---- framesize helpers (esp32-camera framesize_t order) -------------------
struct FrameDim { uint16_t w, h; };
static const FrameDim kFrameDims[] = {
    {96, 96}, {160, 120}, {128, 128}, {176, 144}, {240, 176}, {240, 240},
    {320, 240}, {320, 320}, {400, 296}, {480, 320}, {640, 480}, {800, 600},
    {1024, 768}, {1280, 720}, {1280, 1024}, {1600, 1200}, {1920, 1080},
    {720, 1280}, {864, 1536}, {2048, 1536}, {2560, 1440}, {2560, 1600},
    {1080, 1920}, {2560, 1920}, {2592, 1944},
};
static const int32_t kFramesizeCount = (int32_t)(sizeof(kFrameDims) / sizeof(kFrameDims[0]));

inline bool framesizeDims(int32_t fs, uint16_t &w, uint16_t &h)
{
    if (fs < 0 || fs >= kFramesizeCount)
        return false;
    w = kFrameDims[fs].w;
    h = kFrameDims[fs].h;
    return true;
}

inline const char *sensorName(int32_t pid)
{
    switch (pid) {
    case 0x26: return "OV2640";
    case 0x3660: return "OV3660";
    case 0x5640: return "OV5640";
    case kUnknown: return "unknown";
    default: return "other";
    }
}

// ---- wire encoding ---------------------------------------------------------
// Seven comma-separated groups; an empty element means kUnknown:
//   state: camState,drone,pid,xclkMhz,fromSensor
//   caps : maxFramesize,capMask
//   img  : framesize,quality,brightness,contrast,saturation,sharpness,denoise
//   exp  : aec,aec2,aecValue,aeLevel,agc,agcGain,gainCeiling
//   wb   : awb,awbGain,wbMode
//   geo  : rotation,hmirror,vflip,cropZoomX10,cropX,cropY
//   src  : sourceMask,profileVersion,profileState
static const size_t kGroupCount = 7;
static const size_t kGroupSizes[kGroupCount] = {5, 2, 7, 7, 3, 6, 3};

inline int32_t *reportField(Report &r, size_t index)
{
    return reinterpret_cast<int32_t *>(&r) + index;
}

static_assert(sizeof(Report) == sizeof(int32_t) * (5 + 2 + 7 + 7 + 3 + 6 + 3),
              "wire groups must cover every Report field in order");

inline void appendInt(char *&p, char *end, int32_t v)
{
    if (v == kUnknown || p >= end)
        return;
    const int n = snprintf(p, (size_t)(end - p), "%ld", (long)v);
    p += (n > 0 && p + n < end) ? n : 0;
}

// Writes the seven groups into out[i] (each up to 64 chars).
inline void encode(const Report &rIn, char out[kGroupCount][64])
{
    Report r = rIn;
    size_t field = 0;
    for (size_t g = 0; g < kGroupCount; ++g) {
        char *p = out[g];
        char *end = out[g] + 63;
        *p = '\0';
        for (size_t i = 0; i < kGroupSizes[g]; ++i) {
            if (i && p < end)
                *p++ = ',';
            appendInt(p, end, *reportField(r, field++));
            *p = '\0';
        }
    }
}

inline bool parseElement(const char *s, size_t len, int32_t &out)
{
    if (len == 0) {
        out = kUnknown;
        return true;
    }
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (len == 1)
            return false;
    }
    int64_t v = 0;
    for (; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (s[i] - '0');
        if (v > 2147483647LL)
            return false;
    }
    if (neg)
        v = -v;
    if (v == kUnknown)
        return false;
    out = (int32_t)v;
    return true;
}

// Strict decode: exact element counts, integers only, plausible ranges.
inline bool decode(const char *const groups[kGroupCount], Report &out)
{
    Report r;
    reportClear(r);
    size_t field = 0;
    for (size_t g = 0; g < kGroupCount; ++g) {
        const char *s = groups[g];
        if (!s)
            return false;
        size_t count = 0;
        const char *start = s;
        for (const char *p = s;; ++p) {
            if (*p == ',' || *p == '\0') {
                if (count >= kGroupSizes[g])
                    return false;
                if (!parseElement(start, (size_t)(p - start), *reportField(r, field + count)))
                    return false;
                ++count;
                if (*p == '\0')
                    break;
                start = p + 1;
            }
        }
        if (count != kGroupSizes[g])
            return false;
        field += count;
    }
    auto in = [](int32_t v, int32_t lo, int32_t hi) { return v == kUnknown || (v >= lo && v <= hi); };
    if (!in(r.camState, 0, 2) || !in(r.drone, 0, 1) || !in(r.pid, 0, 0xFFFF) ||
        !in(r.xclkMhz, 1, 40) || !in(r.fromSensor, 0, 1) ||
        !in(r.maxFramesize, 0, kFramesizeCount - 1) || !in(r.framesize, 0, kFramesizeCount - 1) ||
        !in(r.quality, 0, 63) || !in(r.brightness, -3, 3) || !in(r.contrast, -3, 3) ||
        !in(r.saturation, -4, 4) || !in(r.sharpness, -3, 3) || !in(r.denoise, 0, 255) ||
        !in(r.aec, 0, 1) || !in(r.aec2, 0, 1) || !in(r.aecValue, 0, 65535) ||
        !in(r.aeLevel, -5, 5) || !in(r.agc, 0, 1) || !in(r.agcGain, 0, 255) ||
        !in(r.gainCeiling, 0, 255) || !in(r.awb, 0, 1) || !in(r.awbGain, 0, 1) ||
        !in(r.wbMode, 0, 4) || !in(r.rotation, 0, 359) || !in(r.hmirror, 0, 1) ||
        !in(r.vflip, 0, 1) || !in(r.cropZoomX10, 10, 40) || !in(r.cropX, 0, 2) ||
        !in(r.cropY, 0, 2))
        return false;
    if (r.capMask == kUnknown || r.capMask < 0 || r.sourceMask == kUnknown || r.sourceMask < 0)
        return false;
    if (r.profileVersion == kUnknown || r.profileVersion < 0 || r.profileState == kUnknown ||
        r.profileState < 0 || r.profileState > 4)
        return false;
    out = r;
    return true;
}

} // namespace ClusterCamera
