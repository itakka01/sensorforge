#include "cluster_espnow_radio.h"
#include "cluster_espnow_packets.h"
#include "cluster_espnow_twoway.h"
#include "cluster_beacon_sync.h"
#include "cluster.h"
#include "cluster_slot_scheduler.h"
#include "loop_load_diag.h"
#include "psram_alloc.h"
#include "board_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <string.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <time.h>
#include <sys/time.h>

namespace ClusterEspNowRadio {
const char *precisionStateName(PrecisionState s) {
    switch (s) {
        case PrecisionState::Valid: return "valid";
        case PrecisionState::Inactive: return "inactive";
        case PrecisionState::Coordinator: return "coordinator";
        case PrecisionState::NoBeacons: return "no_beacons";
        case PrecisionState::TooFewSamples: return "too_few_samples";
        case PrecisionState::SpanTooShort: return "span_too_short";
        case PrecisionState::JitterTooHigh: return "jitter_too_high";
        case PrecisionState::DriftOutOfRange: return "drift_out_of_range";
        case PrecisionState::Stale: return "stale";
        case PrecisionState::NoDelay: return "no_delay";
        case PrecisionState::NoUtc: return "no_utc";
    }
    return "unknown";
}
const char *delaySourceName(DelaySource s) {
    switch (s) {
        case DelaySource::Own: return "own";
        case DelaySource::Cluster: return "cluster";
        default: return "none";
    }
}
}

#if defined(BOARD_HAS_ESP_NOW) && BOARD_HAS_ESP_NOW
namespace ClusterEspNowRadio {
namespace {
// V2 Software Timestamp Correction: Track measured delays for next transmission
static uint32_t lastProbePreSendUs=0;   // Delay from last Probe (for next Probe)
static uint32_t lastReplyPreSendUs=0;   // Delay from current Reply (for Final)

// --- One-way beacon (beta147, ESPNOW_TIME_ARCHITECTURE.md)
constexpr uint32_t kBeaconEveryMs=1000;
// Follow-up waits for the beacon's TX-done callback, at most this long.
constexpr uint32_t kFollowUpTimeoutMs=30;
// Precision time stays valid this long after the newest beacon sample
// (holdover; a quartz error of the model of 1 ppm costs 15 us).
constexpr uint32_t kPrecisionHoldoverMs=15000;
// Own delay calibration is preferred over the cluster median this long.
constexpr uint32_t kOwnDelayMaxAgeMs=3600000;
// A two-way series that finished before the beacon model was valid is
// converted into a delay once the model is valid, if not older than this.
constexpr uint32_t kPendingCalibrationMs=120000;
// Coordinator: node calibrations that enter the cluster median.
constexpr uint32_t kClusterDelayReportMaxAgeMs=60000;
constexpr uint32_t kClusterDelayMaxCalibrationAgeS=3600;
// Node: cluster median from a beacon is used this long.
constexpr uint32_t kClusterDelayRxMaxAgeMs=30000;
constexpr int64_t kNoDelayNs=INT64_MIN;
// Node reports: calibration state (joins the slot schedule) and beacon state.
constexpr uint32_t kReportEveryMs=10000;
constexpr uint32_t kSeriesStatsEveryMs=30000;

// --- Two-way calibration series
// Failed quality checks retry with jitter so competing nodes desynchronize.
constexpr uint32_t kRetryBaseMs=12000;
constexpr uint32_t kRetryJitterMs=12000;
// After an accepted series the node probes again only when the coordinator
// grants a new slot (the schedule decides); this only blocks a second series
// inside the same slot.
constexpr uint32_t kAcceptedPauseMs=30000;
// Single-owner measurement slots.  Short leases and a guard interval
// avoid overlapping permissions even if an authenticated revoke is lost.
constexpr uint32_t kGrantEveryMs=1000;
constexpr uint32_t kGrantLeaseMs=3000;
constexpr uint32_t kOwnerSlotMs=14000;
constexpr uint32_t kSlotGuardMs=3500;
constexpr uint32_t kPeerActiveMs=30000;
constexpr uint32_t kProbeGapMs=45;
constexpr uint32_t kExchangeTimeoutMs=1200;
constexpr uint8_t kInitialSamples=16;
constexpr uint8_t kMaxSamplesPerSeries=64;
constexpr uint32_t kMaxSeriesDurationMs=12000;
constexpr uint8_t kBroadcastMac[6]={255,255,255,255,255,255};
constexpr size_t kQueueSize=24;
struct Queued { uint8_t bytes[EspNowTimePackets::kPacketSize]; uint8_t mac[6]; int64_t rxUs; };
Queued queue[kQueueSize]={};
portMUX_TYPE queueMux=portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t head=0,tail=0;
volatile uint32_t drops=0;
Diagnostics stats;

// Callback-safe TX-done matching. Entries are inserted before esp_now_send().
// Match by destination MAC and FIFO; no per-packet sequence is supplied to TX callbacks.
constexpr size_t kTxPendingCapacity=24;
// Every esp_now_send() registers an entry, so callbacks of other frames to the
// same MAC cannot consume another frame's entry.
constexpr uint8_t kTxClassOther=0, kTxClassBeacon=1;
struct TxPending { bool used=false; uint8_t mac[6]={}; int64_t enqueueUs=0; uint8_t frameClass=kTxClassOther; };
TxPending txPending[kTxPendingCapacity];
portMUX_TYPE txMux=portMUX_INITIALIZER_UNLOCKED;
// TX-done callback of the last beacon, matched by its enqueue time.
int64_t beaconTxDoneEnqueueUs=0, beaconTxDoneUs=0;
void txTimingReset() {
    portENTER_CRITICAL(&txMux);
    for(auto &entry:txPending)entry={};
    beaconTxDoneEnqueueUs=beaconTxDoneUs=0;
    portEXIT_CRITICAL(&txMux);
}
// Registers a TX-latency entry before esp_now_send(). Returns the slot (or capacity on overflow).
size_t txPendingRegister(const uint8_t mac[6],uint8_t frameClass,int64_t enqueueUs) {
    size_t slot=kTxPendingCapacity;
    portENTER_CRITICAL(&txMux);
    for(size_t i=0;i<kTxPendingCapacity;++i) {
        if(txPending[i].used && enqueueUs-txPending[i].enqueueUs>2000000LL)txPending[i].used=false;
        if(!txPending[i].used && slot==kTxPendingCapacity)slot=i;
    }
    if(slot!=kTxPendingCapacity) {
        txPending[slot].used=true;
        memcpy(txPending[slot].mac,mac,6);
        txPending[slot].enqueueUs=enqueueUs;
        txPending[slot].frameClass=frameClass;
    }
    portEXIT_CRITICAL(&txMux);
    return slot;
}
// Releases an entry whose esp_now_send() was not issued or failed.
void txPendingCancel(size_t slot,int64_t enqueueUs) {
    if(slot==kTxPendingCapacity)return;
    portENTER_CRITICAL(&txMux);
    if(txPending[slot].used && txPending[slot].enqueueUs==enqueueUs)txPending[slot].used=false;
    portEXIT_CRITICAL(&txMux);
}
void onSentMac(const uint8_t* mac,esp_now_send_status_t status) {
    const int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&txMux);
    size_t match=kTxPendingCapacity;
    int64_t oldest=INT64_MAX;
    for(size_t i=0;i<kTxPendingCapacity;++i)
        if(txPending[i].used && (!mac || memcmp(txPending[i].mac,mac,6)==0) &&
           txPending[i].enqueueUs<oldest){match=i;oldest=txPending[i].enqueueUs;}
    if(match!=kTxPendingCapacity) {
        txPending[match].used=false;
        if(txPending[match].frameClass==kTxClassBeacon && status==ESP_NOW_SEND_SUCCESS) {
            beaconTxDoneEnqueueUs=txPending[match].enqueueUs;
            beaconTxDoneUs=now;
        }
    }
    portEXIT_CRITICAL(&txMux);
}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,5,0)
void onSent(const wifi_tx_info_t* info,esp_now_send_status_t status) {
    onSentMac(info?info->des_addr:nullptr,status);
}
#else
void onSent(const uint8_t* mac,esp_now_send_status_t status) {
    onSentMac(mac,status);
}
#endif
bool initialized=false, wasCoordinator=false;
uint8_t boundChannel=0;
uint32_t nextBeaconMs=0,nextSeriesMs=0,nextProbeMs=0,txSequence=0,lastEpoch=0;
uint32_t captureGuardUntilMs=0;
uint32_t permissionUntilMs=0, nextGrantMs=0;
bool sendFrame(EspNowTimePackets::Frame& f,const uint8_t mac[6],
               int64_t* txStampOut=nullptr, bool replyStamp=false);
struct SlotPeer { uint8_t mac[6]={}; uint32_t boot=0; uint32_t seenMs=0;
                  ClusterSlotScheduler::PeerState sched; };
// Calibration slot scheduling (cluster_slot_scheduler.h): a slot ends when the
// owner's series is over; the next owner is the most urgent due node.
// Per-node tables sized for a 100-node cluster plus headroom for nodes that
// rebooted (new boot ID) or left. They live in PSRAM (allocated in start(),
// see ensureNodeTables()); without PSRAM they stay empty and no node is
// scheduled. Only loopTask touches them, never the receive callback.
constexpr size_t kNodeTableCapacity=kNodeReportCapacity;
constexpr size_t kSlotPeerCapacity=kNodeTableCapacity;
SfPsramArray<SlotPeer> slotPeers;
// Diagnostics only: last time a node was seen while every slot was taken.
uint32_t slotTableFullLastMs=0;
int16_t slotOwner=-1;
uint32_t slotStartedMs=0, slotGuardUntilMs=0;
uint32_t slotGrantsSent=0, slotRotations=0;
// A calibration is refreshed every 10 min; after 30 min without one it counts
// as expired (diagnostics). Precision time does not depend on it: the beacon
// model carries the time, the calibration only the constant path delay.
constexpr uint32_t kSlotLifetimeMs=1800000;
constexpr uint32_t kSlotRefreshAgeMs=600000;
constexpr uint32_t kSlotStartTimeoutMs=3000;    // node starts ~1 s after its first grant
constexpr uint32_t kSlotIdleEndMs=1500;         // > one lost exchange (1200 + 45 ms) + margin
constexpr uint32_t kSlotShortGuardMs=0;        // next owner starts ~1 s after its grant
constexpr uint32_t kSlotOutcomeTimeoutMs=12000; // node reports right after a series, else every 10 s
constexpr ClusterSlotScheduler::Params kSlotParams{
    kSlotLifetimeMs, kSlotRefreshAgeMs, kSlotStartTimeoutMs, kSlotIdleEndMs,
    kOwnerSlotMs, kSlotShortGuardMs, kSlotGuardMs, kSlotOutcomeTimeoutMs, 5000, 60000};
static_assert(kSlotIdleEndMs > kExchangeTimeoutMs + kProbeGapMs,
              "one lost exchange must not end the slot");
// The short guard relies on a node not probing again right after its series:
// accepted -> kAcceptedPauseMs, rejected/aborted -> >= kRetryBaseMs, both beyond a grant lease.
static_assert(kRetryBaseMs > kGrantLeaseMs + kSlotShortGuardMs,
              "a finished owner must not probe again within its lease");
static_assert(kAcceptedPauseMs > kOwnerSlotMs,
              "an accepted owner must not start a second series in its slot");
static_assert(kSlotStartTimeoutMs > kGrantEveryMs + 1000,
              "a node starts its series about 1 s after the first grant");
static_assert(kSlotOutcomeTimeoutMs > kReportEveryMs,
              "a lost immediate report is covered by the periodic one");
ClusterSlotScheduler::SlotState slotState;
// Unicast ESP-NOW peers added for replies. ESP-IDF holds at most
// ESP_NOW_MAX_TOTAL_PEER_NUM peers (broadcast included); when the list is full
// the least recently used reply peer is replaced.
struct UnicastPeer { bool used=false; uint8_t mac[6]={}; uint32_t lastUseMs=0; };
constexpr size_t kUnicastPeerSlots=ESP_NOW_MAX_TOTAL_PEER_NUM-2;  // broadcast + one spare
UnicastPeer unicastPeers[kUnicastPeerSlots];
uint32_t unicastPeerEvictions=0;
ClusterSlotScheduler::Stats slotStats;
uint32_t slotLastStartMs=0;
uint64_t slotPeriodTotalMs=0;
uint32_t slotPeriodCount=0;
bool slotPeerActive(const SlotPeer &p, uint32_t now) {
    return p.boot && uint32_t(now-p.seenMs)<=kPeerActiveMs;
}
void sendSlotControl(uint32_t epoch,uint64_t tag,uint32_t boot,
                     uint32_t targetBoot,uint32_t leaseMs);
// Ends the running slot without judging its outcome (capture pause, benchmark).
void slotAbortWithoutOutcome(uint32_t guardUntilMs) {
    if(slotOwner>=0) {
        slotPeers[slotOwner].sched.lastSlotEndMs=millis();
        ++slotRotations;
    }
    slotState={};
    slotOwner=-1;
    slotGuardUntilMs=guardUntilMs;
}
void noteSlotPeer(const uint8_t mac[6], uint32_t boot) {
    if(!boot) return;
    const uint32_t now=millis();
    for(auto &p:slotPeers) {
        if(p.boot && memcmp(p.mac,mac,6)==0) {
            // A different boot on the same MAC is a different owner.
            if(p.boot!=boot && slotOwner==(&p-slotPeers)) slotAbortWithoutOutcome(now+kSlotGuardMs);
            if(p.boot!=boot)p.sched={};
            p.boot=boot; p.seenMs=now; return;
        }
    }
    for(auto &p:slotPeers) if(!slotPeerActive(p,now) && (&p-slotPeers)!=slotOwner) {
        p={};memcpy(p.mac,mac,6);p.boot=boot;p.seenMs=now;return;
    }
    slotTableFullLastMs=now?now:1;
}
bool slotIsOwner(const uint8_t mac[6], uint32_t boot) {
    return slotOwner>=0 && slotPeers[slotOwner].boot==boot &&
           memcmp(slotPeers[slotOwner].mac,mac,6)==0 &&
           uint32_t(millis()-slotStartedMs)<kOwnerSlotMs;
}
// The coordinator answered a probe of the current slot owner.
void slotNoteOwnerProbe() {
    ClusterSlotScheduler::onOwnerProbe(slotState,millis());
}
void sendSlotControl(uint32_t epoch,uint64_t tag,uint32_t boot,
                     uint32_t targetBoot,uint32_t leaseMs) {
    EspNowTimePackets::Frame f;
    f.type=EspNowTimePackets::Type::SyncControl;
    f.epoch=epoch;f.coordinatorTag=tag;f.senderBoot=boot;
    f.nonce=targetBoot;f.timeUs=leaseMs;
    if(sendFrame(f,kBroadcastMac) && leaseMs) ++slotGrantsSent;
}
void endSlot(uint32_t epoch,uint64_t tag,uint32_t boot,ClusterSlotScheduler::EndReason reason) {
    const uint32_t now=millis();
    if(slotOwner>=0) {
        sendSlotControl(epoch,tag,boot,slotPeers[slotOwner].boot,0);
        ClusterSlotScheduler::onSlotEnd(slotState,&slotPeers[slotOwner].sched,reason,now,
                                        kSlotParams,slotStats);
        ++slotRotations;
    }
    slotState={};
    slotOwner=-1;
    slotGuardUntilMs=now+ClusterSlotScheduler::guardMs(reason,kSlotParams);
}
// The benchmark takes the radio: revoke the owner, do not judge the slot,
// keep the long guard so its lease expires.
void revokeSlotWithoutOutcome(uint32_t epoch,uint64_t tag,uint32_t boot) {
    if(slotOwner>=0) sendSlotControl(epoch,tag,boot,slotPeers[slotOwner].boot,0);
    slotAbortWithoutOutcome(millis()+kSlotGuardMs);
}
// Coordinator: calibration age from a node's authenticated NodeSyncReport.
void slotNotePrecisionReport(uint32_t boot,bool hasPrecision,uint32_t ageMs) {
    for(auto &p:slotPeers) if(p.boot==boot) {
        ClusterSlotScheduler::onPrecisionReport(p.sched,hasPrecision,ageMs,millis(),
                                                kSlotParams,slotStats);
        return;
    }
}
void serviceSlots(uint32_t now,uint32_t epoch,uint64_t tag,uint32_t boot) {
    for(auto &p:slotPeers)
        if(p.boot) ClusterSlotScheduler::serviceOutcome(p.sched,now,kSlotParams,slotStats);
    if(slotOwner>=0) {
        const ClusterSlotScheduler::EndReason reason=
            !slotPeerActive(slotPeers[slotOwner],now) ? ClusterSlotScheduler::EndReason::OwnerLost :
            ClusterSlotScheduler::checkEnd(slotState,now,kSlotParams);
        if(reason!=ClusterSlotScheduler::EndReason::None) endSlot(epoch,tag,boot,reason);
    }
    if(slotOwner<0) {
        if(static_cast<int32_t>(now-slotGuardUntilMs)<0) return;
        ClusterSlotScheduler::PeerState *peers[kSlotPeerCapacity];
        const size_t peerCount=slotPeers.size();
        for(size_t i=0;i<peerCount;++i)
            peers[i]=slotPeerActive(slotPeers[i],now) ? &slotPeers[i].sched : nullptr;
        const int next=ClusterSlotScheduler::choose(peers,peerCount,now,kSlotParams,slotStats);
        if(next>=0) {
            slotOwner=static_cast<int16_t>(next);
            ClusterSlotScheduler::onSlotStart(slotState,slotPeers[slotOwner].sched,now,slotStats);
            if(slotLastStartMs){slotPeriodTotalMs+=uint32_t(now-slotLastStartMs);++slotPeriodCount;}
            slotLastStartMs=now;
            slotStartedMs=now;nextGrantMs=now;
        }
    }
    if(slotOwner>=0 && static_cast<int32_t>(now-nextGrantMs)>=0) {
        nextGrantMs=now+kGrantEveryMs;
        sendSlotControl(epoch,tag,boot,slotPeers[slotOwner].boot,kGrantLeaseMs);
    }
}
uint32_t localBootNonceForControl=0;
uint64_t lastTag=0;
// Probe/Reply/Final are bound to the coordinator identity of the beacons.
struct Pending {
    bool active=false;
    uint32_t nonce=0;
    int64_t t1=0,t2=0,t4ReplyRx=0;
    uint32_t startedMs=0;
    uint64_t expectedBoot=0;    // coordinator boot from the beacons (0 = not known yet)
    uint8_t expectedMac[6]={};  // coordinator MAC from the beacons (zero = not known yet)
} pending;
uint32_t nonceSeed=0;
// Transport benchmark: 100 downlink packets and 100 uplink packets per peer.
// The result is *transport* delivery/ACK latency, not an absolute one-way time.
constexpr uint32_t kBenchRunLimitMs=125000;
BenchmarkState bench;
uint8_t benchMac[3][6]={};
uint32_t benchPhaseStart=0, benchLastSend=0, benchCtrlLastMs=0;
uint32_t benchQuietUntilMs=0;
bool benchCtrlAck=false;
uint16_t benchNextSeq=0;
uint32_t benchTxUs[100]={};
uint32_t benchNodeBoot=0, benchNodeRun=0, benchNodeNextMs=0;
uint16_t benchNodeNextSeq=0;
uint32_t benchNodeGapMs=50;
uint8_t benchNodePhase=0;
uint32_t benchNodeDeadlineMs=0;
uint32_t benchNodeUpAck=0,benchNodeDownReceived=0;
uint32_t benchNodeTxUs[100]={},benchNodeAckRttUs[100]={};
uint8_t benchNodeUpAckSeen[100]={},benchNodeDownSeen[100]={};
// Per-node 100-exchange ring; survives completion until the next run/reboot.
struct BenchExchange { uint32_t run=0; uint16_t seq=0; ClusterEspNowTwoWay::Exchange x; };
BenchExchange benchRaw[100];
uint16_t benchRawCount=0;
uint16_t benchTimeProbes=0;
uint32_t benchTimeNextMs=0;

bool benchSend(EspNowTimePackets::Type type,uint32_t nonce,int64_t payload,const uint8_t *mac) {
    EspNowTimePackets::Frame f;
    f.type=type; f.epoch=lastEpoch; f.coordinatorTag=lastTag;
    f.senderBoot=localBootNonceForControl; f.nonce=nonce; f.timeUs=payload;
    return sendFrame(f,mac);
}
int64_t benchControlValue(uint32_t phase);
void benchClearNode(){benchNodePhase=0;benchNodeBoot=0;benchNodeNextSeq=0;benchNodeUpAck=0;benchNodeDownReceived=0;pending={};}
void benchAbortInternal(bool mark){
    if(bench.running){
        // Revoke the test on every selected node before entering a capture hold.
        for(uint8_t i=0;i<bench.peerCount;++i)
            benchSend(EspNowTimePackets::Type::BenchControl,bench.peers[i].boot,
                      benchControlValue(0),kBroadcastMac);
        bench.running=false;bench.finished=true;bench.aborted=mark;bench.completedMs=millis();
    }
    benchClearNode();
}
int64_t benchControlValue(uint32_t phase){
    return static_cast<int64_t>((uint64_t(bench.runId)<<32)|((bench.intervalMs&0xffffu)<<16)|(phase&0xffffu));
}
void benchControlPhase(uint8_t phase,uint32_t now){
    bench.phase=phase;benchPhaseStart=now;benchCtrlLastMs=now;benchCtrlAck=false;
    benchSend(EspNowTimePackets::Type::BenchControl,bench.peers[bench.peerIndex].boot,
              benchControlValue(phase),kBroadcastMac);
}
void benchAdvance(uint32_t now) {
    benchLastSend=now-bench.intervalMs;
    benchNextSeq=0;
    for(auto &t:benchTxUs)t=0;
    // Reset local sync slot ownership, don't let the normal scheduler compete.
    slotAbortWithoutOutcome(now+kSlotGuardMs);
    benchControlPhase(1,now);
}
void benchTick(uint32_t now){
    if(!bench.running)return;
    if(uint32_t(now-bench.startedMs)>kBenchRunLimitMs){benchAbortInternal(true);return;}
    if(bench.phase==0){
        if(static_cast<int32_t>(now-benchQuietUntilMs)>=0)benchAdvance(now);
        return;
    }
    if(!benchCtrlAck){
        // Explicit signed acknowledgement avoids mistaking a missing phase command
        // for 100 lost test packets. Retry until accepted or mark the run aborted.
        if(uint32_t(now-benchPhaseStart)>4500UL){benchAbortInternal(true);return;}
        if(uint32_t(now-benchCtrlLastMs)>=400UL){
            benchCtrlLastMs=now;
            benchSend(EspNowTimePackets::Type::BenchControl,bench.peers[bench.peerIndex].boot,
                      benchControlValue(bench.phase),kBroadcastMac);
        }
        return;
    }
    if(bench.phase==1){
        if(benchNextSeq<100 && uint32_t(now-benchLastSend)>=bench.intervalMs){
            const uint16_t idx=benchNextSeq++;
            benchLastSend=now;
            benchTxUs[idx]=static_cast<uint32_t>(esp_timer_get_time());
            benchSend(EspNowTimePackets::Type::BenchDown,idx,
                      bench.peers[bench.peerIndex].boot,kBroadcastMac);
        }
        if(benchNextSeq==100 && uint32_t(now-benchPhaseStart)>bench.intervalMs*100+3000UL){
            benchControlPhase(2,now);
        }
    } else if(bench.phase==2 && uint32_t(now-benchPhaseStart)>bench.intervalMs*100+3000UL){
        benchControlPhase(3,now);
    } else if(bench.phase==3 && uint32_t(now-benchPhaseStart)>bench.intervalMs*100+4000UL){
        benchSend(EspNowTimePackets::Type::BenchControl,bench.peers[bench.peerIndex].boot,
                  benchControlValue(0),kBroadcastMac);
        ++bench.peerIndex;
        if(bench.peerIndex>=bench.peerCount){bench.running=false;bench.finished=true;bench.completedMs=now;}
        else benchAdvance(now);
    }
}
void benchNodeTick(uint32_t now){
    if(!benchNodePhase)return;
    if(static_cast<int32_t>(now-benchNodeDeadlineMs)>=0){benchClearNode();return;}
    if(benchNodePhase==3){
        // A real four-timestamp exchange, independent of the calibration.
        if(pending.active && uint32_t(now-pending.startedMs)>kExchangeTimeoutMs){pending={};++stats.exchangeTimeouts;}
        if(benchTimeProbes<100 && !pending.active && static_cast<int32_t>(now-benchTimeNextMs)>=0){
            if(!nonceSeed)nonceSeed=static_cast<uint32_t>(esp_timer_get_time())^localBootNonceForControl;
            nonceSeed=nonceSeed*1664525u+1013904223u;
            EspNowTimePackets::Frame probe;
            probe.type=EspNowTimePackets::Type::Probe;probe.epoch=lastEpoch;
            probe.coordinatorTag=lastTag;probe.senderBoot=localBootNonceForControl;
            probe.nonce=nonceSeed?nonceSeed:1;
            if(sendFrame(probe,kBroadcastMac,&probe.timeUs,false)){
                pending.active=true;pending.nonce=probe.nonce;
                // V2: Apply software timestamp correction with CURRENT Probe's measured delay
                pending.t1=probe.timeUs + static_cast<int64_t>(lastProbePreSendUs);
                pending.startedMs=now;++stats.probesSent;
                ++benchTimeProbes;
            }
            benchTimeNextMs=now+benchNodeGapMs;
        }
        return;
    }
    if(benchNodePhase!=2)return;
    if(benchNodeNextSeq<100 && static_cast<int32_t>(now-benchNodeNextMs)>=0){
        const uint16_t seq=benchNodeNextSeq++;
        benchNodeTxUs[seq]=static_cast<uint32_t>(esp_timer_get_time());
        benchSend(EspNowTimePackets::Type::BenchUp,seq,benchNodeRun,kBroadcastMac);
        benchNodeNextMs=now+benchNodeGapMs;
    }
}

// Boot history for random boot nonces / epochs (replay protection).
struct BootHistory { uint64_t boot=0; uint32_t epoch=0; uint32_t lastSeenMs=0; };
struct PeerReplay {
    bool used=false;
    uint8_t mac[6]={};
    EspNowTimePackets::ReplayWindow replay;
    BootHistory bootHistory[3]; // last 3 (boot, epoch) sessions (both random, not sequential)
    uint8_t activeBootIndex=0;
};
// One entry per sender MAC for the whole radio session (never evicted, so replay
// state of a known node is never lost). Sized like the slot table (PSRAM).
SfPsramArray<PeerReplay> peerReplay;
ClusterEspNowTwoWay::Exchange exchanges[kMaxSamplesPerSeries];
uint8_t seriesCount=0;
uint32_t seriesStartedMs=0;
uint32_t nextReportMs=0, nextBeaconReportMs=0;
bool reportSoon=false;
bool seriesPrecisionAccepted=false;
// One entry per reporting node boot ID (PSRAM); when full, the entry heard
// from longest ago is replaced, so stale boot IDs go first.
struct RemoteReport {
    NodeSyncReport report;
    uint32_t receivedAt=0, beaconReceivedAt=0;
    // Long-term statistics (see NodeSyncReport): accumulated precise time,
    // first beacon report and the moment of the last precision loss.
    uint64_t preciseMs=0;
    uint32_t firstBeaconAt=0, lastLossAt=0;
};
SfPsramArray<RemoteReport> remoteReports;

// Last 128 beacons for offline analysis (PSRAM).
constexpr size_t kTraceCapacity=128;
SfPsramArray<BeaconTraceEntry> beaconTrace;
size_t traceCount=0, traceWrite=0;
uint64_t traceBoot=0;
void traceAppend(const BeaconTraceEntry &e, uint64_t coordinatorBoot) {
    if(!beaconTrace.size()) return;
    if(traceCount && traceBoot!=coordinatorBoot) traceCount=traceWrite=0;
    traceBoot=coordinatorBoot;
    beaconTrace[traceWrite]=e;
    traceWrite=(traceWrite+1)%beaconTrace.size();
    if(traceCount<beaconTrace.size()) ++traceCount;
}

void ensureNodeTables() {
    if(!slotPeers.allocate(kNodeTableCapacity) ||
       !peerReplay.allocate(kNodeTableCapacity) ||
       !remoteReports.allocate(kNodeTableCapacity) ||
       !beaconTrace.allocate(kTraceCapacity))
        Serial.println("[ESPNOW] PSRAM node tables unavailable");
}
uint32_t reportCount=0;
uint32_t retrySerial=0;
uint32_t qualityRetryDelayMs() {
    // Different node boots and successive failures use different retry phases.
    // Only affects retry scheduling, not clock estimates or quality thresholds.
    const uint32_t x=localBootNonceForControl ^ (++retrySerial*0x9e3779b9UL);
    const uint32_t mixed=(x ^ (x>>16))*0x7feb352dUL;
    return kRetryBaseMs + ((mixed ^ (mixed>>15)) % kRetryJitterMs);
}

// --- Coordinator beacon transmitter
struct FollowUpPending {
    bool active=false;
    uint32_t sequence=0;
    int64_t enqueueUs=0, stampUs=0, utcAtStampUs=0;
    uint32_t sentMs=0;
} followUp;
int64_t clusterDelayNsTx=kNoDelayNs;

// --- Node beacon receiver
ClusterBeaconSync::Estimator beaconModel;
ClusterBeaconSync::DelayCalibration ownDelay;
uint32_t ownDelayLastMs=0;
uint64_t beaconSourceBoot=0;
uint8_t beaconSourceMac[6]={};
bool beaconSourceKnown=false;
struct RecentBeacon { bool used=false; uint32_t sequence=0; uint64_t boot=0; int64_t rxUs=0; };
RecentBeacon recentBeacons[4];
uint8_t recentBeaconWrite=0;
int64_t utcCoordUs=0, utcAtCoordUs=0;   // newest coordinator UTC mapping
uint32_t lastBeaconSampleMs=0;
int64_t clusterDelayNsRx=kNoDelayNs;
uint32_t clusterDelayRxMs=0;
struct PendingCalibration { bool active=false; double thetaUs=0; int64_t nodeRefUs=0; uint32_t atMs=0; } pendingCalibration;
uint32_t calibrationsDiscarded=0;
uint32_t sourceChanges=0;

// Delay used for the coordinator time now (node).
DelaySource currentDelay(uint32_t nowMs, double &delayUs) {
    if(ownDelay.known() && ownDelayLastMs && uint32_t(nowMs-ownDelayLastMs)<=kOwnDelayMaxAgeMs) {
        delayUs=ownDelay.value();
        return DelaySource::Own;
    }
    if(clusterDelayNsRx!=kNoDelayNs && clusterDelayRxMs && uint32_t(nowMs-clusterDelayRxMs)<=kClusterDelayRxMaxAgeMs) {
        delayUs=static_cast<double>(clusterDelayNsRx)/1000.0;
        return DelaySource::Cluster;
    }
    delayUs=0;
    return DelaySource::None;
}
PrecisionState nodePrecisionState(uint32_t nowMs, double &delayUs, DelaySource &source) {
    delayUs=0; source=DelaySource::None;
    if(!initialized) return PrecisionState::Inactive;
    if(wasCoordinator) return PrecisionState::Coordinator;
    const auto &m=beaconModel.model();
    if(!m.samples) return PrecisionState::NoBeacons;
    if(!m.valid) {
        switch(m.invalid) {
            case ClusterBeaconSync::Invalid::SpanTooShort: return PrecisionState::SpanTooShort;
            case ClusterBeaconSync::Invalid::JitterTooHigh: return PrecisionState::JitterTooHigh;
            case ClusterBeaconSync::Invalid::DriftOutOfRange: return PrecisionState::DriftOutOfRange;
            default: return PrecisionState::TooFewSamples;
        }
    }
    if(!lastBeaconSampleMs || uint32_t(nowMs-lastBeaconSampleMs)>kPrecisionHoldoverMs) return PrecisionState::Stale;
    source=currentDelay(nowMs,delayUs);
    if(source==DelaySource::None) return PrecisionState::NoDelay;
    if(!utcAtCoordUs) return PrecisionState::NoUtc;
    return PrecisionState::Valid;
}
// Converts an accepted two-way series into the path delay of this link.
bool resolveCalibration(double thetaUs, int64_t nodeRefUs) {
    if(!beaconModel.model().valid ||
       beaconModel.model().samples<ClusterBeaconSync::kMinCalibrationSamples) return false;
    const double d=thetaUs-beaconModel.envelopeOffsetUs(nodeRefUs);
    if(!std::isfinite(d) || d<ClusterBeaconSync::kDelayMinUs || d>ClusterBeaconSync::kDelayMaxUs) {
        ++calibrationsDiscarded;
        Serial.printf("[ESPNOW] calibration discarded: delay %.1f us out of range\n", d);
        return true;  // handled (discarded)
    }
    ownDelay.add(d,nodeRefUs);
    ownDelayLastMs=millis();
    return true;
}
void resetBeaconReceiver() {
    beaconModel.reset();
    beaconSourceBoot=0;beaconSourceKnown=false;memset(beaconSourceMac,0,6);
    for(auto &b:recentBeacons)b={};
    recentBeaconWrite=0;
    utcCoordUs=utcAtCoordUs=0;
    lastBeaconSampleMs=0;
    clusterDelayNsRx=kNoDelayNs;clusterDelayRxMs=0;
    pendingCalibration={};
}

uint32_t successfulSeries=0, failedSeries=0, nextSeriesStatsReportMs=0;
uint8_t lastSeriesFailure=0; // 1=invalid/insufficient; 2=spread; 3=RTT; 4=series deadline
void seriesFailed(uint8_t reason) {
    ++failedSeries;lastSeriesFailure=reason;reportSoon=true;
}
void acceptEstimate(const ClusterEspNowTwoWay::Estimate &e) {
    seriesPrecisionAccepted=false;
    // A wide offset range indicates RF/software timing jitter.
    if(!e.valid || e.accepted<8){seriesFailed(1);return;}
    if(!std::isfinite(e.spreadUs) || e.spreadUs>250.0){seriesFailed(2);return;}
    if(!std::isfinite(e.medianRttUs) || e.medianRttUs>30000.0){seriesFailed(3);return;}
    lastSeriesFailure=0;
    ++successfulSeries;
    seriesPrecisionAccepted=true;
    reportSoon=true;
    // Path delay of this link = two-way offset minus beacon envelope offset.
    if(!resolveCalibration(e.offsetUs,e.nodeReferenceUs))
        pendingCalibration={true,e.offsetUs,e.nodeReferenceUs,millis()};
    else pendingCalibration={};
}

uint64_t idTag(const char* id) {
    uint64_t x=14695981039346656037ULL;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(id);p && *p;++p)
        x=(x^*p)*1099511628211ULL;
    return x ? x : 1;
}
void onReceive(const esp_now_recv_info_t* info,const uint8_t* data,int length) {
    if(!info||!info->src_addr||!data)return;
    // Time frames only; HMAC and type are checked in the cluster loop.
    if(length!=static_cast<int>(EspNowTimePackets::kPacketSize) ||
       data[0]!='S' || data[1]!='F' || data[2]!='E' || data[3]!='N')return;
    const int64_t rx=esp_timer_get_time();
    portENTER_CRITICAL(&queueMux);
    uint8_t next=static_cast<uint8_t>((head+1)%kQueueSize);
    if(next==tail)++drops;
    else {
        memcpy(queue[head].bytes,data,EspNowTimePackets::kPacketSize);
        memcpy(queue[head].mac,info->src_addr,6);
        queue[head].rxUs=rx;head=next;
    }
    portEXIT_CRITICAL(&queueMux);
}
bool pop(Queued &v) {
    bool ok=false;
    portENTER_CRITICAL(&queueMux);
    if(tail!=head){v=queue[tail];tail=static_cast<uint8_t>((tail+1)%kQueueSize);ok=true;}
    portEXIT_CRITICAL(&queueMux);
    return ok;
}
void clearQueue(){portENTER_CRITICAL(&queueMux);head=tail=0;drops=0;portEXIT_CRITICAL(&queueMux);}
bool start(uint8_t channel) {
    ensureNodeTables();
    if(esp_now_init()!=ESP_OK)return false;
    if(esp_now_register_recv_cb(onReceive)!=ESP_OK){esp_now_deinit();return false;}
    txTimingReset();
    if(esp_now_register_send_cb(onSent)!=ESP_OK){esp_now_deinit();return false;}
    esp_now_peer_info_t peer={};
    memcpy(peer.peer_addr,kBroadcastMac,6);peer.channel=0;peer.ifidx=WIFI_IF_STA;
    if(esp_now_add_peer(&peer)!=ESP_OK){esp_now_deinit();return false;}
    initialized=true;boundChannel=channel;stats.active=true;stats.channel=channel;
    const uint32_t now=millis();
    nextBeaconMs=now+500;nextSeriesMs=now+2000;nextGrantMs=now;
    permissionUntilMs=0;nextReportMs=now+1500UL;nextBeaconReportMs=now+3000UL;
    for(auto &p:slotPeers)p={};slotOwner=-1;slotState={};
    slotStartedMs=0;slotGuardUntilMs=now+kSlotGuardMs;
    slotGrantsSent=slotRotations=0;
    slotStats={};slotLastStartMs=0;slotPeriodTotalMs=0;slotPeriodCount=0;
    for(auto &u:unicastPeers)u={};
    followUp={};
    resetBeaconReceiver();
    return true;
}
uint32_t nextSequence(){++txSequence;if(!txSequence)++txSequence;return txSequence;}
bool sendFrame(EspNowTimePackets::Frame& f,const uint8_t mac[6],
               int64_t* txStampOut, bool replyStamp) {
    f.sequence=nextSequence();
    // Prepare callback bookkeeping BEFORE sampling the transmission timestamp.
    // Otherwise queue bookkeeping unnecessarily biases t1/t3.
    const int64_t queuedAtUs=esp_timer_get_time();
    const size_t slot=txPendingRegister(mac,kTxClassOther,queuedAtUs);
    // V2 Software Timestamp Correction:
    // - Probe: carries t1 (early) + delay from PREVIOUS Probe
    // - Reply: carries t2 (RX timestamp, no correction needed)
    // - Final: carries t3 (early) + delay from CURRENT Reply
    // Timestamps captured before HMAC for authentication, delays measured after send.
    int64_t stampUs=0;
    if(txStampOut) {
        stampUs=esp_timer_get_time();
        *txStampOut=stampUs;
        if(!replyStamp)f.timeUs=stampUs;
    }
    // Final carries the Reply's measured delay; a Probe cannot carry its own.
    if(f.type==EspNowTimePackets::Type::Final) f.preSendDelayUs=lastReplyPreSendUs;
    else if(f.type==EspNowTimePackets::Type::Probe || f.type==EspNowTimePackets::Type::Reply) f.preSendDelayUs=0;
    uint8_t b[EspNowTimePackets::kPacketSize];
    if(!EspNowTimePackets::encode(f,b,clusterEspNowTimeSign,nullptr)) {
        txPendingCancel(slot,queuedAtUs);
        return false;
    }
    if(esp_now_send(mac,b,sizeof(b))!=ESP_OK) {
        txPendingCancel(slot,queuedAtUs);
        return false;
    }
    // V2: Measure delay AFTER esp_now_send() completes
    if(txStampOut) {
        const int64_t preSendUs=esp_timer_get_time()-stampUs;
        if(preSendUs>=0 && preSendUs<=UINT32_MAX) {
            const uint32_t delta=static_cast<uint32_t>(preSendUs);
            if(f.type==EspNowTimePackets::Type::Probe) lastProbePreSendUs=delta;
            else if(f.type==EspNowTimePackets::Type::Reply) lastReplyPreSendUs=delta;
        }
    }
    ++stats.sent;return true;
}
bool ensurePeer(const uint8_t mac[6]) {
    const uint32_t now=millis();
    UnicastPeer *entry=nullptr;
    for(auto &u:unicastPeers) if(u.used && memcmp(u.mac,mac,6)==0){entry=&u;break;}
    if(esp_now_is_peer_exist(mac)) {
        if(entry) entry->lastUseMs=now;
        return true;
    }
    if(!entry) {
        for(auto &u:unicastPeers) if(!u.used){entry=&u;break;}
        if(!entry) {
            // Full: replace the least recently used reply peer.
            entry=&unicastPeers[0];
            for(auto &u:unicastPeers) if(int32_t(u.lastUseMs-entry->lastUseMs)<0) entry=&u;
            if(esp_now_is_peer_exist(entry->mac)) esp_now_del_peer(entry->mac);
            ++unicastPeerEvictions;
        }
    }
    esp_now_peer_info_t p={};memcpy(p.peer_addr,mac,6);p.channel=0;p.ifidx=WIFI_IF_STA;
    if(esp_now_add_peer(&p)!=ESP_OK){entry->used=false;return false;}
    entry->used=true;memcpy(entry->mac,mac,6);entry->lastUseMs=now;
    return true;
}

// --- Coordinator: beacon + follow-up
// Cluster median of the node calibrations, sent with every beacon so that a
// node without its own calibration can use it.
void updateClusterDelay(uint32_t nowMs) {
    double values[ClusterBeaconSync::kMedianMax];
    size_t n=0;
    for(const auto &rr:remoteReports) {
        const auto &r=rr.report;
        if(!r.bootNonce || !r.hasBeaconReport || !r.ownDelayKnown) continue;
        if(uint32_t(nowMs-rr.beaconReceivedAt)>kClusterDelayReportMaxAgeMs) continue;
        if(r.ownDelayAgeS>kClusterDelayMaxCalibrationAgeS) continue;
        if(n<ClusterBeaconSync::kMedianMax) values[n++]=r.ownDelayUs;
    }
    stats.clusterDelayContributors=static_cast<uint32_t>(n);
    if(!n){clusterDelayNsTx=kNoDelayNs;stats.clusterDelayKnown=false;stats.clusterDelayUs=0;return;}
    const double d=ClusterBeaconSync::median(values,n);
    clusterDelayNsTx=static_cast<int64_t>(llround(d*1000.0));
    stats.clusterDelayKnown=true;stats.clusterDelayUs=d;
}
// The beacon itself carries no time: its timestamp is taken immediately
// before esp_now_send() (after the HMAC) and delivered by the follow-up.
void sendBeacon(uint32_t epoch,uint64_t tag,uint32_t boot,uint32_t nowMs) {
    updateClusterDelay(nowMs);
    EspNowTimePackets::Frame b;
    b.type=EspNowTimePackets::Type::Beacon;b.epoch=epoch;
    b.coordinatorTag=tag;b.senderBoot=boot;
    b.sequence=nextSequence();
    b.auxUs=clusterDelayNsTx;
    uint8_t packet[EspNowTimePackets::kPacketSize];
    if(!EspNowTimePackets::encode(b,packet,clusterEspNowTimeSign,nullptr)) return;
    // Wall-clock/monotonic pair for the UTC mapping of the stamp.
    const int64_t monoBefore=esp_timer_get_time();
    struct timeval tv={};
    const bool clockOk=gettimeofday(&tv,nullptr)==0 && tv.tv_sec>=1577836800LL;
    const int64_t monoAfter=esp_timer_get_time();
    const int64_t enqueueUs=esp_timer_get_time();
    const size_t slot=txPendingRegister(kBroadcastMac,kTxClassBeacon,enqueueUs);
    const int64_t stampUs=esp_timer_get_time();
    if(esp_now_send(kBroadcastMac,packet,sizeof(packet))!=ESP_OK) {
        txPendingCancel(slot,enqueueUs);
        return;
    }
    ++stats.sent;++stats.beaconsSent;
    followUp.active=true;followUp.sequence=b.sequence;
    followUp.enqueueUs=enqueueUs;followUp.stampUs=stampUs;followUp.sentMs=nowMs;
    followUp.utcAtStampUs=(clockOk && monoAfter-monoBefore<2000) ?
        static_cast<int64_t>(tv.tv_sec)*1000000LL+tv.tv_usec+(stampUs-(monoBefore+(monoAfter-monoBefore)/2)) : 0;
}
void serviceFollowUp(uint32_t epoch,uint64_t tag,uint32_t boot,uint32_t nowMs) {
    if(!followUp.active) return;
    int64_t doneEnqueue=0,doneUs=0;
    portENTER_CRITICAL(&txMux);
    doneEnqueue=beaconTxDoneEnqueueUs;doneUs=beaconTxDoneUs;
    portEXIT_CRITICAL(&txMux);
    const bool done=doneEnqueue==followUp.enqueueUs && doneUs>=followUp.stampUs;
    if(!done && uint32_t(nowMs-followUp.sentMs)<kFollowUpTimeoutMs) return;
    EspNowTimePackets::Frame f;
    f.type=EspNowTimePackets::Type::FollowUp;f.epoch=epoch;
    f.coordinatorTag=tag;f.senderBoot=boot;
    f.nonce=followUp.sequence;
    f.timeUs=followUp.stampUs;
    f.auxUs=followUp.utcAtStampUs;
    int32_t txPath=-1;
    if(done) {
        const int64_t path=doneUs-followUp.stampUs;
        txPath=path>INT32_MAX ? INT32_MAX : static_cast<int32_t>(path);
        const uint32_t p=static_cast<uint32_t>(txPath);
        stats.beaconTxPathLastUs=p;
        if(!stats.beaconTxPathMinUs || p<stats.beaconTxPathMinUs) stats.beaconTxPathMinUs=p;
        if(p>stats.beaconTxPathMaxUs) stats.beaconTxPathMaxUs=p;
    } else ++stats.followUpsWithoutTxDone;
    f.preSendDelayUs=txPath<0 ? UINT32_MAX : static_cast<uint32_t>(txPath);
    followUp.active=false;
    if(sendFrame(f,kBroadcastMac)) ++stats.followUpsSent;
    BeaconTraceEntry t;
    t.sequence=f.nonce;t.localUs=f.timeUs;t.coordUs=f.timeUs;t.txPathUs=txPath;
    traceAppend(t,boot);
}

// --- Node: beacon receiver
// A beacon/follow-up from the current coordinator; a new coordinator boot
// starts a new model (the monotonic clock of the coordinator restarted).
bool beaconSourceAccept(const EspNowTimePackets::Frame &f,const uint8_t mac[6]) {
    if(f.coordinatorTag!=lastTag) return false;
    if(beaconSourceKnown && (f.senderBoot!=beaconSourceBoot || memcmp(beaconSourceMac,mac,6)!=0)) {
        Serial.printf("[ESPNOW] beacon source changed (boot %llx -> %llx), model restarted\n",
                      (unsigned long long)beaconSourceBoot,(unsigned long long)f.senderBoot);
        resetBeaconReceiver();
        ++sourceChanges;
    }
    if(!beaconSourceKnown) {
        beaconSourceKnown=true;beaconSourceBoot=f.senderBoot;memcpy(beaconSourceMac,mac,6);
    }
    return true;
}
void onBeacon(const EspNowTimePackets::Frame &f,int64_t rxUs,uint32_t nowMs) {
    ++stats.beaconsReceived;
    RecentBeacon &b=recentBeacons[recentBeaconWrite];
    b.used=true;b.sequence=f.sequence;b.boot=f.senderBoot;b.rxUs=rxUs;
    recentBeaconWrite=static_cast<uint8_t>((recentBeaconWrite+1)%4);
    if(f.auxUs!=kNoDelayNs && f.auxUs>=0 &&
       f.auxUs<=static_cast<int64_t>(ClusterBeaconSync::kDelayMaxUs*1000.0)) {
        clusterDelayNsRx=f.auxUs;clusterDelayRxMs=nowMs?nowMs:1;
    }
}
void onFollowUp(const EspNowTimePackets::Frame &f,uint32_t nowMs) {
    ++stats.followUpsReceived;
    RecentBeacon *b=nullptr;
    for(auto &x:recentBeacons) if(x.used && x.sequence==f.nonce && x.boot==f.senderBoot){b=&x;break;}
    if(!b){++stats.followUpsUnmatched;return;}
    b->used=false;
    if(f.auxUs>=1577836800000000LL && f.auxUs<4102444800000000LL) {
        utcCoordUs=f.timeUs;utcAtCoordUs=f.auxUs;
    }
    ClusterBeaconSync::Sample s;
    s.nodeUs=b->rxUs;s.coordUs=f.timeUs;
    s.txPathUs=f.preSendDelayUs==UINT32_MAX ? -1 :
        static_cast<int32_t>(std::min<uint32_t>(f.preSendDelayUs,INT32_MAX));
    BeaconTraceEntry t;
    t.sequence=f.nonce;t.localUs=s.nodeUs;t.coordUs=s.coordUs;t.txPathUs=s.txPathUs;
    if(beaconModel.model().valid) {
        t.residualUs=static_cast<int32_t>(llround(std::max(-2e9,std::min(2e9,beaconModel.residualUs(s)))));
        double delay=0;
        if(currentDelay(nowMs,delay)!=DelaySource::None)
            t.estCoordUs=s.nodeUs+static_cast<int64_t>(llround(beaconModel.envelopeOffsetUs(s.nodeUs)+delay));
    }
    const ClusterBeaconSync::AddResult r=beaconModel.add(s);
    t.result=static_cast<uint8_t>(r);
    traceAppend(t,f.senderBoot);
    if(r==ClusterBeaconSync::AddResult::Accepted || r==ClusterBeaconSync::AddResult::Reset)
        lastBeaconSampleMs=nowMs?nowMs:1;
    if(r==ClusterBeaconSync::AddResult::Reset) ++sourceChanges;
    if(pendingCalibration.active) {
        if(uint32_t(nowMs-pendingCalibration.atMs)>kPendingCalibrationMs) {
            pendingCalibration={};++calibrationsDiscarded;
        } else if(resolveCalibration(pendingCalibration.thetaUs,pendingCalibration.nodeRefUs)) {
            pendingCalibration={};
        }
    }
}

// Reports share the HMAC/replay protection and coordinator epoch of probes.
// timeUs is two packed 32-bit values, NOT a timestamp for report frames.
RemoteReport *reportSlot(uint32_t boot) {
    const size_t n=remoteReports.size();
    if(!boot || !n) return nullptr;
    size_t slot=n;
    for(size_t i=0;i<n;++i)if(remoteReports[i].report.bootNonce==boot){slot=i;break;}
    if(slot==n){for(size_t i=0;i<n;++i)if(!remoteReports[i].report.bootNonce){slot=i;break;}}
    if(slot==n){
        // Full: replace the node heard from longest ago (any report type).
        const uint32_t nowMs=millis();
        auto silentMs=[nowMs](const RemoteReport &r){
            const uint32_t a=uint32_t(nowMs-r.receivedAt);
            const uint32_t b=r.beaconReceivedAt ? uint32_t(nowMs-r.beaconReceivedAt) : a;
            return a<b ? a : b;
        };
        slot=0;
        for(size_t i=1;i<n;++i)
            if(silentMs(remoteReports[i])>silentMs(remoteReports[slot]))slot=i;
    }
    RemoteReport &rr=remoteReports[slot];
    if(rr.report.bootNonce!=boot){rr={};rr.report.bootNonce=boot;}
    return &rr;
}
void acceptBeaconSyncReport(const EspNowTimePackets::Frame &f) {
    RemoteReport *rr=reportSlot(static_cast<uint32_t>(f.senderBoot));
    if(!rr) return;
    auto &r=rr->report;
    const uint32_t nowMs=millis();
    const bool precise=(f.nonce&2U)!=0;
    if(rr->beaconReceivedAt) {
        const uint32_t dt=nowMs-rr->beaconReceivedAt;
        const bool gap=dt>kReportGapMs;
        // The interval since the last report keeps that report's state;
        // a silent node is not precise from the end of the gap allowance.
        if(r.precisionValid) rr->preciseMs+=gap ? kReportGapMs : dt;
        if(r.precisionValid && (gap || !precise)) {
            ++r.precisionLosses;
            rr->lastLossAt=gap ? rr->beaconReceivedAt+kReportGapMs : nowMs;
        }
    } else {
        rr->firstBeaconAt=nowMs;
    }
    r.hasBeaconReport=true;
    r.beaconModelValid=(f.nonce&1U)!=0;
    r.precisionValid=precise;
    r.delaySource=static_cast<DelaySource>((f.nonce>>2)&3U);
    r.precisionState=static_cast<PrecisionState>((f.nonce>>4)&0x0fU);
    r.beaconSamples=static_cast<uint8_t>((f.nonce>>8)&0xffU);
    r.beaconAgeMs=((f.nonce>>16)&0xffffU)*100UL;
    r.driftMilliPpm=static_cast<int32_t>(f.preSendDelayUs);
    r.ownDelayKnown=f.timeUs!=kNoDelayNs;
    r.ownDelayUs=r.ownDelayKnown ? static_cast<double>(f.timeUs)/1000.0 : 0;
    const uint64_t aux=static_cast<uint64_t>(f.auxUs);
    r.ownDelayAgeS=static_cast<uint32_t>(aux>>48);
    r.beaconJitterUs=static_cast<double>(static_cast<uint32_t>(aux))/10.0;
    if(precise && r.beaconJitterUs>r.maxJitterUs) r.maxJitterUs=r.beaconJitterUs;
    rr->beaconReceivedAt=nowMs;
    if(!rr->receivedAt) rr->receivedAt=rr->beaconReceivedAt;
    ++reportCount;
}
// NodeSyncReport flags in nonce: 0x100 own delay calibrated (bits 16..31 =
// age in s), 0x200 not calibrated yet, 0x1000 series counters in timeUs.
constexpr uint32_t kReportCalibrated=0x100U, kReportUncalibrated=0x200U, kReportSeriesCounters=0x1000U;
void acceptNodeReport(const EspNowTimePackets::Frame &f) {
    const uint32_t boot=static_cast<uint32_t>(f.senderBoot);
    RemoteReport *rr=reportSlot(boot);
    if(!rr) return;
    auto &r=rr->report;
    rr->receivedAt=millis();++reportCount;
    if((f.nonce&kReportSeriesCounters)!=0) {
        const uint64_t bits=static_cast<uint64_t>(f.timeUs);
        r.successfulSeries=static_cast<uint32_t>(bits>>32);
        r.rejectedSeries=static_cast<uint32_t>(bits);
    } else if((f.nonce&kReportUncalibrated)!=0) {
        r.precisionAccepted=false;
        r.sampleAgeMs=0;
        slotNotePrecisionReport(boot,false,0);
    } else if((f.nonce&kReportCalibrated)!=0) {
        r.precisionAccepted=true;
        r.sampleAgeMs=(f.nonce>>16)*1000UL;
        slotNotePrecisionReport(boot,true,r.sampleAgeMs);
    }
}
void sendSeriesStatistics(uint32_t epoch,uint64_t tag,uint32_t boot) {
    EspNowTimePackets::Frame f;
    f.type=EspNowTimePackets::Type::NodeSyncReport;
    f.epoch=epoch;f.coordinatorTag=tag;f.senderBoot=boot;
    f.timeUs=static_cast<int64_t>((static_cast<uint64_t>(successfulSeries)<<32)|failedSeries);
    f.nonce=kReportSeriesCounters;
    sendFrame(f,kBroadcastMac);
}
// Calibration state; joins the coordinator's slot schedule.
void sendNodeReports(uint32_t epoch,uint64_t tag,uint32_t boot) {
    EspNowTimePackets::Frame f;
    f.type=EspNowTimePackets::Type::NodeSyncReport;
    f.epoch=epoch;f.coordinatorTag=tag;f.senderBoot=boot;
    if(!ownDelay.known() || !ownDelayLastMs) {
        f.nonce=kReportUncalibrated;
    } else {
        const uint32_t ageS=static_cast<uint32_t>(millis()-ownDelayLastMs)/1000UL;
        f.nonce=(std::min<uint32_t>(65535U,ageS)<<16) | kReportCalibrated;
    }
    sendFrame(f,kBroadcastMac);
}
void sendBeaconSyncReport(uint32_t epoch,uint64_t tag,uint32_t boot) {
    const uint32_t nowMs=millis();
    double delay=0;
    DelaySource source=DelaySource::None;
    const PrecisionState state=nodePrecisionState(nowMs,delay,source);
    const auto &m=beaconModel.model();
    EspNowTimePackets::Frame f;
    f.type=EspNowTimePackets::Type::BeaconSyncReport;
    f.epoch=epoch;f.coordinatorTag=tag;f.senderBoot=boot;
    f.timeUs=ownDelay.known() ? static_cast<int64_t>(llround(ownDelay.value()*1000.0)) : kNoDelayNs;
    const uint32_t ageTenths=lastBeaconSampleMs ?
        std::min<uint32_t>(0xffffU,uint32_t(nowMs-lastBeaconSampleMs)/100U) : 0xffffU;
    f.nonce=(m.valid?1U:0U) | (state==PrecisionState::Valid?2U:0U) |
        ((static_cast<uint32_t>(source)&3U)<<2) | ((static_cast<uint32_t>(state)&0x0fU)<<4) |
        (static_cast<uint32_t>(m.samples)<<8) | (ageTenths<<16);
    const double driftMilli=std::max(-2147483647.0,std::min(2147483647.0,m.driftPpm*1000.0));
    f.preSendDelayUs=static_cast<uint32_t>(static_cast<int32_t>(llround(driftMilli)));
    const uint64_t delayAge=ownDelayLastMs ? std::min<uint64_t>(0xffffU,uint32_t(nowMs-ownDelayLastMs)/1000U) : 0xffffU;
    const uint64_t jitter=static_cast<uint64_t>(llround(std::max(0.0,std::min(4294967295.0,m.jitterUs*10.0))));
    f.auxUs=static_cast<int64_t>((delayAge<<48)|jitter);
    sendFrame(f,kBroadcastMac);
}
bool acceptPeer(const uint8_t mac[6],const EspNowTimePackets::Frame &f) {
    PeerReplay* slot=nullptr;
    for(auto &p:peerReplay)if(p.used&&memcmp(p.mac,mac,6)==0){slot=&p;break;}
    if(!slot)for(auto &p:peerReplay)if(!p.used){slot=&p;p.used=true;memcpy(p.mac,mac,6);break;}
    if(!slot)return false;
    const uint32_t now=millis();
    // Every time frame is bound to the locally confirmed coordinator epoch.
    // An epoch change stops/restarts the radio, so legitimate frames always
    // carry lastEpoch. Equality only - epochs are random.
    if(!lastEpoch || f.epoch!=lastEpoch) return false;
    // Boot IDs and epochs are esp_random(): a session is (boot, epoch). Accept
    // the active session and never-seen sessions; reject recently seen, no
    // longer active sessions (replay of an older boot or epoch context).
    int8_t knownBootIndex = -1;
    for(uint8_t i=0; i<3; ++i) {
        if(slot->bootHistory[i].boot == f.senderBoot && slot->bootHistory[i].boot != 0 &&
           slot->bootHistory[i].epoch == f.epoch) {
            knownBootIndex = i;
            break;
        }
    }
    if(slot->replay.initialized) {
        const bool sessionChanged = (slot->replay.boot != f.senderBoot) ||
                                    (slot->replay.epoch != f.epoch);
        if(sessionChanged) {
            if(knownBootIndex >= 0 && knownBootIndex != slot->activeBootIndex) return false;
            uint8_t oldestIndex = 0;
            uint32_t oldestTime = slot->bootHistory[0].lastSeenMs;
            for(uint8_t i=1; i<3; ++i) {
                if(slot->bootHistory[i].lastSeenMs < oldestTime) {
                    oldestTime = slot->bootHistory[i].lastSeenMs;
                    oldestIndex = i;
                }
            }
            slot->activeBootIndex = oldestIndex;
            slot->bootHistory[oldestIndex].boot = f.senderBoot;
            slot->bootHistory[oldestIndex].epoch = f.epoch;
            slot->bootHistory[oldestIndex].lastSeenMs = now;
            slot->replay = {};
        }
    } else {
        slot->activeBootIndex = 0;
        slot->bootHistory[0].boot = f.senderBoot;
        slot->bootHistory[0].epoch = f.epoch;
        slot->bootHistory[0].lastSeenMs = now;
    }
    if(knownBootIndex == slot->activeBootIndex)
        slot->bootHistory[slot->activeBootIndex].lastSeenMs = now;
    return EspNowTimePackets::acceptSequence(slot->replay,f);
}
void resetTiming(){
    // Replay windows are kept: protection persists across source changes.
    pending={};seriesCount=0;seriesStartedMs=0;nextProbeMs=0;
}
void finishSample(int64_t t3) {
    const int64_t t4=pending.t4ReplyRx;
    if(!pending.active||!pending.t2||!t4||t3<pending.t2||t4<pending.t1)return;
    if(benchNodePhase==3){
        if(benchRawCount<100){
            BenchExchange &r=benchRaw[benchRawCount];
            r.run=benchNodeRun;r.seq=benchTimeProbes;r.x={pending.t1,pending.t2,t3,t4};
            ++benchRawCount;
        }
        pending={};++stats.exchangesComplete;
        return; // Do not update the calibration.
    }
    if(seriesCount<kMaxSamplesPerSeries)
       exchanges[seriesCount++]={pending.t1,pending.t2,t3,t4};
    pending={};++stats.exchangesComplete;
    if(seriesCount>=kInitialSamples) {
        // Fixed 16-sample series (~0.72 s): drift inside the series stays small.
        auto e=ClusterEspNowTwoWay::estimate(exchanges,seriesCount);
        acceptEstimate(e);
        seriesCount=0;seriesStartedMs=0;
        nextSeriesMs=millis()+(seriesPrecisionAccepted ? kAcceptedPauseMs : qualityRetryDelayMs());
    }
}
bool replyFromCoordinator(const EspNowTimePackets::Frame &f,const uint8_t mac[6]) {
    if(f.coordinatorTag!=lastTag) return false;
    if(pending.expectedBoot && f.senderBoot!=pending.expectedBoot) return false;
    bool macKnown=false;
    for(int i=0;i<6;++i) if(pending.expectedMac[i]) macKnown=true;
    return !macKnown || memcmp(pending.expectedMac,mac,6)==0;
}
}
bool syncPermissionValid() {
    return initialized && !wasCoordinator && permissionUntilMs != 0 &&
           static_cast<int32_t>(millis()-permissionUntilMs)<0 && !capturePaused();
}
namespace {
void revokePermissionBroadcast() {
    if (!initialized || !wasCoordinator || !lastEpoch || !lastTag) return;
    EspNowTimePackets::Frame f;
    f.type=EspNowTimePackets::Type::SyncControl;
    f.epoch=lastEpoch;f.coordinatorTag=lastTag;
    f.senderBoot=localBootNonceForControl;
    f.timeUs=0; // Zero = revoke. Positive = lease duration in milliseconds.
    f.nonce=(slotOwner>=0 ? slotPeers[slotOwner].boot : 0);
    sendFrame(f,kBroadcastMac);
}
}
void pauseForCapture(uint32_t holdMs) {
    benchAbortInternal(true);
    // Timer arithmetic remains unambiguous for holds shorter than 2^31 ms.
    if (holdMs > 1805000UL) holdMs = 1805000UL;
    if (holdMs < 1000UL) holdMs = 1000UL;
    const uint32_t until = millis() + holdMs;
    if (captureGuardUntilMs == 0 ||
        static_cast<int32_t>(until - captureGuardUntilMs) > 0)
        captureGuardUntilMs = until;
    // Coordinator notifies nodes immediately; expiry also protects against lost packets.
    revokePermissionBroadcast();
    if(wasCoordinator && slotOwner>=0) {
        // Capture has priority; not a failure of the node.
        slotAbortWithoutOutcome(captureGuardUntilMs+kSlotGuardMs);
    }
    permissionUntilMs=0;
    // Drop incomplete samples immediately, keep the beacon model for holdover.
    pending = {};
    seriesCount = 0;seriesStartedMs=0;
    nextSeriesMs = captureGuardUntilMs + 1000UL;
    nextProbeMs = 0;
    followUp = {};
}
bool capturePaused() {
    return captureGuardUntilMs != 0 &&
           static_cast<int32_t>(millis() - captureGuardUntilMs) < 0;
}
void stop() {
    benchAbortInternal(true);
    benchRawCount=0;
    captureGuardUntilMs = 0;
    permissionUntilMs=0;localBootNonceForControl=0;
    if(initialized){esp_now_deinit();initialized=false;}
    txTimingReset();
    boundChannel=0;lastEpoch=0;lastTag=0;stats.active=false;
    stats.beaconsSent=stats.followUpsSent=stats.followUpsWithoutTxDone=0;
    stats.beaconTxPathLastUs=stats.beaconTxPathMinUs=stats.beaconTxPathMaxUs=0;
    stats.clusterDelayKnown=false;stats.clusterDelayUs=0;stats.clusterDelayContributors=0;
    stats.beaconsReceived=stats.followUpsReceived=stats.followUpsUnmatched=0;
    resetTiming();clearQueue();
    // Monotonic generation: pending captures become invalid after a radio restart.
    ++sourceChanges;
    for(auto &p:slotPeers)p={};slotOwner=-1;slotState={};
    slotStartedMs=slotGuardUntilMs=0;slotGrantsSent=slotRotations=0;
    slotStats={};slotLastStartMs=0;slotPeriodTotalMs=0;slotPeriodCount=0;
    successfulSeries=failedSeries=0;lastSeriesFailure=0;nextSeriesStatsReportMs=0;
    for(auto &r:remoteReports)r={};reportCount=0;
    followUp={};clusterDelayNsTx=kNoDelayNs;
    resetBeaconReceiver();
    beaconModel.reset();beaconModel.clearStats();
    ownDelay.reset();ownDelayLastMs=0;calibrationsDiscarded=0;
    traceCount=traceWrite=0;traceBoot=0;
}
bool precisionUtcNow(uint32_t epoch, const char *coordinatorId,
                    int64_t &utcUs, uint32_t &sampleAgeMs, double &jitterUs) {
    if (!initialized || !coordinatorId || wasCoordinator || epoch != lastEpoch ||
        idTag(coordinatorId) != lastTag) return false;
    const uint32_t nowMs=millis();
    double delay=0;
    DelaySource source=DelaySource::None;
    if(nodePrecisionState(nowMs,delay,source)!=PrecisionState::Valid) return false;
    const int64_t nodeNow=esp_timer_get_time();
    // Integer parts separately: 64-bit timer values must not lose microseconds.
    const double fraction=beaconModel.envelopeOffsetUs(nodeNow)+delay;
    const int64_t coordNow=nodeNow+static_cast<int64_t>(llround(fraction));
    const int64_t target=utcAtCoordUs+(coordNow-utcCoordUs);
    if(target<1577836800000000LL || target>4102444800000000LL) return false;
    utcUs=target;
    sampleAgeMs=uint32_t(nowMs-lastBeaconSampleMs);
    jitterUs=beaconModel.model().jitterUs;
    return true;
}
size_t nodeSyncReports(NodeSyncReport* out,size_t capacity){
    if(!out||!capacity||!initialized||!wasCoordinator)return 0;
    size_t count=0;
    const uint32_t nowMs=millis();
    for(size_t i=0;i<remoteReports.size()&&count<capacity;++i){
        if(!remoteReports[i].report.bootNonce)continue;
        out[count]=remoteReports[i].report;
        out[count].receivedAgeMs=static_cast<uint32_t>(nowMs-remoteReports[i].receivedAt);
        const RemoteReport &rr=remoteReports[i];
        out[count].beaconReceivedAgeMs=rr.beaconReceivedAt ?
            static_cast<uint32_t>(nowMs-rr.beaconReceivedAt) : UINT32_MAX;
        if(rr.beaconReceivedAt) {
            // Include the running interval since the newest report.
            const uint32_t open=nowMs-rr.beaconReceivedAt;
            uint64_t precise=rr.preciseMs;
            if(rr.report.precisionValid) precise+=open<kReportGapMs ? open : kReportGapMs;
            out[count].monitoredS=(nowMs-rr.firstBeaconAt)/1000UL;
            out[count].preciseS=static_cast<uint32_t>(precise/1000ULL);
            uint32_t losses=rr.report.precisionLosses, lossAt=rr.lastLossAt;
            if(rr.report.precisionValid && open>kReportGapMs) {
                ++losses;  // currently silent
                lossAt=rr.beaconReceivedAt+kReportGapMs;
            }
            out[count].precisionLosses=losses;
            out[count].lastLossAgeS=losses ? (nowMs-lossAt)/1000UL : UINT32_MAX;
        }
        ++count;
    }
    return count;
}
size_t beaconTraceCount(){return traceCount;}
bool beaconTraceAt(size_t index, BeaconTraceEntry &out){
    if(index>=traceCount || !beaconTrace.size())return false;
    const size_t n=beaconTrace.size();
    out=beaconTrace[(traceWrite+n-traceCount+index)%n];
    return true;
}
uint64_t beaconTraceCoordinatorBoot(){return traceBoot;}
Diagnostics diagnostics(){Diagnostics out=stats;
    const uint32_t now=millis();
    out.capturePauseRemainingMs=capturePaused() ? uint32_t(captureGuardUntilMs-now) : 0;
    portENTER_CRITICAL(&queueMux);out.dropped=drops;portEXIT_CRITICAL(&queueMux);
    // Beacon receiver (nodes; zero on the coordinator).
    const auto &m=beaconModel.model();
    const auto &bs=beaconModel.stats();
    out.beaconSamplesAccepted=bs.accepted;
    out.beaconSamplesTxPathLate=bs.txPathLate;
    out.beaconSamplesEarly=bs.early;
    out.beaconModelResets=bs.resets;
    out.beaconModelValid=m.valid;
    out.beaconModelSamples=m.samples;
    out.beaconModelSelected=m.selected;
    out.beaconLastAgeMs=lastBeaconSampleMs ? uint32_t(now-lastBeaconSampleMs) : 0;
    out.beaconModelSpanMs=static_cast<uint32_t>(m.spanUs/1000);
    out.beaconOffsetUs=m.valid ? beaconModel.envelopeOffsetUs(m.refNodeUs) : 0;
    out.beaconDriftPpm=m.driftPpm;
    out.beaconJitterUs=m.jitterUs;
    out.beaconTxPathMinNodeUs=beaconModel.txPathMinUs();
    double delay=0;
    DelaySource source=DelaySource::None;
    out.precisionState=nodePrecisionState(now,delay,source);
    out.delaySource=wasCoordinator ? DelaySource::None : currentDelay(now,delay);
    out.delayUsedUs=out.delaySource==DelaySource::None ? 0 : delay;
    out.precisionValid=out.precisionState==PrecisionState::Valid;
    out.ownDelayKnown=ownDelay.known();
    out.ownDelayUs=ownDelay.known() ? ownDelay.value() : 0;
    out.ownDelayAgeMs=ownDelayLastMs ? uint32_t(now-ownDelayLastMs) : 0;
    out.ownDelayCalibrations=static_cast<uint8_t>(ownDelay.count);
    out.calibrationsDiscarded=calibrationsDiscarded;
    out.calibrationPending=pendingCalibration.active;
    out.sourceChanges=sourceChanges;
    if(!wasCoordinator) {
        out.clusterDelayKnown=clusterDelayNsRx!=kNoDelayNs && clusterDelayRxMs &&
            uint32_t(now-clusterDelayRxMs)<=kClusterDelayRxMaxAgeMs;
        out.clusterDelayUs=out.clusterDelayKnown ? static_cast<double>(clusterDelayNsRx)/1000.0 : 0;
    }
    out.seriesAccepted=successfulSeries;
    out.seriesRejected=failedSeries;
    out.seriesLastFailureReason=lastSeriesFailure;
    out.slotRefreshMs=kSlotRefreshAgeMs;
    out.slotLifetimeMs=kSlotLifetimeMs;
    if(wasCoordinator){
        for(const auto &p:slotPeers) {
            if(!slotPeerActive(p,now)) continue;
            ++out.slotActivePeers;
            if(ClusterSlotScheduler::due(p.sched,now,kSlotParams)) ++out.slotDuePeers;
            if(!p.sched.ageKnown) { ++out.slotExpiredPeers; continue; }
            const uint32_t age=ClusterSlotScheduler::estimatedAgeMs(p.sched,now);
            if(age>=kSlotLifetimeMs) ++out.slotExpiredPeers;
            if(age>out.slotOldestAgeMs) out.slotOldestAgeMs=age;
        }
        const uint32_t periodMeanMs=slotPeriodCount ? static_cast<uint32_t>(slotPeriodTotalMs/slotPeriodCount) : 0;
        out.slotCycleEstimateMs=out.slotActivePeers*periodMeanMs;
        out.slotCapacity=slotPeers.size();
        out.slotTableFullRecent=slotTableFullLastMs && uint32_t(now-slotTableFullLastMs)<=kPeerActiveMs;
        out.slotRotations=slotRotations;
        out.slotSucceeded=slotStats.succeeded;
        out.slotFailed=slotStats.failed;
    }
    return out;}
bool benchmarkStart(uint32_t intervalMs,uint32_t targetBoot){
    if(!initialized||!wasCoordinator||capturePaused()||bench.running||
       (intervalMs!=5 && intervalMs!=10 && intervalMs!=50))return false;
    bench={};bench.intervalMs=intervalMs;
    // Keep the packed signed 64-bit control payload positive.
    bench.runId=static_cast<uint32_t>(esp_timer_get_time()) & 0x7fffffffU;
    if(!bench.runId)bench.runId=1;
    const uint32_t now=millis();
    for(const auto &p:slotPeers){
        if(bench.peerCount>=3)break;
        if(!p.boot || uint32_t(now-p.seenMs)>kPeerActiveMs ||
           (targetBoot && p.boot!=targetBoot))continue;
        const uint8_t index=bench.peerCount++;
        bench.peers[index].boot=p.boot;
        memcpy(benchMac[index],p.mac,6);
    }
    if(!bench.peerCount)return false;
    bench.running=true;bench.startedMs=now;bench.phase=0;
    if(slotOwner>=0)revokeSlotWithoutOutcome(lastEpoch,lastTag,localBootNonceForControl);
    // Let all outstanding 3-s sync permission leases expire before measuring.
    benchQuietUntilMs=now+kGrantLeaseMs+500UL;
    return true;
}
void benchmarkAbort(){benchAbortInternal(true);}
BenchmarkState benchmarkSnapshot(){return bench;}
size_t benchmarkExchangeCount(){return benchRawCount;}
bool benchmarkExchangeAt(size_t index, BenchmarkExchange &out){
    if(index>=benchRawCount)return false;
    const auto &e=benchRaw[index];
    out={e.run,e.seq,e.x.nodeTxUs,e.x.coordinatorRxUs,e.x.coordinatorTxUs,e.x.nodeRxUs};
    return true;
}
uint32_t benchmarkNodeRun(){return benchNodeRun;}
void benchmarkLocalTransport(uint32_t &runId,uint32_t &downReceived,uint32_t &upAcknowledged){
    runId=benchNodeRun;downReceived=0;upAcknowledged=0;
    for(size_t i=0;i<100;++i){downReceived+=benchNodeDownSeen[i]!=0;upAcknowledged+=benchNodeUpAckSeen[i]!=0;}
}
bool benchmarkLocalPacket(size_t seq,bool &downReceived,bool &upAcked,uint32_t &upAckRttUs){
    if(seq>=100)return false;
    downReceived=benchNodeDownSeen[seq]!=0;upAcked=benchNodeUpAckSeen[seq]!=0;
    upAckRttUs=benchNodeAckRttUs[seq];return true;
}

namespace {
// Coordinator side of one received, decoded frame.
void coordinatorReceive(const Queued &item,const EspNowTimePackets::Frame &f,
                        uint32_t epoch,uint64_t tag,uint32_t localBootNonce) {
    if((f.type==EspNowTimePackets::Type::BenchUp || f.type==EspNowTimePackets::Type::BenchAck) &&
       acceptPeer(item.mac,f)) {
        ++stats.authenticated;
        if(bench.running && bench.peerIndex<bench.peerCount &&
           f.senderBoot==bench.peers[bench.peerIndex].boot &&
           memcmp(item.mac,benchMac[bench.peerIndex],6)==0) {
            if(f.type==EspNowTimePackets::Type::BenchAck && f.nonce==UINT32_MAX &&
               f.timeUs==benchControlValue(bench.phase)) {
                if(!benchCtrlAck){benchCtrlAck=true;benchPhaseStart=millis();}
                return;
            }
            if(f.nonce>=100 || !benchCtrlAck)return;
            BenchmarkPeer &b=bench.peers[bench.peerIndex];
            if(f.type==EspNowTimePackets::Type::BenchAck && bench.phase==1 && f.timeUs==bench.runId && !b.downSeen[f.nonce]) {
                b.downSeen[f.nonce]=1;++b.downReceived;
                const uint32_t elapsed=static_cast<uint32_t>(esp_timer_get_time())-benchTxUs[f.nonce];
                b.downRttUs[f.nonce]=elapsed;
                b.downRttTotalUs+=elapsed;
                if(!b.downRttMinUs||elapsed<b.downRttMinUs)b.downRttMinUs=elapsed;
                if(elapsed>b.downRttMaxUs)b.downRttMaxUs=elapsed;
            } else if(f.type==EspNowTimePackets::Type::BenchUp && bench.phase==2) {
                if(!b.upSeen[f.nonce]){b.upSeen[f.nonce]=1;++b.upReceived;}
                benchSend(EspNowTimePackets::Type::BenchAck,f.nonce,bench.runId,kBroadcastMac);
            }
        }
        return;
    }
    if((f.type!=EspNowTimePackets::Type::Probe &&
        f.type!=EspNowTimePackets::Type::NodeSyncReport &&
        f.type!=EspNowTimePackets::Type::BeaconSyncReport)||!acceptPeer(item.mac,f)){
        ++stats.rejected;return;
    }
    ++stats.authenticated;
    if(f.type==EspNowTimePackets::Type::BeaconSyncReport){acceptBeaconSyncReport(f);return;}
    if(f.type==EspNowTimePackets::Type::NodeSyncReport){
        noteSlotPeer(item.mac,static_cast<uint32_t>(f.senderBoot));
        acceptNodeReport(f);return;
    }
    // Probe: only for this coordinator, only from the slot owner or the benchmark target.
    if(f.coordinatorTag != tag) { ++stats.rejected; return; }
    if(!slotIsOwner(item.mac,static_cast<uint32_t>(f.senderBoot)) &&
       !(bench.running && bench.phase==3 && bench.peerIndex<bench.peerCount &&
         f.senderBoot==bench.peers[bench.peerIndex].boot &&
         memcmp(item.mac,benchMac[bench.peerIndex],6)==0)) return;
    // Four-timestamp exchange: t1=Node Probe TX, t2=Coordinator Probe RX,
    // t3=Coordinator REPLY TX call, t4=Node REPLY RX callback.
    // Final delivers t3 metadata only; Final RX is NEVER t4.
    if(!ensurePeer(item.mac)){++stats.rejected;return;}
    EspNowTimePackets::Frame r;
    r.type=EspNowTimePackets::Type::Reply;r.epoch=epoch;r.coordinatorTag=tag;
    r.senderBoot=localBootNonce;r.nonce=f.nonce;r.timeUs=item.rxUs;
    int64_t replyDispatchBeforeUs=0;
    // t3 denotes the Reply send path, NOT the later Final send call.
    if(!sendFrame(r,item.mac,&replyDispatchBeforeUs,true))return;
    r.type=EspNowTimePackets::Type::Final;r.timeUs=replyDispatchBeforeUs;
    if(sendFrame(r,item.mac))++stats.probesAnswered;
    if(slotIsOwner(item.mac,static_cast<uint32_t>(f.senderBoot))) slotNoteOwnerProbe();
}
// Node side of one received, decoded frame.
void nodeReceive(const Queued &item,const EspNowTimePackets::Frame &f,uint32_t nowMs) {
    using T=EspNowTimePackets::Type;
    if(f.type!=T::SyncControl && f.type!=T::BenchControl && f.type!=T::BenchDown &&
       f.type!=T::BenchAck && f.type!=T::Reply && f.type!=T::Final &&
       f.type!=T::Beacon && f.type!=T::FollowUp){++stats.rejected;return;}
    if(f.coordinatorTag!=lastTag){++stats.rejected;return;}
    if(!acceptPeer(item.mac,f)){++stats.replay;return;}
    ++stats.authenticated;
    if(f.type==T::Beacon || f.type==T::FollowUp) {
        if(f.senderBoot==localBootNonceForControl || !beaconSourceAccept(f,item.mac)){++stats.rejected;return;}
        if(f.type==T::Beacon) onBeacon(f,item.rxUs,nowMs);
        else onFollowUp(f,nowMs);
        return;
    }
    if(f.type==T::BenchControl) {
        // Target boot prevents other nodes from participating.
        if(f.nonce!=localBootNonceForControl)return;
        const uint32_t phase=static_cast<uint16_t>(f.timeUs);
        const uint32_t gap=(static_cast<uint32_t>(f.timeUs)>>16)&0xffffu;
        if(gap!=5 && gap!=10 && gap!=50)return;
        if(phase==0){benchClearNode();return;}
        const uint32_t incomingRun=static_cast<uint32_t>(static_cast<uint64_t>(f.timeUs)>>32);
        if((phase==benchNodePhase && incomingRun==benchNodeRun) ||
           (phase==1) ||
           (phase==2 && benchNodePhase==1 && incomingRun==benchNodeRun) ||
           (phase==3 && benchNodePhase==2 && incomingRun==benchNodeRun)) {
            // Acknowledge before data traffic; duplicates do not restart a phase.
            benchSend(T::BenchAck,UINT32_MAX,f.timeUs,kBroadcastMac);
            if(phase==benchNodePhase && incomingRun==benchNodeRun)return;
        } else return;
        if(phase==1){
            benchNodeRun=incomingRun;
            benchNodeGapMs=gap;
            benchNodeBoot=static_cast<uint32_t>(f.senderBoot);
            benchNodePhase=1;benchNodeDeadlineMs=millis()+benchNodeGapMs*100+4500;
            benchNodeDownReceived=0;benchNodeNextSeq=0;benchNodeUpAck=0;
            for(auto &v:benchNodeUpAckSeen)v=0;
            for(auto &v:benchNodeDownSeen)v=0;
            for(auto &v:benchNodeTxUs)v=0;
            for(auto &v:benchNodeAckRttUs)v=0;
            benchRawCount=0;benchTimeProbes=0;
            permissionUntilMs=0;pending={};seriesCount=0;seriesStartedMs=0;
        } else if(phase==3 && benchNodePhase==2 && benchNodeRun==incomingRun) {
            benchNodePhase=3;benchNodeDeadlineMs=millis()+benchNodeGapMs*100+4500;
            benchTimeProbes=0;benchTimeNextMs=millis();pending={};
        } else if(phase==2 && benchNodePhase==1 && benchNodeRun==incomingRun) {
            benchNodePhase=2;benchNodeDeadlineMs=millis()+benchNodeGapMs*100+4500;
            benchNodeNextMs=millis();benchNodeNextSeq=0;
        }
        return;
    }
    if(f.type==T::BenchDown){
        if(benchNodePhase==1 && f.timeUs==localBootNonceForControl && f.nonce<100){
            if(!benchNodeDownSeen[f.nonce]){benchNodeDownSeen[f.nonce]=1;++benchNodeDownReceived;}
            benchSend(T::BenchAck,f.nonce,benchNodeRun,kBroadcastMac);
        }
        return;
    }
    if(f.type==T::BenchAck){
        if(benchNodePhase==2 && f.nonce<100 && f.timeUs==benchNodeRun && !benchNodeUpAckSeen[f.nonce]){
            benchNodeUpAckSeen[f.nonce]=1;++benchNodeUpAck;
            benchNodeAckRttUs[f.nonce]=static_cast<uint32_t>(esp_timer_get_time())-benchNodeTxUs[f.nonce];
        }
        return;
    }
    if(f.type==T::SyncControl) {
        // timeUs is a bounded lease length. Stop is immediate on authenticated receipt.
        if(f.nonce!=localBootNonceForControl) return;
        if(f.timeUs==0) {
            permissionUntilMs=0;pending={};seriesCount=0;seriesStartedMs=0;
            nextSeriesMs=millis()+1000UL;
        } else if(f.timeUs>0 && f.timeUs<=static_cast<int64_t>(kGrantLeaseMs)) {
            permissionUntilMs=millis()+static_cast<uint32_t>(f.timeUs);
        } else { ++stats.rejected; }
        return;
    }
    // Reply/Final of the running exchange, only from the beacon coordinator.
    if(!pending.active || f.nonce!=pending.nonce) return;
    if(!replyFromCoordinator(f,item.mac)) { pending={}; ++stats.rejected; return; }
    if(f.type==T::Reply) {
        pending.t2=f.timeUs;
        pending.t4ReplyRx=item.rxUs; // t4: the Reply receive callback, not Final
    } else {
        // V2: software timestamp correction of t3 with the delay of the coordinator's Reply.
        finishSample(f.timeUs + static_cast<int64_t>(f.preSendDelayUs));
    }
}
// Node: two-way calibration series while the coordinator grants a slot.
void nodeSeries(uint32_t now,uint32_t epoch,uint64_t tag,uint32_t localBootNonce) {
    if(!syncPermissionValid()) {
        pending={};seriesCount=0;seriesStartedMs=0;nextSeriesMs=now+1000UL;
        return;
    }
    if(pending.active&&static_cast<uint32_t>(now-pending.startedMs)>=kExchangeTimeoutMs){
        pending={};++stats.exchangeTimeouts;
    }
    if(seriesCount==0&&static_cast<int32_t>(now-nextSeriesMs)<0)return;
    if(seriesCount>0 && uint32_t(now-seriesStartedMs)>kMaxSeriesDurationMs) {
        // Never extend a busy/failed measurement series indefinitely.
        seriesCount=0;seriesStartedMs=0;
        pending={};seriesFailed(4);
        nextSeriesMs=now+qualityRetryDelayMs();return;
    }
    if(seriesCount==0 && !pending.active && seriesStartedMs==0)
        seriesStartedMs=now;
    if(pending.active||static_cast<int32_t>(now-nextProbeMs)<0)return;
    nextProbeMs=now+kProbeGapMs;
    if(!nonceSeed)nonceSeed=static_cast<uint32_t>(esp_timer_get_time())^localBootNonce;
    nonceSeed=nonceSeed*1664525u+1013904223u;
    if(!nonceSeed)++nonceSeed;
    EspNowTimePackets::Frame p;
    p.type=EspNowTimePackets::Type::Probe;p.epoch=epoch;
    p.coordinatorTag=tag;p.senderBoot=localBootNonce;p.nonce=nonceSeed;
    int64_t probeStampUs=0;
    if(sendFrame(p,kBroadcastMac,&probeStampUs,false)) {
        pending.active=true;pending.nonce=p.nonce;
        // V2: software timestamp correction with THIS Probe's measured delay.
        pending.t1=p.timeUs + static_cast<int64_t>(lastProbePreSendUs);
        pending.startedMs=now;
        pending.expectedBoot=beaconSourceKnown ? beaconSourceBoot : 0;
        if(beaconSourceKnown) memcpy(pending.expectedMac,beaconSourceMac,6);
        else memset(pending.expectedMac,0,6);
        ++stats.probesSent;
    }
}
}

void service(bool allowed,bool coordinator,uint32_t epoch,const char* id,uint32_t localBootNonce) {
    if(!allowed||!epoch||!localBootNonce||!id||!*id||WiFi.status()!=WL_CONNECTED||
       WiFi.getMode()!=WIFI_MODE_STA){if(initialized)stop();return;}
    uint8_t channel=0;wifi_second_chan_t secondary=WIFI_SECOND_CHAN_NONE;
    if(esp_wifi_get_channel(&channel,&secondary)!=ESP_OK||!channel){if(initialized)stop();return;}
    uint64_t tag=idTag(id);
    if(initialized&&(boundChannel!=channel||lastEpoch!=epoch||lastTag!=tag||wasCoordinator!=coordinator))stop();
    if(!initialized && !start(channel))return;
    wasCoordinator=coordinator;lastEpoch=epoch;lastTag=tag;localBootNonceForControl=localBootNonce;
    if (capturePaused()) return; // Capture always wins.
    if (captureGuardUntilMs) {
        captureGuardUntilMs = 0;
        clearQueue(); // Discard frames received while processing was paused.
        nextBeaconMs = millis();
        nextSeriesMs = millis() + 1000UL;
    }
    int64_t loadMarkUs=esp_timer_get_time();
    Queued item;unsigned budget=0;
    while(budget++<8&&pop(item)) {
        ++stats.received;
        EspNowTimePackets::Frame f;
        if(!EspNowTimePackets::decode(item.bytes,sizeof(item.bytes),f,clusterEspNowTimeVerify,nullptr)){
            ++stats.rejected;
            continue;
        }
        if(coordinator) coordinatorReceive(item,f,epoch,tag,localBootNonce);
        else nodeReceive(item,f,millis());
    }
    LoopLoadDiag::lap(LoopLoadDiag::EspNowRx,loadMarkUs);
    const uint32_t now=millis();
    if(coordinator){
        // Beacons continue during a benchmark; only capture pauses them.
        serviceFollowUp(epoch,tag,localBootNonce,now);
        if(!followUp.active && static_cast<int32_t>(now-nextBeaconMs)>=0){
            nextBeaconMs=now+kBeaconEveryMs;
            sendBeacon(epoch,tag,localBootNonce,now);
        }
        if(bench.running){benchTick(now);return;}
        LoopLoadDiag::Scope loadScope(LoopLoadDiag::EspNowSlots);
        serviceSlots(now,epoch,tag,localBootNonce);
        return;
    }
    if(benchNodePhase){benchNodeTick(now);return;}
    // The node joins the calibration schedule once its beacon model is settled
    // (a delay needs the envelope offset); the beacon report goes out always.
    if(static_cast<int32_t>(now-nextBeaconReportMs)>=0){
        nextBeaconReportMs=now+kReportEveryMs;
        sendBeaconSyncReport(epoch,tag,localBootNonce);
    }
    if(beaconModel.model().valid &&
       beaconModel.model().samples>=ClusterBeaconSync::kMinCalibrationSamples &&
       (reportSoon || static_cast<int32_t>(now-nextReportMs)>=0)){
        reportSoon=false;
        nextReportMs=now+kReportEveryMs;
        sendNodeReports(epoch,tag,localBootNonce);
    }
    if(static_cast<int32_t>(now-nextSeriesStatsReportMs)>=0){
        nextSeriesStatsReportMs=now+kSeriesStatsEveryMs;
        sendSeriesStatistics(epoch,tag,localBootNonce);
    }
    nodeSeries(now,epoch,tag,localBootNonce);
    // Clock selection is performed by clusterTimeNowUs(), never by callback.
}

uint32_t timeSourceGeneration() {
    return sourceChanges;
}

}
#else
namespace ClusterEspNowRadio {
void service(bool,bool,uint32_t,const char*,uint32_t){}
void stop(){}
void pauseForCapture(uint32_t){}
bool capturePaused(){return false;}
bool syncPermissionValid(){return false;}
bool precisionUtcNow(uint32_t,const char*,int64_t&,uint32_t&,double&){return false;}
Diagnostics diagnostics(){return {};}
size_t beaconTraceCount(){return 0;}
bool beaconTraceAt(size_t,BeaconTraceEntry&){return false;}
uint64_t beaconTraceCoordinatorBoot(){return 0;}
bool benchmarkStart(uint32_t,uint32_t){return false;}
void benchmarkAbort(){}
BenchmarkState benchmarkSnapshot(){return {};}
size_t benchmarkExchangeCount(){return 0;}
bool benchmarkExchangeAt(size_t,BenchmarkExchange&){return false;}
uint32_t benchmarkNodeRun(){return 0;}
void benchmarkLocalTransport(uint32_t &runId,uint32_t &downReceived,uint32_t &upAcknowledged){runId=downReceived=upAcknowledged=0;}
bool benchmarkLocalPacket(size_t,bool&,bool&,uint32_t&){return false;}
size_t nodeSyncReports(NodeSyncReport*,size_t){return 0;}
uint32_t timeSourceGeneration(){return 0;}
}
#endif
