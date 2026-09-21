#pragma once

#include <Arduino.h>

// =============================================================
// HLK-LD2410S RADAR DRIVER
// =============================================================
//
// OT2 remains the authoritative low-overhead presence/wakeup signal.
// UART is used for configuration and optional diagnostic target reports.
//
// UART protocol:
//   115200 baud, 8N1, 3.3-V TTL
//

struct RadarSettings {
    uint32_t maxGate;
    uint32_t minGate;
    uint32_t absenceSec;

    // Protocol representation: 5..80 means 0.5..8.0 Hz.
    uint32_t statusRateX10;
    uint32_t distanceRateX10;

    // 5 = normal, 10 = fast.
    uint32_t responseSpeed;

    uint32_t triggerThreshold[16];
    uint32_t holdThreshold[16];
};


// Initialize the dedicated hardware UART.
void radarBegin();

// Switch the LD2410S to standard report mode, read the active
// distance/trigger settings, and prepare the fast ESP-side motion detector.
//
// Call once after radarBegin(). Returns false if the UART/configuration
// handshake fails. OT2 remains available as a hardware wake/fallback signal.
bool radarStartMotionTracking(
    String &error
);

// Consume compact or standard LD2410S UART reports.
// In standard mode this also updates the ESP-side 2-second motion state.
void radarLoop();


// Read all parameters used by our Radar Config page.
bool radarReadSettings(
    RadarSettings &settings,
    String &error
);

// Write all parameters, read them back, and verify exact equality.
bool radarWriteSettings(
    const RadarSettings &settings,
    String &error
);

// Validate values against the ranges documented by Hi-Link.
bool radarValidateSettings(
    const RadarSettings &settings,
    String &error
);

// Fill a settings structure with the standard values shown by the
// official Hi-Link LD2410S host tool. This does not write the sensor.
void radarGetHiLinkDefaultSettings(
    RadarSettings &settings
);


// Optional live UART report information.
// OT2 remains the recording/wakeup input.
bool radarReportIsRecent(
    uint32_t maxAgeMs = 10000UL
);

uint8_t radarLastTargetState();
uint16_t radarLastTargetDistanceCm();


// ESP-side motion detector.
//
// A fresh event is generated whenever the energy of an active distance gate
// reaches its configured LD2410S trigger threshold. The event then remains
// active for 2000 ms without requiring the sensor's 10-second presence hold.
bool radarMotionTrackingAvailable();

// True after any valid LD2410S configuration handshake during the current
// boot. Unlike radarMotionTrackingAvailable(), this is a hardware-presence
// decision and stays true if live standard reports later become stale. If all
// startup attempts fail without a valid LD2410S response, SensorForge treats
// PRESENCE_PIN/PIR_PIN as a standalone digital PIR/motion input.
bool radarSensorDetected();

bool radarMotionActive();
uint32_t radarMotionRemainingMs();

int radarLastMotionGate();
float radarLastMotionEnergyDb();


// Return the last settings successfully cached from the LD2410S.
// This lets WebConfig render the page during recording without
// entering sensor configuration mode.
bool radarGetCachedSettings(
    RadarSettings &settings
);


// Temporarily hold the ESP-side radar motion state active without
// changing the last detected gate or energy.
void radarHoldMotion(
    uint32_t durationMs
);


// Latest standard-report energy for each distance gate.
// These values are cached continuously by radarLoop() and do not send
// additional UART commands to the LD2410S.
bool radarGateEnergyIsRecent(
    uint32_t maxAgeMs = 3000UL
);

float radarGateEnergyDb(
    uint8_t gate
);

// Age of the most recent valid standard-report sample in which this gate
// reached or exceeded its configured trigger threshold. This is tracked in
// radarLoop() for all 16 gates, independently from WebConfig polling and from
// the operational min/max gate range. Returns false if the gate has not crossed
// its trigger threshold since boot / the last trigger-threshold change.
bool radarGateLastTriggerAgeMs(
    uint8_t gate,
    uint32_t &ageMs
);


// =============================================================
// RADAR RAM DIAGNOSTIC RING BUFFER
// =============================================================
//
// Compact RAM-only snapshots captured inside radarLoop(), independently from
// browser polling. The buffer keeps the newest 400 diagnostic entries and
// overwrites the oldest entries when full. A 1-second baseline is retained,
// while target/OT2/threshold-zone changes are captured immediately.
//
// gateEnergyDeciDb uses 0.1-dB units. 0xFFFF means the LD2410S reported a
// raw energy value of zero / no usable value for that gate.

static const uint16_t RADAR_DIAGNOSTIC_CAPACITY = 400U;
static const uint16_t RADAR_DIAGNOSTIC_INVALID_ENERGY = 0xFFFFU;

struct RadarDiagnosticSample {
    uint32_t sequence;
    uint32_t uptimeMs;
    uint32_t epochSec;
    uint16_t epochMs;
    uint16_t targetDistanceCm;
    uint8_t targetState;
    uint8_t ot2High;
    uint8_t espMotionActive;
    uint8_t calibrationMode;
    uint16_t gateEnergyDeciDb[16];
};

uint16_t radarDiagnosticCount();
uint16_t radarDiagnosticCapacity();
void radarDiagnosticClear();

// Read one entry in chronological order: index 0 is the oldest retained entry.
bool radarDiagnosticGet(
    uint16_t index,
    RadarDiagnosticSample &sample
);


// =============================================================
// RADAR CALIBRATION / DIAGNOSTIC STATISTICS
// =============================================================
//
// Two independent RAM-only measurement sets are retained:
//   QUIET  = empty-room / background measurement
//   MOTION = intentional movement measurement
//
// Starting a mode clears only that mode and begins a fresh measurement.
// The other mode is retained so WebConfig can compare both distributions.
// During calibration the sensor range is temporarily widened to all 16 gates
// so even gates excluded from normal alarm detection receive real measurements.
// The normal min/max range is restored when calibration stops; a small NVS
// recovery marker protects that restoration across an unexpected reset.

enum RadarCalibrationMode : uint8_t {
    RADAR_CALIBRATION_NONE = 0,
    RADAR_CALIBRATION_QUIET,
    RADAR_CALIBRATION_MOTION
};


struct RadarCalibrationGateStats {
    uint32_t samples;
    uint32_t discardedSamples;
    float minimumDb;
    float meanDb;
    float p10Db;
    float p50Db;
    float p95Db;
    float p99Db;
    float peakDb;
};


// Start a fresh measurement for the selected mode. Returns false when
// standard gate-energy reporting is not available.
bool radarCalibrationStart(
    RadarCalibrationMode mode
);

bool radarCalibrationStart(
    RadarCalibrationMode mode,
    String &error
);

// Stop the currently active measurement while retaining its statistics.
void radarCalibrationStop();

bool radarCalibrationStop(
    String &error
);

// Clear one retained measurement. If it is currently active, it is stopped.
void radarCalibrationReset(
    RadarCalibrationMode mode
);

// Clear both retained measurement sets and stop calibration.
void radarCalibrationResetAll();

RadarCalibrationMode radarCalibrationActiveMode();

// Start delay and automatic-completion state used by the simplified WebConfig
// workflow. The countdown runs inside the ESP, independent of browser timers.
uint32_t radarCalibrationCountdownRemainingMs(
    RadarCalibrationMode mode
);

bool radarCalibrationMeasurementStarted(
    RadarCalibrationMode mode
);

bool radarCalibrationAutoCompleted(
    RadarCalibrationMode mode
);

bool radarCalibrationQualityLimited(
    RadarCalibrationMode mode
);

bool radarCalibrationAborted(
    RadarCalibrationMode mode
);

uint32_t radarCalibrationTargetValidSamples();
uint32_t radarCalibrationMaxReports();

// Elapsed wall-clock time for the selected retained measurement. The 10-second
// pre-measurement countdown is intentionally not included.
uint32_t radarCalibrationElapsedMs(
    RadarCalibrationMode mode
);

// Number of complete standard reports seen during the selected measurement.
// Per-gate valid/discarded sample counts are exposed in RadarCalibrationGateStats.
uint32_t radarCalibrationSampleCount(
    RadarCalibrationMode mode
);

bool radarCalibrationHasData(
    RadarCalibrationMode mode
);

// Read derived statistics for one gate. Raw energy == 0 is treated as an
// invalid/missing measurement for that gate and is counted as discarded.
// Percentiles are calculated from a compact 0.5-dB histogram; min/mean/peak
// retain only actual valid measured values. P10 is also exposed so a useful
// provisional trigger can be estimated when only a motion phase is available.
bool radarCalibrationGetGateStats(
    RadarCalibrationMode mode,
    uint8_t gate,
    RadarCalibrationGateStats &stats
);


// Dedicated UART pins from board_config.h.
int radarRxPin();
int radarTxPin();
