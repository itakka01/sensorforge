#include "motion_diagnostics.h"

#include "board_config.h"
#include "radar.h"

static bool diagnosticsInitialized = false;
static bool lastPresenceActive = false;
static bool lastRadarMotionActive = false;

static uint32_t presenceTriggerCount = 0;
static uint32_t presenceLastTriggerMs = 0;
static bool presenceLastTriggerValid = false;

static uint32_t radarTriggerCount = 0;
static uint32_t radarLastTriggerMs = 0;
static bool radarLastTriggerValid = false;

static void notePresenceTriggerNow()
{
    presenceTriggerCount++;
    presenceLastTriggerMs = millis();
    presenceLastTriggerValid = true;
}

static void noteRadarTriggerNow()
{
    radarTriggerCount++;
    radarLastTriggerMs = millis();
    radarLastTriggerValid = true;
}

void motionDiagnosticsBegin(bool presenceWakeAtBoot)
{
    diagnosticsInitialized = true;

    presenceTriggerCount = 0;
    presenceLastTriggerMs = 0;
    presenceLastTriggerValid = false;

    radarTriggerCount = 0;
    radarLastTriggerMs = 0;
    radarLastTriggerValid = false;

    // Start from inactive and sample once. This preserves a HIGH input that is
    // genuinely active after the normal PIR startup guard. A presence wake is
    // recorded explicitly so it is never dependent on pulse width.
    lastPresenceActive = false;
    lastRadarMotionActive = false;

    if (presenceWakeAtBoot) {
        notePresenceTriggerNow();
        lastPresenceActive = true;
    }

    motionDiagnosticsLoop();
}

void motionDiagnosticsLoop()
{
    if (!diagnosticsInitialized)
        return;

    bool presenceActive =
        digitalRead(PRESENCE_PIN) == HIGH;

    bool radarActive =
        radarMotionActive();

    if (
        presenceActive &&
        !lastPresenceActive
    ) {
        notePresenceTriggerNow();
    }

    if (
        radarActive &&
        !lastRadarMotionActive
    ) {
        noteRadarTriggerNow();
    }

    lastPresenceActive = presenceActive;
    lastRadarMotionActive = radarActive;
}

void motionDiagnosticsNotePresenceTrigger()
{
    if (!diagnosticsInitialized)
        return;

    // EXT1 HIGH is itself authoritative evidence of a presence/PIR event.
    // Mark the input as active as well so the following loop pass cannot count
    // the same physical pulse a second time.
    notePresenceTriggerNow();
    lastPresenceActive = true;
}

void motionDiagnosticsGetSnapshot(MotionDiagnosticsSnapshot &snapshot)
{
    uint32_t now = millis();

    snapshot.presenceActive =
        digitalRead(PRESENCE_PIN) == HIGH;

    snapshot.radarMotionActive =
        radarMotionActive();

    snapshot.presenceTriggerCount =
        presenceTriggerCount;

    snapshot.presenceLastTriggerValid =
        presenceLastTriggerValid;

    snapshot.presenceLastTriggerAgeMs =
        presenceLastTriggerValid
        ? (uint32_t)(now - presenceLastTriggerMs)
        : 0;

    snapshot.radarTriggerCount =
        radarTriggerCount;

    snapshot.radarLastTriggerValid =
        radarLastTriggerValid;

    snapshot.radarLastTriggerAgeMs =
        radarLastTriggerValid
        ? (uint32_t)(now - radarLastTriggerMs)
        : 0;
}
