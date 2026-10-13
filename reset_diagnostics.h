#pragma once
#include <Arduino.h>
#include <esp_system.h>

// Boot-only reset diagnostics; abnormal reset history persists in NVS.
void resetDiagnosticsBegin();
const char* resetDiagnosticsReasonName();
bool resetDiagnosticsAbnormal();
uint32_t resetDiagnosticsBrownoutCount();
uint32_t resetDiagnosticsAbnormalCount();
String resetDiagnosticsHistoryHtml();
