#pragma once
#include <stdint.h>

// Wire-format semantics for a future *signed* SFHT1 cluster command.
// This module does not authenticate a packet or enable any GPIO. Call it only
// after the existing Cluster signature, coordinator ID/epoch, boot nonce,
// remote-IP and replay-sequence checks have succeeded.
namespace ClusterHwTimeProtocol {
static constexpr uint8_t kChannels = 3;
static constexpr int64_t kMinLeadUs = 1500000LL;
static constexpr int64_t kMaxLeadUs = 1800000000LL;
static constexpr uint32_t kMaxSamples = 256;

struct Request {
    uint32_t coordinatorBoot;
    uint32_t coordinatorEpoch;
    uint32_t sequence;
    uint8_t channel;  // 1, 2, 3
    int64_t targetUtcUs;
};

// Rejects malformed requests, a stale/future target, and replayed sequences.
// Comparing seq is valid only within the same validated coordinator boot/epoch.
inline bool validate(const Request &r, int64_t nowUtcUs, uint32_t lastSeq,
                     bool droneModeConfirmed, bool outputPinAvailable) {
    if (!droneModeConfirmed || !outputPinAvailable) return false;
    if (!r.coordinatorBoot || !r.coordinatorEpoch || !r.sequence ||
        r.sequence <= lastSeq || r.channel < 1 || r.channel > kChannels) return false;
    if (nowUtcUs <= 0 || r.targetUtcUs <= nowUtcUs) return false;
    const int64_t lead = r.targetUtcUs - nowUtcUs;
    return lead >= kMinLeadUs && lead <= kMaxLeadUs;
}

// The signed receiving path must record a valid sequence before attempting
// resource allocation, so retransmissions can never start two pulses.
struct ReplayGuard {
    uint32_t boot = 0, epoch = 0, sequence = 0;
    bool accept(const Request &r) {
        if (!r.coordinatorBoot || !r.coordinatorEpoch || !r.sequence) return false;
        if (boot == r.coordinatorBoot && epoch == r.coordinatorEpoch &&
            r.sequence <= sequence) return false;
        boot = r.coordinatorBoot;
        epoch = r.coordinatorEpoch;
        sequence = r.sequence;
        return true;
    }
};
}
