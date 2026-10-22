#pragma once
// Coordinator-side recovery slot for the ESP-NOW precision slot rotation.
// Pure decisions shared by cluster_espnow_radio.cpp (serviceSlots) and the host
// test test_slot_recovery.cpp.test. No Arduino/ESP-IDF dependency, no new packets:
// a recovery slot is an ordinary SyncControl grant to one node, inputs are the
// node's existing authenticated NodeSyncReport (precision age, whole seconds).
//
// Problem: with N active nodes one rotation lasts N * (owner slot + guard). From
// four nodes on (70 s) a single rejected series means the next regular slot ends
// after the 120 s precision lifetime, so the node falls back to WiFi time although
// one further measurement opportunity would have been enough.
//
// Invariants (checked by the host test):
//  I1 At most one slot owner at any time (unchanged single-owner state + guard).
//  I2 Never two recovery slots back to back: between two recovery slots there is
//     always at least one regular slot, so the regular rotation advances.
//  I3 A node gets at most one recovery slot between two of its regular slots.
//  I4 A recovery slot shifts all following regular slots by one slot period. It is
//     only inserted when that shift does not make any other node's regular slot end
//     after the precision lifetime that would otherwise have ended in time.
//  I5 Only for a real quality problem: the node reported a precision estimate that
//     is still within the lifetime, a report received after its last own slot shows
//     that this slot produced no accepted series, and its regular slot would end
//     after the lifetime.
// The estimator, quality limits, lifetime and WiFi fallback are NOT touched: a node
// without an accepted series still falls back exactly as before.
#include <stdint.h>
#include <stddef.h>

namespace ClusterSlotRecovery {

struct Params {
    uint32_t slotPeriodMs;   // owner slot + guard interval
    uint32_t ownerSlotMs;    // owner slot length
    uint32_t lifetimeMs;     // precision lifetime on the node
};

// Per peer bookkeeping on the coordinator. Zero-initialised = nothing known.
struct PeerState {
    bool ageKnown = false;          // last report carried a precision age
    uint32_t reportedAgeMs = 0;     // age of the last accepted series at report time
    uint32_t reportRxMs = 0;        // local millis() when that report arrived
    bool hadSlot = false;           // at least one own slot since peer was noted
    uint32_t lastSlotStartMs = 0;   // start of the last own slot (regular or recovery)
    bool recoveryUsed = false;      // I3: reset by the next regular slot
    bool requestCounted = false;    // count each recovery need once per regular cycle
    bool outcomePending = false;    // recovery slot granted, result not yet reported
    uint32_t recoveryStartMs = 0;
};

struct Stats {
    uint32_t requested = 0;   // recovery needs detected (once per node and cycle)
    uint32_t granted = 0;     // recovery slots actually given
    uint32_t deferred = 0;    // needs not served at a slot boundary (I2/I4)
    uint32_t succeeded = 0;   // accepted series reported after a recovery slot
    uint32_t failed = 0;      // no accepted series after a recovery slot
};

inline bool reached(uint32_t now, uint32_t at) {
    return static_cast<int32_t>(now - at) >= 0;
}

// Estimated current age of the node's last accepted series.
inline uint32_t estimatedAgeMs(const PeerState &p, uint32_t now) {
    return p.reportedAgeMs + (now - p.reportRxMs);
}

// True when the precision of a node at rotation position `pos` (0 = next regular
// slot) still holds at the end of its regular slot, optionally shifted.
inline bool regularSlotInTime(uint32_t ageMs, size_t pos, size_t shift, const Params &c) {
    const uint64_t end = static_cast<uint64_t>(ageMs) +
        static_cast<uint64_t>(pos + shift) * c.slotPeriodMs + c.ownerSlotMs;
    return end <= c.lifetimeMs;
}

// I5. `pos` is the node's position in the upcoming regular order.
inline bool needsRecovery(const PeerState &p, size_t pos, uint32_t now, const Params &c) {
    if (pos == 0 || !p.ageKnown || !p.hadSlot || p.recoveryUsed) return false;
    // Outcome of the last own slot is only known from a report sent after it.
    if (!reached(p.reportRxMs, p.lastSlotStartMs + c.ownerSlotMs)) return false;
    const uint32_t age = estimatedAgeMs(p, now);
    if (age <= now - p.lastSlotStartMs) return false;   // last own slot succeeded
    if (age >= c.lifetimeMs) return false;              // already expired: regular path
    return !regularSlotInTime(age, pos, 0, c);
}

// I4: would inserting one slot now push another node past the lifetime?
inline bool shiftIsSafe(PeerState *const *ordered, size_t n, size_t candidate,
                        uint32_t now, const Params &c) {
    for (size_t m = 0; m < n; ++m) {
        if (m == candidate || !ordered[m]->ageKnown) continue;
        const uint32_t age = estimatedAgeMs(*ordered[m], now);
        if (age >= c.lifetimeMs) continue;
        if (regularSlotInTime(age, m, 0, c) && !regularSlotInTime(age, m, 1, c)) return false;
    }
    return true;
}

// Called at a slot boundary with the active peers in regular rotation order
// (ordered[0] = next regular owner). Returns the position to serve as recovery
// slot, or -1 for the regular owner. Deterministic: most urgent (oldest estimate)
// first, ties broken by rotation position.
inline int choose(PeerState *const *ordered, size_t n, bool lastSlotWasRecovery,
                  uint32_t now, const Params &c, Stats &stats) {
    bool anyNeed = false;
    for (size_t k = 1; k < n; ++k) {
        if (!needsRecovery(*ordered[k], k, now, c)) continue;
        anyNeed = true;
        if (!ordered[k]->requestCounted) { ordered[k]->requestCounted = true; ++stats.requested; }
    }
    if (!anyNeed) return -1;
    if (lastSlotWasRecovery) { ++stats.deferred; return -1; }   // I2
    bool tried[16] = {};
    for (;;) {
        int best = -1;
        uint32_t bestAge = 0;
        for (size_t k = 1; k < n && k < 16; ++k) {
            if (tried[k] || !needsRecovery(*ordered[k], k, now, c)) continue;
            const uint32_t age = estimatedAgeMs(*ordered[k], now);
            if (best < 0 || age > bestAge) { best = static_cast<int>(k); bestAge = age; }
        }
        if (best < 0) { ++stats.deferred; return -1; }
        if (shiftIsSafe(ordered, n, static_cast<size_t>(best), now, c)) return best;
        tried[best] = true;
    }
}

// Bookkeeping when a peer becomes slot owner.
inline void onSlotStart(PeerState &p, bool recovery, uint32_t now, Stats &stats) {
    p.hadSlot = true;
    p.lastSlotStartMs = now;
    if (recovery) {
        p.recoveryUsed = true;
        p.outcomePending = true;
        p.recoveryStartMs = now;
        ++stats.granted;
        return;
    }
    // A regular slot closes the recovery cycle; an unresolved recovery counts as failed.
    if (p.outcomePending) { p.outcomePending = false; ++stats.failed; }
    p.recoveryUsed = false;
    p.requestCounted = false;
}

// Bookkeeping for an authenticated NodeSyncReport of this peer.
inline void onPrecisionReport(PeerState &p, bool hasPrecision, uint32_t ageMs,
                              uint32_t now, const Params &c, Stats &stats) {
    p.ageKnown = hasPrecision;
    p.reportedAgeMs = hasPrecision ? ageMs : 0;
    p.reportRxMs = now;
    if (!p.outcomePending) return;
    if (hasPrecision && ageMs <= now - p.recoveryStartMs) {
        p.outcomePending = false; ++stats.succeeded;
    } else if (reached(now, p.recoveryStartMs + c.ownerSlotMs)) {
        p.outcomePending = false; ++stats.failed;
    }
}

}  // namespace ClusterSlotRecovery
