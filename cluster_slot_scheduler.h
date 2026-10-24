#pragma once
// Coordinator-side scheduler for ESP-NOW precision measurement slots.
//
// Pure decisions shared by cluster_espnow_radio.cpp (serviceSlots) and the host
// test test_slot_scheduler.cpp.test. No Arduino/ESP-IDF dependency, no new packets,
// no node-side change: a slot is still an ordinary SyncControl grant to one node,
// the outcome is read from the node's existing authenticated NodeSyncReport.
//
// Replaces the fixed rotation (every active node 14 s owner slot + 3.5 s guard,
// cycle N * 17.5 s) and its recovery-slot add-on. With 9 nodes that cycle took
// 157.5 s while a precision estimate is valid for 120 s, so nodes fell back to
// WiFi time although their series succeeded. A series itself needs ~2 s.
//
// 1. A slot ends as soon as the owner's series is over: the coordinator answers
//    only the owner's probes, so it sees when probing stops (idleEndMs, longer
//    than the node's exchange timeout), then shortGuardMs follows. A node
//    that finished or aborted a series probes again at the earliest after 12 s,
//    far beyond the 3 s grant lease, so a lost revoke cannot make two nodes probe
//    at the same time. The next owner starts probing about 1 s after its first
//    grant anyway, so after a finished series no extra guard is needed
//    (shortGuardMs = 0). Without any probe (startTimeoutMs), at the unchanged
//    14 s hard cap or when the owner disappears, the long guard (3.5 s) stays.
// 2. The next owner is the most urgent DUE node instead of the next in a ring:
//    due = no precision known, or precision age >= refreshAgeMs. Most urgent =
//    no precision first, then the oldest estimate; ties go to the node that
//    waited longest for a slot. Nobody due = no slot (less radio traffic).
// 3. After a slot the node is not chosen again until a report shows the
//    outcome (or outcomeTimeoutMs passes). A slot without an accepted series
//    is retried right away; from the second failure in a row an increasing
//    back-off applies (failBackoffBaseMs doubling up to failBackoffMaxMs), so a
//    permanently failing node cannot starve the others.
//
// The estimator, the 16-exchange series, quality limits, the 120 s lifetime and
// the WiFi fallback are NOT changed. See ESPNOW_TIME_ARCHITECTURE.md ("Slot-Planung").
#include <stdint.h>
#include <stddef.h>

namespace ClusterSlotScheduler {

struct Params {
    uint32_t lifetimeMs;         // precision lifetime on the node (diagnostics only)
    uint32_t refreshAgeMs;       // a node with precision becomes due at this age
    uint32_t startTimeoutMs;     // owner sent no probe since slot start -> end slot
    uint32_t idleEndMs;          // owner probed, then silent this long -> series over
    uint32_t maxSlotMs;          // hard cap of one slot (unchanged 14 s)
    uint32_t shortGuardMs;       // pause after a slot whose series is over
    uint32_t longGuardMs;        // pause after no activity / hard cap / owner lost
    uint32_t outcomeTimeoutMs;   // no report after the slot -> treat as failed
    uint32_t failBackoffBaseMs;  // back-off after the first failed slot
    uint32_t failBackoffMaxMs;   // back-off ceiling for repeated failures
};

// Per peer bookkeeping on the coordinator. Zero-initialised = nothing known.
struct PeerState {
    bool ageKnown = false;          // a report carried a precision age
    uint32_t reportedAgeMs = 0;     // age of the last accepted series at report time
    uint32_t reportRxMs = 0;        // local millis() when that report arrived
    bool hadSlot = false;
    uint32_t lastSlotStartMs = 0;
    uint32_t lastSlotEndMs = 0;
    bool outcomePending = false;    // slot over, report with the result not yet seen
    bool lastSlotWasRetry = false;  // the last slot followed a failed one
    uint8_t consecutiveFailures = 0;
    uint32_t eligibleAtMs = 0;      // back-off end (valid when backoffActive)
    bool backoffActive = false;
};

enum class EndReason : uint8_t {
    None = 0,
    SeriesOver,    // owner probed and then stopped
    NoActivity,    // owner never probed
    HardCap,       // maxSlotMs reached while still probing
    OwnerLost,     // owner no longer active (handled by the caller)
};

// The running slot.
struct SlotState {
    bool active = false;
    uint32_t startMs = 0;
    bool probed = false;
    uint32_t lastProbeMs = 0;
    uint32_t probes = 0;
};

struct Stats {
    uint32_t slots = 0;          // slots started
    uint32_t seriesOver = 0;     // ended early because the series was over
    uint32_t noActivity = 0;     // ended because the owner never probed
    uint32_t hardCap = 0;        // ended at maxSlotMs
    uint32_t ownerLost = 0;      // ended because the owner disappeared
    uint32_t succeeded = 0;      // report showed an accepted series from the slot
    uint32_t failed = 0;         // no accepted series from the slot
    uint32_t retrySlots = 0;     // slots given to a node whose previous slot failed
    uint32_t retrySucceeded = 0; // ... of which succeeded
    uint32_t retryFailed = 0;    // ... of which failed
    uint32_t lastSlotMs = 0;     // duration of the last finished slot
    uint64_t totalSlotMs = 0;    // sum of finished slot durations (mean = total/finished)
    uint32_t finishedSlots = 0;
};

inline bool reached(uint32_t now, uint32_t at) {
    return static_cast<int32_t>(now - at) >= 0;
}

// Estimated current age of the node's last accepted series.
inline uint32_t estimatedAgeMs(const PeerState &p, uint32_t now) {
    return p.reportedAgeMs + (now - p.reportRxMs);
}

inline bool due(const PeerState &p, uint32_t now, const Params &c) {
    return !p.ageKnown || estimatedAgeMs(p, now) >= c.refreshAgeMs;
}

inline bool eligible(const PeerState &p, uint32_t now) {
    if (p.outcomePending) return false;
    return !p.backoffActive || reached(now, p.eligibleAtMs);
}

// First failure: no back-off (retry as soon as the outcome is known; a single
// rejected series is common). From the second failure in a row: base, doubling
// up to the ceiling, so a node that keeps failing costs about one slot a minute.
inline uint32_t backoffMs(uint8_t failures, const Params &c) {
    if (failures <= 1) return 0;
    uint32_t b = c.failBackoffBaseMs;
    for (uint8_t i = 2; i < failures && b < c.failBackoffMaxMs; ++i) b *= 2;
    return b < c.failBackoffMaxMs ? b : c.failBackoffMaxMs;
}

// Urgency order: no precision before precision, then the older estimate, then
// the longer wait for a slot (never had one = longest), then the lower index.
inline bool moreUrgent(const PeerState &a, size_t ia, const PeerState &b, size_t ib,
                       uint32_t now) {
    if (a.ageKnown != b.ageKnown) return !a.ageKnown;
    if (a.ageKnown) {
        const uint32_t aa = estimatedAgeMs(a, now), ab = estimatedAgeMs(b, now);
        if (aa != ab) return aa > ab;
    }
    if (a.hadSlot != b.hadSlot) return !a.hadSlot;
    if (a.hadSlot) {
        const uint32_t wa = now - a.lastSlotEndMs, wb = now - b.lastSlotEndMs;
        if (wa != wb) return wa > wb;
    }
    return ia < ib;
}

// Index of the next owner among the active peers, or -1 when nobody is due and
// eligible. `peers[i] == nullptr` marks an unused or inactive table entry.
inline int choose(PeerState *const *peers, size_t n, uint32_t now, const Params &c,
                  Stats &stats) {
    int best = -1;
    for (size_t i = 0; i < n; ++i) {
        const PeerState *p = peers[i];
        if (!p || !eligible(*p, now) || !due(*p, now, c)) continue;
        if (best < 0 || moreUrgent(*p, i, *peers[best], static_cast<size_t>(best), now))
            best = static_cast<int>(i);
    }
    (void)stats;
    return best;
}

inline void onSlotStart(SlotState &s, PeerState &p, uint32_t now, Stats &stats) {
    s = SlotState{};
    s.active = true;
    s.startMs = now;
    p.hadSlot = true;
    p.lastSlotStartMs = now;
    p.lastSlotWasRetry = p.consecutiveFailures > 0;
    p.backoffActive = false;
    ++stats.slots;
    if (p.lastSlotWasRetry) ++stats.retrySlots;
}

// The coordinator answered a probe of the slot owner.
inline void onOwnerProbe(SlotState &s, uint32_t now) {
    if (!s.active) return;
    s.probed = true;
    s.lastProbeMs = now;
    ++s.probes;
}

inline EndReason checkEnd(const SlotState &s, uint32_t now, const Params &c) {
    if (!s.active) return EndReason::None;
    if (reached(now, s.startMs + c.maxSlotMs)) return EndReason::HardCap;
    if (s.probed) return reached(now, s.lastProbeMs + c.idleEndMs) ? EndReason::SeriesOver
                                                                   : EndReason::None;
    return reached(now, s.startMs + c.startTimeoutMs) ? EndReason::NoActivity : EndReason::None;
}

inline uint32_t guardMs(EndReason r, const Params &c) {
    return r == EndReason::SeriesOver ? c.shortGuardMs : c.longGuardMs;
}

inline void recordFailure(PeerState &p, uint32_t now, const Params &c, Stats &stats) {
    p.outcomePending = false;
    if (p.consecutiveFailures < 255) ++p.consecutiveFailures;
    const uint32_t wait = backoffMs(p.consecutiveFailures, c);
    p.backoffActive = wait != 0;
    p.eligibleAtMs = now + wait;
    if (p.lastSlotWasRetry) ++stats.retryFailed;
    ++stats.failed;
}

inline void recordSuccess(PeerState &p, Stats &stats) {
    p.outcomePending = false;
    p.backoffActive = false;
    if (p.lastSlotWasRetry) ++stats.retrySucceeded;
    p.consecutiveFailures = 0;
    ++stats.succeeded;
}

// Bookkeeping when a slot ends. `p` may be null when the owner entry is gone.
inline void onSlotEnd(SlotState &s, PeerState *p, EndReason r, uint32_t now,
                      const Params &c, Stats &stats) {
    if (!s.active) return;
    const uint32_t length = now - s.startMs;
    stats.lastSlotMs = length;
    stats.totalSlotMs += length;
    ++stats.finishedSlots;
    switch (r) {
    case EndReason::SeriesOver: ++stats.seriesOver; break;
    case EndReason::NoActivity: ++stats.noActivity; break;
    case EndReason::HardCap: ++stats.hardCap; break;
    case EndReason::OwnerLost: ++stats.ownerLost; break;
    default: break;
    }
    if (p) {
        p->lastSlotEndMs = now;
        if (r == EndReason::NoActivity || r == EndReason::OwnerLost) {
            // No series was attempted: nothing to wait for.
            recordFailure(*p, now, c, stats);
        } else {
            // A series was attempted. A report taken after the series shows
            // whether it was accepted; it may already have arrived.
            if (p->ageKnown && estimatedAgeMs(*p, now) <= now - s.startMs) recordSuccess(*p, stats);
            else p->outcomePending = true;
        }
    }
    s = SlotState{};
}

// Authenticated NodeSyncReport of this peer. `hasPrecision` = the node has an
// accepted series at all; `ageMs` = its age (whole seconds on the wire).
inline void onPrecisionReport(PeerState &p, bool hasPrecision, uint32_t ageMs,
                              uint32_t now, const Params &c, Stats &stats) {
    p.ageKnown = hasPrecision;
    p.reportedAgeMs = hasPrecision ? ageMs : 0;
    p.reportRxMs = now;
    if (!p.outcomePending) return;
    if (hasPrecision && ageMs <= now - p.lastSlotStartMs) recordSuccess(p, stats);
    else recordFailure(p, now, c, stats);
}

// Periodic: a slot whose outcome never got reported counts as failed.
inline void serviceOutcome(PeerState &p, uint32_t now, const Params &c, Stats &stats) {
    if (p.outcomePending && reached(now, p.lastSlotEndMs + c.outcomeTimeoutMs))
        recordFailure(p, now, c, stats);
}

// Number of active peers that are due now (diagnostics).
inline size_t dueCount(PeerState *const *peers, size_t n, uint32_t now, const Params &c) {
    size_t count = 0;
    for (size_t i = 0; i < n; ++i)
        if (peers[i] && due(*peers[i], now, c)) ++count;
    return count;
}

}  // namespace ClusterSlotScheduler
