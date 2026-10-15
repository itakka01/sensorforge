#include "motion_diagnostics.h"

#include "board_config.h"
#include "radar.h"
#include "driver/rtc_io.h"

static bool diagnosticsInitialized = false;
static bool presenceSamplingSuspended = false;
static bool presenceRtcSamplingActive = false;

// The physical presence input is operationally authoritative as a LEVEL.
// Do not place a high-time debounce in front of recording: AM312/OT2 HIGH must
// be allowed to start recording immediately. We only debounce the LOW re-arm
// state so short chatter cannot manufacture a train of duplicate trigger events.
static const uint32_t PRESENCE_LOW_REARM_MS = 80UL;

static bool lastRawPresenceActive = false;
static bool presenceTriggerArmed = true;
static bool presenceLowRearmTiming = false;
static uint32_t presenceLowSinceMs = 0;

static bool lastRadarMotionActive = false;

static uint32_t presenceTriggerCount = 0;
static uint32_t presenceRawRiseCount = 0;
static uint32_t presenceRejectedGlitchCount = 0;
static uint32_t presenceLastTriggerMs = 0;
static bool presenceLastTriggerValid = false;

// One-shot backup event for the awake recording path. Raw GPIO HIGH remains the
// primary start source; this latch only preserves a sampled rising event if the
// level returns LOW before a later recording decision is reached.
static bool presenceStartPending = false;
static uint32_t presenceStartPendingSinceMs = 0;

static uint32_t radarTriggerCount = 0;
static uint32_t radarLastTriggerMs = 0;
static bool radarLastTriggerValid = false;

static void acceptRawPresenceHigh();

static bool readPresenceLevel()
{
    if (presenceRtcSamplingActive) {
        return rtc_gpio_get_level(PRESENCE_PIN) != 0;
    }

    return digitalRead(PRESENCE_PIN) == HIGH;
}

static bool configureRtcPresenceInput()
{
    esp_err_t err = rtc_gpio_init(PRESENCE_PIN);
    if (err != ESP_OK)
        return false;

    err = rtc_gpio_set_direction(
        PRESENCE_PIN,
        RTC_GPIO_MODE_INPUT_ONLY
    );
    if (err != ESP_OK) {
        rtc_gpio_deinit(PRESENCE_PIN);
        return false;
    }

    rtc_gpio_pullup_dis(PRESENCE_PIN);
    rtc_gpio_pulldown_en(PRESENCE_PIN);
    return true;
}

static void resyncPresenceSamplerAfterBackendChange()
{
    const bool rawHigh = readPresenceLevel();
    lastRawPresenceActive = rawHigh;
    presenceLowRearmTiming = false;
    presenceLowSinceMs = 0;

    // Do not discard a HIGH that is already present while the backend changes.
    // It is a valid physical trigger and must survive the WiFi shutdown window.
    if (rawHigh) {
        if (presenceRawRiseCount != UINT32_MAX)
            ++presenceRawRiseCount;
        acceptRawPresenceHigh();
    } else if (!presenceTriggerArmed) {
        presenceLowRearmTiming = true;
        presenceLowSinceMs = millis();
    }
}

static void notePresenceTriggerNow()
{
    const uint32_t now = millis();

    if (presenceTriggerCount != UINT32_MAX)
        ++presenceTriggerCount;

    presenceLastTriggerMs = now;
    presenceLastTriggerValid = true;

    presenceStartPending = true;
    presenceStartPendingSinceMs = now;
}

static void noteRadarTriggerNow()
{
    if (radarTriggerCount != UINT32_MAX)
        ++radarTriggerCount;

    radarLastTriggerMs = millis();
    radarLastTriggerValid = true;
}

static void acceptRawPresenceHigh()
{
    if (!presenceTriggerArmed)
        return;

    presenceTriggerArmed = false;
    presenceLowRearmTiming = false;
    notePresenceTriggerNow();
}

void motionDiagnosticsBegin(bool presenceWakeAtBoot)
{
    diagnosticsInitialized = true;
    presenceSamplingSuspended = false;

    presenceTriggerCount = 0;
    presenceRawRiseCount = 0;
    presenceRejectedGlitchCount = 0;
    presenceLastTriggerMs = 0;
    presenceLastTriggerValid = false;
    presenceStartPending = false;
    presenceStartPendingSinceMs = 0;

    radarTriggerCount = 0;
    radarLastTriggerMs = 0;
    radarLastTriggerValid = false;
    lastRadarMotionActive = false;

    if (presenceRtcSamplingActive) {
        if (!configureRtcPresenceInput()) {
            presenceRtcSamplingActive = false;
            pinMode(PRESENCE_PIN, INPUT_PULLDOWN);
        }
    } else {
        pinMode(PRESENCE_PIN, INPUT_PULLDOWN);
    }

    const bool rawHigh = readPresenceLevel();

    lastRawPresenceActive = rawHigh;
    presenceTriggerArmed = true;
    presenceLowRearmTiming = false;
    presenceLowSinceMs = 0;

    // A hardware EXT1 wake is authoritative. On a normal boot, a HIGH that is
    // still present after the existing PIR startup guard is equally valid and
    // must not be hidden behind a software debounce window.
    if (presenceWakeAtBoot || rawHigh) {
        if (rawHigh && presenceRawRiseCount != UINT32_MAX)
            ++presenceRawRiseCount;

        acceptRawPresenceHigh();
    }
}

void motionDiagnosticsSuspendPresenceInterrupt()
{
    // Historic API name retained for the RTC/light-sleep ownership handoff.
    // There is intentionally no GPIO ISR anymore.
    presenceSamplingSuspended = true;
}

void motionDiagnosticsResumePresenceInterrupt()
{
    if (!diagnosticsInitialized)
        return;

    // Reassert whichever awake backend currently owns the presence pin.
    // After a light-sleep RTC handoff, Freenove offline-awake mode deliberately
    // returns to RTC sampling; normal/WebConfig mode returns to digital GPIO.
    if (presenceRtcSamplingActive) {
        if (!configureRtcPresenceInput()) {
            presenceRtcSamplingActive = false;
            rtc_gpio_deinit(PRESENCE_PIN);
            pinMode(PRESENCE_PIN, INPUT_PULLDOWN);
        }
    } else {
        rtc_gpio_deinit(PRESENCE_PIN);
        pinMode(PRESENCE_PIN, INPUT_PULLDOWN);
    }

    if (!presenceSamplingSuspended)
        return;

    presenceSamplingSuspended = false;

    const bool rawHigh = readPresenceLevel();

    lastRawPresenceActive = rawHigh;

    // Start a fresh awake sampling epoch. If this was an EXT1 presence wake,
    // motionDiagnosticsNotePresenceTrigger() is called immediately afterwards.
    // If it was another wake while presence is HIGH, the next loop accepts HIGH
    // directly as a valid trigger.
    presenceTriggerArmed = true;
    presenceLowRearmTiming = false;
    presenceLowSinceMs = 0;
}

bool motionDiagnosticsSetPresenceRtcSampling(bool enabled)
{
    if (enabled == presenceRtcSamplingActive) {
        if (enabled)
            return configureRtcPresenceInput();

        pinMode(PRESENCE_PIN, INPUT_PULLDOWN);
        return true;
    }

    if (enabled) {
        if (!configureRtcPresenceInput()) {
            presenceRtcSamplingActive = false;
            rtc_gpio_deinit(PRESENCE_PIN);
            pinMode(PRESENCE_PIN, INPUT_PULLDOWN);
            return false;
        }

        presenceRtcSamplingActive = true;
    } else {
        rtc_gpio_deinit(PRESENCE_PIN);
        presenceRtcSamplingActive = false;
        pinMode(PRESENCE_PIN, INPUT_PULLDOWN);
    }

    if (diagnosticsInitialized)
        resyncPresenceSamplerAfterBackendChange();

    return true;
}

bool motionDiagnosticsPresenceRtcSamplingActive()
{
    return presenceRtcSamplingActive;
}

void motionDiagnosticsLoop()
{
    if (!diagnosticsInitialized || presenceSamplingSuspended)
        return;

    const uint32_t now = millis();
    const bool rawHigh = readPresenceLevel();

    if (rawHigh != lastRawPresenceActive) {
        if (rawHigh) {
            if (presenceRawRiseCount != UINT32_MAX)
                ++presenceRawRiseCount;

            // If LOW was not stable long enough to re-arm, this is chatter or
            // continuation of the same physical event. Count it diagnostically
            // but never queue an additional recording event.
            if (!presenceTriggerArmed) {
                if (presenceRejectedGlitchCount != UINT32_MAX)
                    ++presenceRejectedGlitchCount;
            }
        } else {
            presenceLowRearmTiming = true;
            presenceLowSinceMs = now;
        }

        lastRawPresenceActive = rawHigh;
    }

    if (rawHigh) {
        presenceLowRearmTiming = false;

        // No HIGH debounce here by design. A sampled HIGH is immediately valid
        // for PIR/OT2 recording, matching the proven direct GPIO behavior.
        acceptRawPresenceHigh();
    } else if (!presenceTriggerArmed) {
        if (!presenceLowRearmTiming) {
            presenceLowRearmTiming = true;
            presenceLowSinceMs = now;
        }

        if (
            (uint32_t)(now - presenceLowSinceMs) >=
            PRESENCE_LOW_REARM_MS
        ) {
            presenceTriggerArmed = true;
            presenceLowRearmTiming = false;
        }
    }

    const bool radarActive = radarMotionActive();

    if (radarActive && !lastRadarMotionActive)
        noteRadarTriggerNow();

    lastRadarMotionActive = radarActive;
}

void motionDiagnosticsNotePresenceTrigger()
{
    if (!diagnosticsInitialized)
        return;

    // EXT1 HIGH is authoritative. Avoid double-counting if the same event was
    // already accepted by normal level sampling immediately before this call.
    if (presenceTriggerArmed) {
        acceptRawPresenceHigh();
    } else if (!presenceStartPending) {
        // Preserve a wake as a pending event even if diagnostics had already
        // observed the same HIGH but its old pending flag was consumed.
        const uint32_t now = millis();
        presenceLastTriggerMs = now;
        presenceLastTriggerValid = true;
        presenceStartPending = true;
        presenceStartPendingSinceMs = now;
    }

    lastRawPresenceActive = readPresenceLevel();
}

bool motionDiagnosticsPresenceActive()
{
    // Operationally authoritative raw level. Diagnostics must never filter a
    // real HIGH away from the recorder start/hold path.
    return readPresenceLevel();
}

bool motionDiagnosticsPresenceTriggeredRecently(
    uint32_t maxAgeMs
)
{
    if (
        !diagnosticsInitialized ||
        !presenceLastTriggerValid ||
        maxAgeMs == 0
    ) {
        return false;
    }

    return
        (uint32_t)(millis() - presenceLastTriggerMs) <=
        maxAgeMs;
}

bool motionDiagnosticsPresenceStartPending(
    uint32_t maxAgeMs
)
{
    if (
        !diagnosticsInitialized ||
        !presenceStartPending ||
        maxAgeMs == 0
    ) {
        return false;
    }

    const uint32_t ageMs =
        (uint32_t)(millis() - presenceStartPendingSinceMs);

    if (ageMs > maxAgeMs) {
        presenceStartPending = false;
        presenceStartPendingSinceMs = 0;
        return false;
    }

    return true;
}

void motionDiagnosticsConsumePresenceStartTrigger()
{
    presenceStartPending = false;
    presenceStartPendingSinceMs = 0;
}

void motionDiagnosticsGetSnapshot(MotionDiagnosticsSnapshot &snapshot)
{
    const uint32_t now = millis();
    const bool rawHigh = readPresenceLevel();

    snapshot.presenceActive = rawHigh;

    // Retained field for the existing status API. It now represents the
    // operational presence level, which is intentionally the raw GPIO level.
    snapshot.presenceDebouncedActive = rawHigh;

    snapshot.radarMotionActive = radarMotionActive();

    snapshot.presenceTriggerCount = presenceTriggerCount;
    snapshot.presenceRawRiseCount = presenceRawRiseCount;
    snapshot.presenceRejectedGlitchCount = presenceRejectedGlitchCount;

    snapshot.presenceLastTriggerValid = presenceLastTriggerValid;
    snapshot.presenceLastTriggerAgeMs =
        presenceLastTriggerValid
        ? (uint32_t)(now - presenceLastTriggerMs)
        : 0;

    snapshot.radarTriggerCount = radarTriggerCount;
    snapshot.radarLastTriggerValid = radarLastTriggerValid;
    snapshot.radarLastTriggerAgeMs =
        radarLastTriggerValid
        ? (uint32_t)(now - radarLastTriggerMs)
        : 0;
}
