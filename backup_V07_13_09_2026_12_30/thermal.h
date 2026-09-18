#pragma once

#include <Arduino.h>

// SensorForge thermal safety policy.
//
// These are deliberately firmware safety constants rather than config.txt
// settings. A damaged or accidentally edited config must not be able to
// disable or weaken the over-temperature protection.
//
// ESP32 thresholds protect the processor/camera electronics. The DS3231
// threshold is a secondary enclosure-temperature indicator. It is NOT a
// direct LiPo cell-temperature measurement.
static constexpr float SENSORFORGE_THERMAL_WARNING_C = 70.0f;
static constexpr float SENSORFORGE_THERMAL_EMERGENCY_C = 80.0f;
static constexpr float SENSORFORGE_THERMAL_RECOVERY_C = 65.0f;

static constexpr float SENSORFORGE_THERMAL_RTC_WARNING_C = 45.0f;
static constexpr float SENSORFORGE_THERMAL_RTC_EMERGENCY_C = 55.0f;
static constexpr float SENSORFORGE_THERMAL_RTC_RECOVERY_C = 40.0f;

// Emergency requires several consecutive hot monitor samples to reject a
// single transient/noisy reading. CPU and RTC/enclosure use independent
// counters so either channel can trigger the emergency program.
static constexpr uint8_t SENSORFORGE_THERMAL_EMERGENCY_CONFIRM_SAMPLES = 3;
static constexpr uint8_t SENSORFORGE_THERMAL_RTC_EMERGENCY_CONFIRM_SAMPLES = 3;
static constexpr uint32_t SENSORFORGE_THERMAL_SAMPLE_INTERVAL_MS = 5000UL;

// After a thermal emergency, wake only by timer and continue normal boot only
// after all available temperature channels have cooled below their recovery
// thresholds. Five minutes prevents rapid wake/sleep cycling in a sealed case.
static constexpr uint32_t SENSORFORGE_THERMAL_COOLDOWN_SECONDS = 300UL;
static constexpr uint64_t SENSORFORGE_THERMAL_COOLDOWN_US =
    (uint64_t)SENSORFORGE_THERMAL_COOLDOWN_SECONDS * 1000000ULL;

// Cached/current thermal diagnostics supplied by the main firmware loop.
float thermalCpuTemperatureC();
bool thermalRtcTemperatureC(float &temperatureC);
bool thermalWarningActive();
bool thermalEmergencyActive();
const char *thermalStateName();
const char *thermalSourceName();
