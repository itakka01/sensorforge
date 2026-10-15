#pragma once
#include <stdint.h>
// A read-only handover simulation. No grant, TX control or production clock access.
// Caller passes only observations whose diagnostic HMAC was already validated.
namespace ClusterTimeShadowHandover {
struct State {
    bool simulatedActive=false;
    uint32_t simulatedActivations=0;
    uint32_t simulatedFallbacks=0;
    uint32_t sourceChanges=0;
    uint64_t observedTag=0, observedBoot=0;
    uint32_t observedEpoch=0;
};
class Monitor {
public:
    void reset() { state_={}; }
    void observe(bool fresh, uint32_t consecutive, uint64_t tag,
                 uint64_t boot, uint32_t epoch) {
        const bool identityValid=tag!=0 && boot!=0 && epoch!=0;
        const bool same=identityValid && state_.observedTag==tag &&
            state_.observedBoot==boot && state_.observedEpoch==epoch;
        if(state_.observedTag && identityValid && !same) {
            ++state_.sourceChanges;
        }
        if(state_.simulatedActive && (!fresh || !same || consecutive<3)) {
            state_.simulatedActive=false;
            ++state_.simulatedFallbacks;
        }
        if(identityValid && !same) {
            state_.observedTag=tag;
            state_.observedBoot=boot;
            state_.observedEpoch=epoch;
        }
        if(fresh && consecutive>=3 && identityValid && !state_.simulatedActive) {
            state_.simulatedActive=true;
            ++state_.simulatedActivations;
        }
    }
    State state() const {return state_;}
private:
    State state_{};
};
} // namespace ClusterTimeShadowHandover
