#pragma once
#include <stddef.h>
#include <stdint.h>
// Separate 80-byte wire format, never accepted by the 76-byte production
// ESP-NOW time receiver. Timestamps are software samples, not PHY stamps.
namespace ClusterTimeDiagnosticBeacon {
constexpr size_t kBodyBytes=48, kMacBytes=32, kWireBytes=80;
struct Message {
    uint32_t epoch=0, sequence=0;
    uint64_t senderTag=0, senderBoot=0;
    int64_t monotonicUs=0, utcUs=0;
};
using Sign=bool (*)(const uint8_t*,size_t,uint8_t[32]);
using Verify=bool (*)(const uint8_t*,size_t,const uint8_t[32]);
bool encode(const Message&,uint8_t out[kWireBytes],Sign);
bool decode(const uint8_t*,size_t,Message&,Verify);
}
