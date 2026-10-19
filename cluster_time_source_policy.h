#pragma once
// Pure time-source decisions shared by cluster_espnow_radio.cpp and the host
// regression test (test_time_source_flapping.cpp.test). No Arduino/ESP-IDF
// dependencies: inputs are already HMAC-verified and replay-checked frame fields.
#include <stdint.h>

namespace ClusterTimeSourcePolicy {

// A coordinator may only count production beacons of the broadcaster it leased
// (identity bound to tag + boot in the Grant). Used to confirm broadcaster
// liveness before suppressing its own beacons - never as a time reference.
inline bool isLeasedBroadcasterBeacon(bool leaseActive, uint64_t leaseTag, uint64_t leaseBoot,
                                      uint64_t senderTag, uint64_t senderBoot) {
    return leaseActive && leaseTag && leaseBoot &&
           senderTag == leaseTag && senderBoot == leaseBoot;
}

constexpr uint32_t kBroadcasterConfirmWindowMs = 2000U;

// Coordinator suppresses its own beacon only while a lease is active, the
// bootstrap grace has elapsed and the leased broadcaster was heard recently.
// Any gap > kBroadcasterConfirmWindowMs resumes coordinator beacons (failover).
inline bool coordinatorShouldSuppressBeacon(bool leaseActive, uint32_t suppressionStartMs,
                                            uint32_t lastLeasedBeaconMs, uint32_t nowMs) {
    if (!leaseActive || !suppressionStartMs || nowMs < suppressionStartMs) return false;
    return lastLeasedBeaconMs != 0 &&
           static_cast<uint32_t>(nowMs - lastLeasedBeaconMs) < kBroadcasterConfirmWindowMs;
}

// Beta136: after the last beacon slot was suppressed, the coordinator sends its
// next beacon as soon as suppression no longer holds (confirmation lost or lease
// ended) instead of waiting up to kBroadcastEveryMs for the next regular slot.
// One early beacon per loss; it uses the normal beacon send path.
inline bool coordinatorShouldResumeEarly(bool lastSlotSuppressed, bool suppressNow) {
    return lastSlotSuppressed && !suppressNow;
}

// Node side: a validated beacon whose sender identity differs from the active
// source is a real source change (timing state must be reset).
inline bool isSourceChange(uint64_t activeTag, uint64_t activeBoot, uint32_t activeEpoch,
                           uint64_t tag, uint64_t boot, uint32_t epoch) {
    return activeTag && (activeTag != tag || activeBoot != boot || activeEpoch != epoch);
}

}  // namespace ClusterTimeSourcePolicy
