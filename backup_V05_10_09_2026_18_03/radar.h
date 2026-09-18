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


// Dedicated UART pins from board_config.h.
int radarRxPin();
int radarTxPin();
