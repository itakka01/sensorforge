#include "cluster_time_diagnostic_beacon.h"
#include <string.h>
namespace ClusterTimeDiagnosticBeacon {
namespace {
void put32(uint8_t*p,uint32_t x){for(int i=0;i<4;++i)p[i]=uint8_t(x>>(i*8));}
void put64(uint8_t*p,uint64_t x){for(int i=0;i<8;++i)p[i]=uint8_t(x>>(i*8));}
uint32_t get32(const uint8_t*p){uint32_t x=0;for(int i=0;i<4;++i)x|=uint32_t(p[i])<<(8*i);return x;}
uint64_t get64(const uint8_t*p){uint64_t x=0;for(int i=0;i<8;++i)x|=uint64_t(p[i])<<(8*i);return x;}
}
bool encode(const Message&m,uint8_t out[kWireBytes],Sign sign){
    if(!out||!sign||!m.epoch||!m.sequence||!m.senderTag||!m.senderBoot)return false;
    memset(out,0,kWireBytes);
    out[0]='S';out[1]='F';out[2]='D';out[3]='B';out[4]=1;
    put32(out+8,m.epoch);put32(out+12,m.sequence);
    put64(out+16,m.senderTag);put64(out+24,m.senderBoot);
    put64(out+32,static_cast<uint64_t>(m.monotonicUs));
    put64(out+40,static_cast<uint64_t>(m.utcUs));
    return sign(out,kBodyBytes,out+kBodyBytes);
}
bool decode(const uint8_t*wire,size_t len,Message&out,Verify verify){
    if(!wire||len!=kWireBytes||!verify||memcmp(wire,"SFDB",4)!=0||wire[4]!=1||
       wire[5]||wire[6]||wire[7])return false;
    if(!verify(wire,kBodyBytes,wire+kBodyBytes))return false;
    Message x{};x.epoch=get32(wire+8);x.sequence=get32(wire+12);
    x.senderTag=get64(wire+16);x.senderBoot=get64(wire+24);
    x.monotonicUs=static_cast<int64_t>(get64(wire+32));
    x.utcUs=static_cast<int64_t>(get64(wire+40));
    if(!x.epoch||!x.sequence||!x.senderTag||!x.senderBoot)return false;
    out=x;return true;
}
}
