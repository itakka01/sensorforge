#include "cluster_espnow_drift.h"
#include <Preferences.h>
#include <cmath>
#include <cstring>

namespace {
constexpr uint32_t VERSION = 1;
constexpr size_t SLOTS = 4;
constexpr uint32_t WRITE_GAP_MS = 30UL * 60UL * 1000UL;
uint32_t lastWriteMs = 0;
bool wroteThisBoot = false;

struct Persisted {
    uint32_t version;
    char coordinator[48];
    double driftPpm;
    double uncertaintyPpm;
    uint32_t samples;
    int64_t utcUs;
    uint32_t checksum;
};

uint32_t checksum(const Persisted &v) {
    const auto *p = reinterpret_cast<const uint8_t *>(&v);
    uint32_t h = 2166136261UL;
    for (size_t i = 0; i < offsetof(Persisted, checksum); ++i) {
        h = (h ^ p[i]) * 16777619UL;
    }
    return h;
}

bool validId(const String &id) {
    if (id.isEmpty() || id.length() >= sizeof(Persisted::coordinator)) return false;
    for (size_t i = 0; i < id.length(); ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

bool valid(const Persisted &v) {
    if (v.version != VERSION ||
        !memchr(v.coordinator, 0, sizeof(v.coordinator)) ||
        !validId(String(v.coordinator)) || v.checksum != checksum(v)) return false;
    return std::isfinite(v.driftPpm) && fabs(v.driftPpm) <= 500.0 &&
           std::isfinite(v.uncertaintyPpm) && v.uncertaintyPpm >= 0.0 &&
           v.uncertaintyPpm <= 100.0 && v.samples >= 8 &&
           v.utcUs > 1600000000000000LL;
}

String slotName(size_t i) { return String("d") + String((unsigned)i); }
}

bool clusterEspNowDriftLoad(const String &coordinatorId, ClusterEspNowDrift &out) {
    out = ClusterEspNowDrift{};
    if (!validId(coordinatorId)) return false;
    Preferences pref;
    if (!pref.begin("sf-nowdrift", true)) return false;
    bool found = false;
    Persisted best{};
    for (size_t i = 0; i < SLOTS; ++i) {
        Persisted p{};
        if (pref.getBytesLength(slotName(i).c_str()) != sizeof(p) ||
            pref.getBytes(slotName(i).c_str(), &p, sizeof(p)) != sizeof(p) ||
            !valid(p) || coordinatorId != String(p.coordinator)) continue;
        if (!found || p.utcUs > best.utcUs) { best = p; found = true; }
    }
    pref.end();
    if (!found) return false;
    out.coordinatorId = coordinatorId;
    out.ppm = best.driftPpm;
    out.uncertaintyPpm = best.uncertaintyPpm;
    out.samples = best.samples;
    out.calibratedUtcUs = best.utcUs;
    return true;
}

bool clusterEspNowDriftStore(const ClusterEspNowDrift &value, bool force) {
    if (!validId(value.coordinatorId)) return false;
    Persisted p{};
    p.version = VERSION;
    value.coordinatorId.toCharArray(p.coordinator, sizeof(p.coordinator));
    p.driftPpm = value.ppm;
    p.uncertaintyPpm = value.uncertaintyPpm;
    p.samples = value.samples;
    p.utcUs = value.calibratedUtcUs;
    p.checksum = checksum(p);
    if (!valid(p)) return false;
    const uint32_t now = millis();
    if (!force && wroteThisBoot && (uint32_t)(now - lastWriteMs) < WRITE_GAP_MS) return false;

    Preferences pref;
    if (!pref.begin("sf-nowdrift", false)) return false;
    size_t chosen = SLOTS;
    int64_t oldest = INT64_MAX;
    for (size_t i = 0; i < SLOTS; ++i) {
        Persisted current{};
        bool good = pref.getBytesLength(slotName(i).c_str()) == sizeof(current) &&
                    pref.getBytes(slotName(i).c_str(), &current, sizeof(current)) == sizeof(current) &&
                    valid(current);
        if (good && strcmp(current.coordinator, p.coordinator) == 0) {
            chosen = i;
            break;
        }
        if (!good) { chosen = i; break; }
        if (current.utcUs < oldest) { oldest = current.utcUs; chosen = i; }
    }
    const String key = slotName(chosen);
    const bool ok = pref.putBytes(key.c_str(), &p, sizeof(p)) == sizeof(p);
    pref.end();
    if (ok) { wroteThisBoot = true; lastWriteMs = now; }
    return ok;
}

bool clusterEspNowDriftClear() {
    Preferences pref;
    if (!pref.begin("sf-nowdrift", false)) return false;
    const bool ok = pref.clear();
    pref.end();
    wroteThisBoot = false;
    return ok;
}
