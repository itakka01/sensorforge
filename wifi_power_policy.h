#pragma once
// Cluster WiFi power save (beta137: coordinator/broadcaster; beta138: all members).
// Rule: every device with an ACTIVE cluster runtime - coordinator, time broadcaster
// (also without lease) and normal nodes, independent of the board - runs with WiFi
// modem sleep disabled (WIFI_PS_NONE). Outside active cluster operation the previous
// behaviour applies: the Arduino-configured STA sleep (WiFi.getSleep(), default
// WIFI_PS_MIN_MODEM). Never depends on the momentary broadcaster lease.
// Pure decision helpers below have no Arduino/ESP-IDF dependency (host test:
// test_wifi_power_policy.cpp.test); the ESP-IDF calls live in the .cpp.
#include <stdint.h>

namespace WifiPowerPolicy {

enum class Mode : uint8_t { None = 0, MinModem = 1, MaxModem = 2, Unknown = 255 };

// Minimum pause before retrying after a failed esp_wifi_set_ps().
constexpr uint32_t kRetryAfterFailureMs = 5000U;

inline Mode expectedMode(bool clusterActive, Mode baseline) {
    return clusterActive ? Mode::None : baseline;
}

// True only when an esp_wifi_set_ps() call is needed now: STA is up, the actual
// mode differs from the expected one and no recent failure is being backed off.
inline bool shouldApply(bool staReady, Mode actual, Mode expected,
                        uint32_t lastFailureMs, uint32_t nowMs) {
    if (!staReady || expected == Mode::Unknown || actual == Mode::Unknown) return false;
    if (actual == expected) return false;
    if (lastFailureMs && static_cast<uint32_t>(nowMs - lastFailureMs) < kRetryAfterFailureMs) return false;
    return true;
}

inline const char *modeName(Mode mode) {
    switch (mode) {
    case Mode::None: return "none";
    case Mode::MinModem: return "min_modem";
    case Mode::MaxModem: return "max_modem";
    default: return "unknown";
    }
}

struct Status {
    Mode actual = Mode::Unknown;    // read back via esp_wifi_get_ps()
    Mode expected = Mode::Unknown;  // required for the current cluster state
    bool clusterActive = false;
    uint32_t applies = 0;           // successful esp_wifi_set_ps() changes
    uint32_t applyFailures = 0;     // set failed or read-back mismatch
    uint32_t readFailures = 0;      // esp_wifi_get_ps() failed while STA up
    int32_t lastError = 0;          // last esp_err_t of a failed apply (0 = none)
};

// Call regularly from the main loop. Cheap when nothing changes (one
// esp_wifi_get_ps() at most every 250 ms, no set call while correct).
// Re-applies automatically after WiFi re-initialisation (Arduino restores
// WiFi.getSleep() on STA start) and restores the baseline when the cluster stops.
void service(bool clusterActive);
Status status();

}  // namespace WifiPowerPolicy
