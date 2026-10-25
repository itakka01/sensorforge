#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Image tuning for uniform cluster cameras (Gaussian splats): fixed exposure,
// gain and white balance plus brightness/contrast/saturation. Stored as one
// config key "camera_tuning"; empty = nothing applied (driver defaults).
// Auto exposure and AE level stay in camera_auto_exposure/camera_ae_level.
// Text: "1,aec_value,auto_gain,agc_gain,gainceiling,awb,wb_mode,awb_gain,
// brightness,contrast,saturation" (leading 1 = format version).
namespace CameraTuning {

struct Values {
    bool set = false;
    int aecValue = 300;     // sensor lines, used while auto exposure is off
    int autoGain = 1;       // 0/1
    int agcGain = 0;        // 0..30, used while autoGain is 0
    int gainCeiling = 0;    // 0..6 (2x..128x), used while autoGain is 1
    int awb = 1;            // automatic white balance 0/1
    int wbMode = 0;         // 0 auto, 1 sunny, 2 cloudy, 3 office, 4 home,
                            // 2000..10000 colour temperature in kelvin
    int awbGain = 1;        // 0/1
    int brightness = 0, contrast = 0, saturation = 0;   // -2..2
};

// Sensor lines; OV2640 uses 0..1200, OV3660/OV5640 larger values.
constexpr int kAecMax = 65535;
constexpr int kAecMaxOv2640 = 1200;
constexpr int kAgcMax = 30;
constexpr int kGainCeilingMax = 6;
constexpr int kWbModeMax = 4;
constexpr int kWbKelvinMin = 2000;
constexpr int kWbKelvinMax = 10000;
// Firmware defaults of camera_auto_exposure / camera_ae_level (config.cpp).
constexpr int kDefaultAutoExposure = 1;
constexpr int kDefaultAeLevel = -1;

inline bool inRange(int v, int lo, int hi) { return v >= lo && v <= hi; }

inline bool isKelvin(int wbMode) { return inRange(wbMode, kWbKelvinMin, kWbKelvinMax); }
inline bool validWbMode(int wbMode) { return inRange(wbMode, 0, kWbModeMax) || isKelvin(wbMode); }

// Manual white balance gains (12 bit, 0x400 = 1.0) for a colour temperature.
// Shape: the OV5640 application-note presets home (2800 K), sunny (5500 K)
// and cloudy (6500 K), interpolated linearly in mired (1e6/K) and extrapolated
// beyond them. Scaled to the sensor's own sunny gains, so 5500 K equals the
// "sunny" preset of the running sensor. Approximate colour temperature; the
// same kelvin gives the same gains on every camera with the same sensor.
inline void kelvinGains(int kelvin, int sunnyR, int sunnyB, int &r, int &b)
{
    struct Anchor { double mired, r, b; };
    static const Anchor kA[] = {{1e6 / 2800.0, 1040, 2112}, {1e6 / 5500.0, 1564, 1267}, {1e6 / 6500.0, 1608, 1235}};
    if (kelvin < kWbKelvinMin) kelvin = kWbKelvinMin;
    if (kelvin > kWbKelvinMax) kelvin = kWbKelvinMax;
    const double m = 1e6 / kelvin;
    const Anchor &lo = m >= kA[1].mired ? kA[0] : kA[1];
    const Anchor &hi = m >= kA[1].mired ? kA[1] : kA[2];
    const double f = (m - lo.mired) / (hi.mired - lo.mired);
    const double rr = lo.r + f * (hi.r - lo.r);
    const double bb = lo.b + f * (hi.b - lo.b);
    auto clamp12 = [](double v) { return v < 1.0 ? 1 : v > 4095.0 ? 4095 : (int)(v + 0.5); };
    r = clamp12(sunnyR * rr / kA[1].r);
    b = clamp12(sunnyB * bb / kA[1].b);
}

inline bool valid(const Values &v)
{
    return inRange(v.aecValue, 0, kAecMax) && inRange(v.autoGain, 0, 1) &&
           inRange(v.agcGain, 0, kAgcMax) && inRange(v.gainCeiling, 0, kGainCeilingMax) &&
           inRange(v.awb, 0, 1) && validWbMode(v.wbMode) && inRange(v.awbGain, 0, 1) &&
           inRange(v.brightness, -2, 2) && inRange(v.contrast, -2, 2) && inRange(v.saturation, -2, 2);
}

inline void format(const Values &v, char *out, size_t size)
{
    if (!size)
        return;
    if (!v.set) {
        out[0] = '\0';
        return;
    }
    snprintf(out, size, "1,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", v.aecValue, v.autoGain, v.agcGain,
             v.gainCeiling, v.awb, v.wbMode, v.awbGain, v.brightness, v.contrast, v.saturation);
}

// Empty text: unset values (true). Anything else must be a complete, valid set.
inline bool parse(const char *text, Values &out)
{
    out = Values{};
    if (!text || !*text)
        return true;
    Values v;
    int version = 0, used = 0;
    if (sscanf(text, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d%n", &version, &v.aecValue, &v.autoGain,
               &v.agcGain, &v.gainCeiling, &v.awb, &v.wbMode, &v.awbGain, &v.brightness,
               &v.contrast, &v.saturation, &used) != 11 ||
        version != 1 || text[used] != '\0' || !valid(v))
        return false;
    v.set = true;
    out = v;
    return true;
}

} // namespace CameraTuning

#ifdef ARDUINO
#include <Arduino.h>
// Implemented in sensorforge.ino (owns the camera). Applies auto exposure,
// AE level and the tuning to the running camera without a restart; loopTask
// only. False (with error) when the camera is off.
bool cameraApplyTuningRuntime(int autoExposure, int aeLevel,
                              const CameraTuning::Values &tuning, String &error);
// Restarts the running camera with the saved config (cfg_resolution etc.;
// an active cluster profile still wins). True when restarted or the camera is
// off; false (error) when the camera is busy or the restart failed.
bool cameraRestartWithSettings(String &error);
#endif
