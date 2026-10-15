#pragma once
#include <stdint.h>

// Optional diagnostic output only; no implicit activation and no new mode.
// Call from the owning cluster/control context (not an ISR).
// A scheduled edge tests clock scheduling + GPIO latency, NOT exposure timing.
bool clusterHwTimePulseArm(int64_t targetUtcUs);
void clusterHwTimePulseCancel();
bool clusterHwTimePulseActive();
// Returns the esp_timer timestamp of the latest output edge (0 if none).
int64_t clusterHwTimePulseLastEdgeUs();
