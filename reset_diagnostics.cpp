#include "reset_diagnostics.h"
#include <Preferences.h>

namespace {
static esp_reset_reason_t s_reason = ESP_RST_UNKNOWN;
static uint32_t s_brownouts = 0;
static uint32_t s_abnormal = 0;
static String s_history[8];
static uint8_t s_historyCount = 0;
static bool s_initialized = false;

const char* reasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}
bool abnormal(esp_reset_reason_t r) {
  return r == ESP_RST_BROWNOUT || r == ESP_RST_PANIC ||
         r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}
} // namespace

void resetDiagnosticsBegin() {
  if (s_initialized) return;
  s_initialized = true;
  s_reason = esp_reset_reason();
  Preferences prefs;
  if (!prefs.begin("sfresetdiag", false)) return;
  s_brownouts = prefs.getUInt("brownouts", 0);
  s_abnormal = prefs.getUInt("abnormal", 0);
  uint32_t sequence = prefs.getUInt("seq", 0);
  if (abnormal(s_reason)) {
    if (s_abnormal != UINT32_MAX) ++s_abnormal;
    if (s_reason == ESP_RST_BROWNOUT && s_brownouts != UINT32_MAX) ++s_brownouts;
    const uint32_t slot = sequence % 8U;
    char key[5];
    snprintf(key, sizeof(key), "e%lu", (unsigned long)slot);
    // No wall-clock timestamp: UTC may not be valid at boot.
    prefs.putString(key, String("#") + String(sequence + 1U) + " " + reasonName(s_reason));
    prefs.putUInt("seq", sequence + 1U);
    prefs.putUInt("abnormal", s_abnormal);
    if (s_reason == ESP_RST_BROWNOUT) prefs.putUInt("brownouts", s_brownouts);
    sequence += 1U;
  }
  s_historyCount = sequence < 8U ? (uint8_t)sequence : 8U;
  for (uint8_t i = 0; i < s_historyCount; ++i) {
    const uint32_t slot = (sequence - 1U - i) % 8U;
    char key[5];
    snprintf(key, sizeof(key), "e%lu", (unsigned long)slot);
    s_history[i] = prefs.getString(key, "");
  }
  prefs.end();
}
const char* resetDiagnosticsReasonName() { return reasonName(s_reason); }
bool resetDiagnosticsAbnormal() { return abnormal(s_reason); }
uint32_t resetDiagnosticsBrownoutCount() { return s_brownouts; }
uint32_t resetDiagnosticsAbnormalCount() { return s_abnormal; }
String resetDiagnosticsHistoryHtml() {
  if (!s_historyCount) return String("Keine abnormalen Neustarts gespeichert");
  String out;
  for (uint8_t i = 0; i < s_historyCount; ++i) {
    if (i) out += " &middot; ";
    // History data is generated only from fixed reasonName() and numeric sequence.
    out += s_history[i];
  }
  return out;
}
