#pragma once

#include <Arduino.h>

// Optional realtime-clock support.
// The implementation auto-detects supported devices on the board-specific
// I2C pins from board_config.h. No config.txt settings are required.
void rtcBegin();

bool rtcDetected();
bool rtcClockValid();
bool rtcOscillatorStopped();
bool rtcEepromDetected();
bool rtcRestoredSystemTime();

const char *rtcTypeName();
uint8_t rtcI2cAddress();

// Write the current ESP system clock to the detected RTC.
// Intended to be called after a successful NTP synchronization.
bool rtcSyncFromSystemTime();

// Human-readable local RTC time using cfg_timezone.
// Returns "unavailable" or "invalid" when appropriate.
String rtcTimeText();

// DS3231 internal temperature sensor (0.25 C resolution).
// Returns false when no supported RTC is present or the read fails.
bool rtcReadTemperatureC(float &temperatureC);

// Compact diagnostic line for Serial/log output.
String rtcDiagnosticSummary();
