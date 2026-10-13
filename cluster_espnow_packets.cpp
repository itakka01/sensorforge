#include "cluster_espnow_packets.h"
#include <string.h>
namespace EspNowTimePackets {
namespace {
void put(uint8_t* b, size_t pos, uint64_t value, unsigned count) {
  for (unsigned i=0;i<count;++i) b[pos+i] = static_cast<uint8_t>(value >> (8*i));
}
uint64_t get(const uint8_t* b, size_t pos, unsigned count) {
  uint64_t v=0; for(unsigned i=0;i<count;++i) v |= uint64_t(b[pos+i]) << (8*i); return v;
}
bool typeValid(Type t) { return t==Type::Beacon || t==Type::Probe || t==Type::Reply || t==Type::Final || t==Type::SyncControl || t==Type::NodeSyncReport || t==Type::NodeDriftReport; }
}
bool encode(const Frame &f, uint8_t out[kPacketSize], SignMac sign, void* user) {
  if (!out || !sign || !typeValid(f.type) || !f.epoch || !f.coordinatorTag || !f.senderBoot || !f.sequence) return false;
  memset(out,0,kPacketSize);
  out[0]='S';out[1]='F';out[2]='E';out[3]='N';out[4]=kVersion;out[5]=static_cast<uint8_t>(f.type);
  put(out,8,f.epoch,4);put(out,12,f.sequence,4);put(out,16,f.coordinatorTag,8);
  put(out,24,f.senderBoot,8);put(out,32,static_cast<uint64_t>(f.timeUs),8);put(out,40,f.nonce,4);
  return sign(out,kPayloadSize,out+kPayloadSize,user);
}
bool decode(const uint8_t* b, size_t len, Frame &out, VerifyMac verify, void* user) {
  if (!b || len!=kPacketSize || !verify || b[0]!='S'||b[1]!='F'||b[2]!='E'||b[3]!='N'||b[4]!=kVersion||b[6]!=0||b[7]!=0) return false;
  Type type=static_cast<Type>(b[5]);if(!typeValid(type)) return false;
  // Authentication precedes all state changes and timestamp interpretation.
  if(!verify(b,kPayloadSize,b+kPayloadSize,user)) return false;
  Frame f;f.type=type;f.epoch=static_cast<uint32_t>(get(b,8,4));f.sequence=static_cast<uint32_t>(get(b,12,4));
  f.coordinatorTag=get(b,16,8);f.senderBoot=get(b,24,8);f.timeUs=static_cast<int64_t>(get(b,32,8));f.nonce=static_cast<uint32_t>(get(b,40,4));
  if(!f.epoch||!f.sequence||!f.coordinatorTag||!f.senderBoot) return false;
  out=f;return true;
}
bool acceptSequence(ReplayWindow& state, const Frame& f) {
  if(!f.epoch||!f.senderBoot||!f.sequence) return false;
  // Different epoch/boot requires externally authenticated coordinator identity,
  // and caller must explicitly discard its old state (not auto-accept here).
  if(!state.initialized) {state.epoch=f.epoch;state.boot=f.senderBoot;state.last=f.sequence;state.initialized=true;return true;}
  if(state.epoch!=f.epoch || state.boot!=f.senderBoot || f.sequence<=state.last) return false;
  state.last=f.sequence;return true;
}
}
