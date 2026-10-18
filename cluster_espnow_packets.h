#pragma once
#include <stddef.h>
#include <stdint.h>

// Wire codec for ESP-NOW time samples. No heap, no radio, no system clock changes.
// MAC must be HMAC-SHA256 under an ESP-NOW-specific key derived from the
// authenticated cluster credentials; raw shared secrets must never be logged.
namespace EspNowTimePackets {
constexpr size_t kPayloadSize = 48;  // V2: expanded from 44 to 48 bytes
constexpr size_t kMacSize = 32;
constexpr size_t kPacketSize = kPayloadSize + kMacSize;
constexpr uint8_t kVersion = 2;  // V2: Software timestamp correction
enum class Type : uint8_t { Beacon=1, Probe=2, Reply=3, Final=4, SyncControl=5, NodeSyncReport=6, NodeDriftReport=7, BenchControl=8, BenchDown=9, BenchUp=10, BenchAck=11 };
struct Frame {
  Type type = Type::Beacon;
  uint32_t epoch = 0;
  uint32_t sequence = 0;
  uint64_t coordinatorTag = 0; // keyed/stable hash of the expected coordinator identity
  uint64_t senderBoot = 0;
  int64_t timeUs = 0; // monotonic timestamp; NOT a radio airtime timestamp
  uint32_t nonce = 0;
  uint32_t preSendDelayUs = 0; // V2: measured TX preparation delay (timestamp to esp_now_send completion)
};
// Sign callback returns true only after writing exactly 32 MAC bytes.
using SignMac = bool (*)(const uint8_t*, size_t, uint8_t[32], void*);
// Verify callback must perform a constant-time comparison internally.
using VerifyMac = bool (*)(const uint8_t*, size_t, const uint8_t[32], void*);
bool encode(const Frame &frame, uint8_t out[kPacketSize], SignMac sign, void* user);
bool decode(const uint8_t* bytes, size_t length, Frame &out, VerifyMac verify, void* user);
// Receiver-owned replay state; reset only on an authenticated epoch/boot change.
struct ReplayWindow { uint32_t epoch=0; uint64_t boot=0; uint32_t last=0; bool initialized=false; };
bool acceptSequence(ReplayWindow& state, const Frame& frame);
}
