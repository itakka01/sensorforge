#pragma once

#include <Arduino.h>
#include <stdint.h>

// Relative oscillator drift only. A persisted value NEVER establishes time sync.
struct ClusterEspNowDrift {
    String coordinatorId;
    double ppm = 0;
    double uncertaintyPpm = 0;
    uint32_t samples = 0;
    int64_t calibratedUtcUs = 0;
};

// Reads only a prior estimate for this stable coordinator identity.
// false means no trustworthy prior estimate; this does NOT indicate sync status.
bool clusterEspNowDriftLoad(const String &coordinatorId, ClusterEspNowDrift &out);

// Persists up to four reference identities, with wear reduction. The caller
// must supply a quality-controlled estimate from authenticated measurements.
// force is intended for tests/commissioning, never routine packet callbacks.
bool clusterEspNowDriftStore(const ClusterEspNowDrift &value, bool force = false);

// Explicitly clears all stored oscillator models (not cluster credentials).
bool clusterEspNowDriftClear();
