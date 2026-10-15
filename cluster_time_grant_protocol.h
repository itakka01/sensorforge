#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Beta125: authenticated wire format ONLY. No sender permissions, no TX switching.
// Distinct HMAC key/domain MUST be supplied by the caller. A valid MAC proves
// cluster-key possession, NOT ownership of the asserted device identity.
namespace ClusterTimeGrantProtocol {
constexpr size_t kBodyBytes=64;
constexpr size_t kMacBytes=32;
constexpr size_t kWireBytes=kBodyBytes+kMacBytes;
constexpr uint8_t kVersion=1;
enum class Action : uint8_t { Grant=1, Ack=2, Revoke=3 };
struct Message {
    Action action=Action::Grant;
    uint64_t coordinatorTag=0,coordinatorBoot=0;
    uint64_t broadcasterTag=0,broadcasterBoot=0;
    uint32_t coordinatorEpoch=0, transaction=0, sequence=0, leaseMs=0;
};
using Sign=bool (*)(const uint8_t*,size_t,uint8_t[32]);
using Verify=bool (*)(const uint8_t*,size_t,const uint8_t[32]);
inline void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
inline void put64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;++i)p[i]=uint8_t(v>>(8*i));}
inline uint32_t get32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i);return v;}
inline uint64_t get64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(p[i])<<(8*i);return v;}
inline bool valid(const Message &m){
    const uint8_t a=static_cast<uint8_t>(m.action);
    return a>=1 && a<=3 && m.coordinatorTag && m.coordinatorBoot &&
        m.broadcasterTag && m.broadcasterBoot && m.coordinatorEpoch &&
        m.transaction && m.sequence && m.leaseMs<=10000U &&
        (m.action==Action::Revoke || m.leaseMs>=500U);
}
inline bool encode(const Message &m,uint8_t *dst,size_t capacity,Sign sign){
    if(!dst||capacity<kWireBytes||!sign||!valid(m))return false;
    uint8_t body[kBodyBytes]={};
    body[0]='S';body[1]='F';body[2]='T';body[3]='G';
    body[4]=kVersion;body[5]=static_cast<uint8_t>(m.action);
    put64(body+8,m.coordinatorTag);put64(body+16,m.coordinatorBoot);
    put64(body+24,m.broadcasterTag);put64(body+32,m.broadcasterBoot);
    put32(body+40,m.coordinatorEpoch);put32(body+44,m.transaction);
    put32(body+48,m.sequence);put32(body+52,m.leaseMs);
    // Bytes 56..63 reserved, required zero, MAC protected.
    uint8_t mac[32]={};
    if(!sign(body,sizeof(body),mac))return false;
    memcpy(dst,body,sizeof(body));memcpy(dst+kBodyBytes,mac,sizeof(mac));
    memset(mac,0,sizeof(mac));return true;
}
inline bool decode(const uint8_t *src,size_t length,Message &out,Verify verify){
    if(!src||length!=kWireBytes||!verify)return false;
    if(src[0]!='S'||src[1]!='F'||src[2]!='T'||src[3]!='G'||src[4]!=kVersion||
       src[6]||src[7])return false;
    for(size_t i=56;i<64;++i)if(src[i])return false;
    const uint8_t action=src[5];if(action<1||action>3)return false;
    if(!verify(src,kBodyBytes,src+kBodyBytes))return false;
    Message m{};m.action=static_cast<Action>(action);
    m.coordinatorTag=get64(src+8);m.coordinatorBoot=get64(src+16);
    m.broadcasterTag=get64(src+24);m.broadcasterBoot=get64(src+32);
    m.coordinatorEpoch=get32(src+40);m.transaction=get32(src+44);
    m.sequence=get32(src+48);m.leaseMs=get32(src+52);
    if(!valid(m))return false;
    out=m;
    return true;
}
// Replay protection is per *authenticated coordinator identity and boot*.
// The caller must check the actual cluster sender identity separately.
struct Replay {
    uint64_t tag=0,boot=0;
    uint32_t epoch=0,lastSequence=0;
    bool accept(const Message &m){
        if(!valid(m))return false;
        if(!tag){
            tag=m.coordinatorTag;boot=m.coordinatorBoot;epoch=m.coordinatorEpoch;
            lastSequence=m.sequence;return true;
        }
        // A source change requires explicit reset after external re-authentication.
        if(tag!=m.coordinatorTag||boot!=m.coordinatorBoot||epoch!=m.coordinatorEpoch)return false;
        if(m.sequence<=lastSequence)return false;
        lastSequence=m.sequence;return true;
    }
};
} // namespace ClusterTimeGrantProtocol
