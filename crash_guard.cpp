#include "crash_guard.h"

#include <Preferences.h>

namespace CrashGuard {
namespace {

const char kNamespace[] = "sfcrash";

bool safe = false;
uint8_t crashes = 0;
uint8_t lastReason = ESP_RST_UNKNOWN;
bool armed = false;
uint32_t armedMs = 0;

bool isCrashReset(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC ||
           reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT ||
           reason == ESP_RST_CPU_LOCKUP;
}

const char *reasonName(uint8_t reason)
{
    switch (reason) {
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default: return "UNKNOWN";
    }
}

}  // namespace

void handleBoot(esp_reset_reason_t resetReason)
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        // Fail open: a resilience feature must never block a normal boot.
        Serial.println("CRASH GUARD: NVS unavailable - guard skipped");
        return;
    }

    const bool pending = prefs.getBool("pending", false);
    crashes = prefs.getUChar("crashes", 0);
    lastReason = prefs.getUChar("reason", ESP_RST_UNKNOWN);

    if (resetReason == ESP_RST_POWERON) {
        // A power cycle is an explicit fresh start (and clears a hung
        // peripheral that a software reset would keep).
        crashes = 0;
    } else if (isCrashReset(resetReason)) {
        // A crash after a stable run starts a new streak.
        crashes = pending ? (crashes < 255 ? crashes + 1 : 255) : 1;
        lastReason = (uint8_t)resetReason;
        prefs.putUChar("reason", lastReason);
    } else if (crashes < kCrashThreshold) {
        // Software restart, deep-sleep wake, brownout, external reset: not a
        // crash. Safe mode itself survives these until power cycle or button.
        crashes = 0;
    }

    safe = crashes >= kCrashThreshold;
    prefs.putUChar("crashes", crashes);
    prefs.putBool("pending", true);
    prefs.end();

    armed = true;
    armedMs = millis();

    if (safe) {
        Serial.printf(
            "CRASH GUARD: SAFE MODE | %u crashes in a row before stable runtime | last=%s | "
            "WiFi/WebConfig only; power cycle or WebConfig button for a normal boot\n",
            (unsigned)crashes, reasonName(lastReason));
    } else if (crashes) {
        Serial.printf("CRASH GUARD: armed | crash streak %u/%u | last=%s\n",
                      (unsigned)crashes, (unsigned)kCrashThreshold, reasonName(lastReason));
    } else {
        Serial.printf("CRASH GUARD: armed | stable_after=%lu s\n",
                      (unsigned long)(kStableAfterMs / 1000UL));
    }
}

bool safeMode()
{
    return safe;
}

uint8_t crashCount()
{
    return crashes;
}

const char *lastCrashReasonName()
{
    return reasonName(lastReason);
}

void loop()
{
    if (!armed || (uint32_t)(millis() - armedMs) < kStableAfterMs)
        return;
    armed = false;

    Preferences prefs;
    if (!prefs.begin(kNamespace, false))
        return;
    prefs.putBool("pending", false);
    if (!safe && crashes) {
        crashes = 0;
        prefs.putUChar("crashes", 0);
    }
    prefs.end();
}

void clearForNormalBoot()
{
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
        prefs.clear();
        prefs.end();
    }
    crashes = 0;
}

String bannerHtml()
{
    if (!safe)
        return String();
    return
        String("<div class='flash-notice' style='border-color:#f2b8b5;border-left-color:var(--danger);background:#fff1f0'>"
               "<strong style='color:var(--danger)'>&#9888; Abgesicherter Modus</strong><br>"
               "SensorForge ist ") + String((unsigned)crashes) +
        " Mal in Folge abgestürzt, bevor es stabil lief (zuletzt: " + reasonName(lastReason) + "). "
        "Es laufen nur WLAN und WebConfig; Kamera, Aufnahme, Bewegungserkennung, Radar und Cluster sind aus. "
        "Ein Software-Neustart bleibt im abgesicherten Modus. Normal startet das Gerät nach dem Trennen "
        "der Stromversorgung oder mit diesem Knopf:"
        "<form method='POST' action='/crash_safe_exit' style='margin-top:10px'>"
        "<button type='submit'>Normal neu starten</button></form></div>";
}

}  // namespace CrashGuard
