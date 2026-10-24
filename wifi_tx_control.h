#pragma once

#include <Arduino.h>

// Runtime WiFi TX power with an optional session override (cluster TX-power
// job, API). The override replaces cfg_wifi_tx_power_dbm until reboot or until
// it is cleared; every later STA/AP radio setup applies the effective value.
// All values are tenths of dBm.
namespace WifiTxControl {

// Board level >= max(requested, SensorForge safety floor), at most the board
// maximum (see board_config.h).
int16_t normalizeX10(int16_t requestedX10);

// 0 = no override (config value applies).
int16_t sessionOverrideX10();
void setSessionOverrideX10(int16_t valueX10);

// Value the radio should use now: override if set, else the config value.
int16_t effectiveX10();

// Sets the driver to valueX10 (already normalized). actualX10 receives the
// value read back from the driver (-1 if unavailable).
bool applyX10(int16_t valueX10, const char *context, int16_t &actualX10);

// Driver value now, -1 if the radio is off or the read fails.
int16_t driverX10();

}  // namespace WifiTxControl
