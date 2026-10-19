#include "wifi_power_policy.h"
#include "logger.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace WifiPowerPolicy {
namespace {

constexpr uint32_t kServiceIntervalMs = 250U;

Status state;
uint32_t lastFailureMs = 0;
uint32_t lastServiceMs = 0;
bool serviced = false;

Mode fromIdf(wifi_ps_type_t type) {
    switch (type) {
    case WIFI_PS_NONE: return Mode::None;
    case WIFI_PS_MIN_MODEM: return Mode::MinModem;
    case WIFI_PS_MAX_MODEM: return Mode::MaxModem;
    default: return Mode::Unknown;
    }
}

wifi_ps_type_t toIdf(Mode mode) {
    switch (mode) {
    case Mode::None: return WIFI_PS_NONE;
    case Mode::MaxModem: return WIFI_PS_MAX_MODEM;
    default: return WIFI_PS_MIN_MODEM;
    }
}

}  // namespace

void service(bool isCoordinator, bool isTimeBroadcaster) {
    const uint32_t now = millis();
    if (serviced && static_cast<uint32_t>(now - lastServiceMs) < kServiceIntervalMs) return;
    serviced = true;
    lastServiceMs = now;

    state.timeCriticalRole = roleRequiresNoSleep(isCoordinator, isTimeBroadcaster);
    // Baseline = previous behaviour: the sleep type Arduino applies on STA start.
    state.expected = expectedMode(state.timeCriticalRole, fromIdf(WiFi.getSleep()));

    const bool staUp = (WiFi.getMode() & WIFI_MODE_STA) != 0;
    if (!staUp) {
        state.actual = Mode::Unknown;
        return;
    }
    wifi_ps_type_t current = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&current) != ESP_OK) {
        ++state.readFailures;
        state.actual = Mode::Unknown;
        return;
    }
    state.actual = fromIdf(current);
    if (!shouldApply(true, state.actual, state.expected, lastFailureMs, now)) return;

    const Mode previous = state.actual;
    const esp_err_t err = esp_wifi_set_ps(toIdf(state.expected));
    wifi_ps_type_t readBack = WIFI_PS_NONE;
    const bool verified = err == ESP_OK && esp_wifi_get_ps(&readBack) == ESP_OK &&
                          fromIdf(readBack) == state.expected;
    if (verified) {
        ++state.applies;
        state.actual = state.expected;
        state.lastError = 0;
        lastFailureMs = 0;
        logWrite(String("[WIFI-PS] ") + modeName(previous) + " -> " + modeName(state.expected) +
                 (state.timeCriticalRole ? " (coordinator/time broadcaster)" : " (node default)"));
    } else {
        ++state.applyFailures;
        state.lastError = err != ESP_OK ? static_cast<int32_t>(err) : -1;
        lastFailureMs = now ? now : 1U;
        logWrite(String("[WIFI-PS] apply ") + modeName(state.expected) + " failed (err " +
                 String(state.lastError) + "), retry in " + String(kRetryAfterFailureMs) + " ms");
    }
}

Status status() { return state; }

}  // namespace WifiPowerPolicy
