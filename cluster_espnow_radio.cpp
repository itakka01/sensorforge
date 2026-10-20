#include "cluster_espnow_radio.h"
#include "cluster_espnow_packets.h"
#include "cluster_espnow_twoway.h"
#include "cluster_espnow_drift.h"
#include "cluster_espnow_regression.h"
#include "cluster.h"
#include "cluster_time_diagnostic_beacon.h"
#include "cluster_time_shadow_handover.h"
#include "cluster_time_grant_protocol.h" // Beta131: production handover enabled
#include "cluster_time_source_policy.h"
static_assert(ClusterTimeGrantProtocol::kWireBytes == 96, "grant frame size");
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

#if defined(BOARD_HAS_ESP_NOW) && BOARD_HAS_ESP_NOW
// HMAC is derived in cluster.cpp with a diagnostic-only domain.
extern bool clusterTimeGrantSign(const uint8_t*, size_t, uint8_t[32]);
extern bool clusterTimeGrantVerify(const uint8_t*, size_t, const uint8_t[32]);
extern bool clusterEspNowDiagnosticBeaconSign(const uint8_t*, size_t, uint8_t[32]);
extern bool clusterEspNowDiagnosticBeaconVerify(const uint8_t*, size_t, const uint8_t[32]);
namespace ClusterEspNowRadio {
namespace {
// V2 Software Timestamp Correction: Track measured delays for next transmission
static uint32_t lastProbePreSendUs=0;   // Delay from last Probe (for next Probe)
static uint32_t lastReplyPreSendUs=0;   // Delay from current Reply (for Final)
constexpr uint32_t kBroadcastEveryMs=10000;
// No active broadcast traffic: retain only the last 12 authenticated beacons.
// About 480 bytes static; no heap allocation, no callback-side changes.
constexpr size_t kBeaconObservations=12;
BeaconObservation beaconObservations[kBeaconObservations];
size_t beaconObsCount=0, beaconObsWrite=0;
BeaconPredictionStats beaconStats{};
// ~3 KiB static. Collected in the existing service task, never RX callback.
// Distinct coordinator identities reset the trace; no RF traffic is added.
constexpr size_t kSharedEventCapacity=128;
BeaconSharedEvent sharedEvents[kSharedEventCapacity];
size_t sharedEventCount=0, sharedEventWrite=0;
uint64_t sharedEventBoot=0;
uint32_t sharedEventEpoch=0;
void appendSharedEvent(const BeaconObservation &o) {
    if (sharedEventCount &&
        (sharedEventBoot!=o.coordinatorBoot || sharedEventEpoch!=o.epoch)) {
        sharedEventCount=sharedEventWrite=0;
    }
    sharedEventBoot=o.coordinatorBoot;
    sharedEventEpoch=o.epoch;
    BeaconSharedEvent &entry=sharedEvents[sharedEventWrite];
    entry.sequence=o.sequence;
    entry.epoch=o.epoch;
    entry.coordinatorMonoLow=o.coordinatorMonoLow;
    entry.nodeRxCallbackUs=static_cast<uint64_t>(o.nodeRxCallbackUs);
    entry.coordinatorBootLow=static_cast<uint32_t>(o.coordinatorBoot);
    sharedEventWrite=(sharedEventWrite+1)%kSharedEventCapacity;
    if(sharedEventCount<kSharedEventCapacity)++sharedEventCount;
}

// Accumulate every authenticated beacon in O(1) time, without storing samples.
void accumulateBeaconStats(const BeaconObservation &o) {
    if (beaconStats.received &&
        (beaconStats.coordinatorBoot!=o.coordinatorBoot || beaconStats.epoch!=o.epoch)) {
        beaconStats={}; // Never mix different coordinator clock epochs.
    }
    if (!beaconStats.received) {
        beaconStats.coordinatorBoot=o.coordinatorBoot;
        beaconStats.epoch=o.epoch;
        beaconStats.firstSequence=o.sequence;
    }
    ++beaconStats.received;
    beaconStats.lastSequence=o.sequence;
    const uint8_t status=o.predictionStatus;
    if(status<9)++beaconStats.statusCounts[status];
    if(status!=0 || o.predictionSamples==0)return;
    const double err=static_cast<double>(o.predictionErrorUs);
    ++beaconStats.valid;
    if(beaconStats.valid==1){
        beaconStats.minErrorUs=o.predictionErrorUs;
        beaconStats.maxErrorUs=o.predictionErrorUs;
    }else{
        if(o.predictionErrorUs<beaconStats.minErrorUs)beaconStats.minErrorUs=o.predictionErrorUs;
        if(o.predictionErrorUs>beaconStats.maxErrorUs)beaconStats.maxErrorUs=o.predictionErrorUs;
    }
    const double delta=err-beaconStats.meanErrorUs;
    beaconStats.meanErrorUs+=delta/static_cast<double>(beaconStats.valid);
    beaconStats.errorM2+=delta*(err-beaconStats.meanErrorUs);
    beaconStats.meanAbsErrorUs+=(fabs(err)-beaconStats.meanAbsErrorUs)/static_cast<double>(beaconStats.valid);
    const double drift=static_cast<double>(o.driftPpmX100)/100.0;
    beaconStats.meanDriftPpm+=(drift-beaconStats.meanDriftPpm)/static_cast<double>(beaconStats.valid);
}


constexpr uint32_t kProbeEveryMs=60000;
// Failed quality checks must not suspend retries longer than the precision
// clock lifetime (120 s). Jitter desynchronizes competing nodes.
constexpr uint32_t kRetryBaseMs=12000;
constexpr uint32_t kRetryJitterMs=12000;
// Single-owner precision measurement slots.  Short leases and a guard interval
// avoid overlapping permissions even if an authenticated revoke is lost.
constexpr uint32_t kGrantEveryMs=1000;
constexpr uint32_t kGrantLeaseMs=3000;
constexpr uint32_t kOwnerSlotMs=14000;
constexpr uint32_t kSlotGuardMs=3500;
constexpr uint32_t kPeerActiveMs=30000;
constexpr uint32_t kProbeGapMs=45; // Shorter series; quality gate unchanged
constexpr uint32_t kExchangeTimeoutMs=1200;
constexpr uint8_t kInitialSamples=16;
constexpr uint8_t kExpandedSamples=32;
constexpr uint8_t kMaxSamplesPerSeries=64;
constexpr uint32_t kMaxSeriesDurationMs=12000;
constexpr uint8_t kBroadcastMac[6]={255,255,255,255,255,255};
constexpr size_t kQueueSize=12;
struct Queued { uint8_t bytes[EspNowTimePackets::kPacketSize]; uint8_t mac[6]; int64_t rxUs; };
Queued queue[kQueueSize]={};
portMUX_TYPE queueMux=portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t head=0,tail=0;
volatile uint32_t drops=0;
Diagnostics stats;
// Separate bounded SFDB diagnostic RX path; never enters the production clock queue.
constexpr uint8_t kDiagnosticQueueSize=4;
struct DiagnosticQueued {
    uint8_t bytes[ClusterTimeDiagnosticBeacon::kWireBytes];
    uint8_t mac[6];
    int64_t rxUs;
};
DiagnosticQueued diagnosticQueue[kDiagnosticQueueSize]={};
uint8_t diagnosticHead=0,diagnosticTail=0;
DiagnosticBeaconRxStats diagnosticRx{};
uint64_t diagnosticLastTag=0,diagnosticLastBoot=0;
uint32_t diagnosticLastEpoch=0,diagnosticLastSequence=0;
uint32_t diagnosticLastReceivedMs=0,diagnosticConsecutive=0;
ClusterTimeShadowHandover::Monitor shadowHandover;
// Beta127: isolated diagnostic SF-TG transport. MAC+HMAC is NOT a trusted
// role/identity enrollment. No variable below feeds productive permission.
constexpr uint8_t kGrantQueueSize=4;
struct GrantQueued { uint8_t bytes[ClusterTimeGrantProtocol::kWireBytes]; uint8_t mac[6]; };
GrantQueued grantQueue[kGrantQueueSize]{};
uint8_t grantHead=0,grantTail=0;
GrantDiagnosticStats grantDiag{};
uint8_t observedBroadcasterMac[6]={};
uint64_t observedBroadcasterTag=0,observedBroadcasterBoot=0;
uint32_t observedBroadcasterAt=0;
uint32_t grantNextMs=0,grantTransaction=0,grantSequence=0;
uint32_t grantPendingTransaction=0,grantPendingSequence=0,grantPendingAt=0;
uint8_t grantPendingMac[6]={};
uint64_t grantPendingTag=0,grantPendingBoot=0;
uint64_t grantLastCoordinatorTag=0,grantLastCoordinatorBoot=0;
uint32_t grantLastCoordinatorEpoch=0,grantLastCoordinatorSequence=0;
// Beta131-FIX (Sixth cycle, Problem 3): Boot history for Grant/Ack replay protection
// CRITICAL: Track coordinator boot history to allow legitimate reboots while rejecting replay attacks
struct GrantBootHistory { uint64_t boot=0; uint32_t lastSeenMs=0; };
GrantBootHistory grantCoordinatorBootHistory[3];  // Last 3 coordinator boot sessions
uint8_t grantActiveBootIndex=0;  // Currently active boot session index

// Beta131-CORRECTED: Broadcaster production lease state with secure transaction protocol.
// Broadcaster runs as NODE (synchronizes with coordinator) AND sends production beacons.
// Production beacons use cluster-synchronized time from latestQualityEstimate, NOT monotonic time.
bool broadcasterLeaseActive=false;
uint32_t broadcasterLeaseActivationMs=0;  // Transaction-confirmed activation time
uint32_t broadcasterLeaseExpiresMs=0,broadcasterLeaseGrantedAt=0;
uint32_t broadcasterProductionBeaconsSent=0;
// Beta131-DIAGNOSIS (Seventeenth cycle): Production beacon blocking diagnostics
uint32_t broadcasterTimeBaseInvalid=0,broadcasterPrecisionFailed=0;
uint32_t precisionGuardWasCoordinator=0,precisionGuardEpochMismatch=0;
uint32_t precisionGuardTagMismatch=0,precisionGuardNotValid=0,precisionGuardOther=0;
// Beta131-DIAGNOSIS (Nineteenth cycle): Separate "other" guards
uint32_t precisionGuardRefLifetime=0,precisionGuardEstimateAge=0;
uint32_t precisionGuardDrift=0,precisionGuardMonoReconstruct=0,precisionGuardUtcRange=0;
uint32_t broadcasterActivationTransaction=0;  // SECURE: confirmed transaction for activation
bool broadcasterTimeBaseValid=false;  // SECURE: only send production when time is synchronized

// Beta131-CORRECTED (Third cycle): Broadcaster own identity tracking (Problem 1 & 2)
// CRITICAL: Separate broadcaster's OWN identity from coordinator identity and time source identity
// - broadcasterOwnTag/Id: THIS device's identity (for diagnostic beacons, Grant verification)
// - coordinatorTag/Id: Time authority identity (for sync source validation)
// - activeTimeSourceTag: Currently active time source (coordinator OR broadcaster)
uint64_t broadcasterOwnTag=0;  // Broadcaster's own identity tag
String broadcasterOwnId;  // Broadcaster's own device ID
uint32_t nextDiagnosticMs=0;  // Diagnostic beacon interval
uint32_t diagnosticSequence=0;  // Diagnostic beacon sequence

// Beta131-CORRECTED: Coordinator tracking of granted broadcaster lease with transaction confirmation
bool coordinatorGrantedLeaseActive=false;
uint32_t coordinatorGrantedLeaseExpiresMs=0;
uint32_t coordinatorSuppressionStartMs=0;  // Beta131-FIX (Tenth cycle): Delayed suppression for bootstrap
uint32_t coordinatorBeaconsSuppressed=0;
uint32_t coordinatorLeaseTransaction=0;  // SECURE: current lease transaction
uint64_t coordinatorLeaseBroadcasterTag=0;  // SECURE: authorized broadcaster identity
uint64_t coordinatorLeaseBroadcasterBoot=0;  // SECURE: authorized broadcaster boot
// Beta131-FIX (Eighth cycle, Problem 1): Startup guard prevents double-sender after coordinator reboot
// CRITICAL: After reboot, coordinator has no knowledge of previously granted leases
// Guard suppresses production beacons until any old leases have safely expired
uint32_t coordinatorStartupGuardUntilMs=0;  // millis() timestamp when startup guard expires
// Beta132-V2-FIX: Track broadcaster production beacon reception for condition-based suppression
// CRITICAL: Coordinator only suppresses when broadcaster CONFIRMED active (received production beacon)
uint32_t lastProductionBeaconFromLeasedBroadcasterMs=0;
bool coordinatorLastSlotSuppressed=false;  // Beta136: last beacon slot was suppressed (early resume)
uint32_t coordinatorProductionBeaconsSent=0, coordinatorEarlyResumes=0;  // Beta136: diagnostics  // Last time coordinator received broadcaster production beacon

// Beta131-CORRECTED: Node tracking of active time source with role validation
uint64_t activeTimeSourceTag=0;  // 0=none, or tag of coordinator/broadcaster
uint64_t activeTimeSourceBoot=0;  // Boot ID of active source
uint32_t activeTimeSourceEpoch=0;  // Epoch of active source
// Beta131-FIX (Seventh cycle, Problem 2): Track active time source MAC for Reply/Final validation
uint8_t activeTimeSourceMac[6]={};  // MAC address of active time source from validated beacon
uint32_t sourceChanges=0;

// Callback-safe TX latency sampling. Entries are inserted before esp_now_send().
// Match by destination MAC and FIFO; no per-packet sequence is supplied to TX callbacks.
constexpr size_t kTxPendingCapacity=24;
// Beta138: EVERY esp_now_send() registers an entry (also grants/acks and diagnostic/production
// beacons). Before, only sendFrame() did, so those frames counted as "unmatched" and their
// callbacks could consume another frame's entry with the same MAC (e.g. a broadcaster production
// beacon taking a pending broadcast Probe), which mis-attributed TX latency samples.
constexpr uint8_t kTxClassOther=0, kTxClassReply=1;
struct TxPending { bool used=false; uint8_t mac[6]={}; int64_t enqueueUs=0; uint8_t frameClass=kTxClassOther; };
TxPending txPending[kTxPendingCapacity];
portMUX_TYPE txMux=portMUX_INITIALIZER_UNLOCKED;
uint32_t txLatencyLastUs=0,txLatencyMaxUs=0,txLatencySamples=0;
uint32_t txCallbackFailed=0,txPendingOverflow=0,txCallbackUnmatched=0;
// Beta138 diagnostics: enqueue -> TX-done callback of Reply frames only (NOT the RF airtime).
uint32_t txReplyLatencyLastUs=0,txReplyLatencyMaxUs=0,txReplyLatencySamples=0;
uint32_t txReplyLatencyHist[Diagnostics::kProbeReplyDelayBuckets]={};
void txTimingReset() {
    portENTER_CRITICAL(&txMux);
    for(auto &entry:txPending)entry={};
    txLatencyLastUs=txLatencyMaxUs=txLatencySamples=0;
    txCallbackFailed=txPendingOverflow=txCallbackUnmatched=0;
    txReplyLatencyLastUs=txReplyLatencyMaxUs=txReplyLatencySamples=0;
    for(auto &bucket:txReplyLatencyHist)bucket=0;
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
    } else ++txPendingOverflow;
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
// Direct esp_now_send() with TX-latency registration (grants, acks, beacons).
esp_err_t espNowSendTracked(const uint8_t mac[6],const uint8_t *data,size_t length) {
    const int64_t enqueueUs=esp_timer_get_time();
    const size_t slot=txPendingRegister(mac,kTxClassOther,enqueueUs);
    const esp_err_t err=esp_now_send(mac,data,length);
    if(err!=ESP_OK)txPendingCancel(slot,enqueueUs);
    return err;
}
void onSentMac(const uint8_t* mac,esp_now_send_status_t status) {
    const int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&txMux);
    size_t match=kTxPendingCapacity;
    int64_t oldest=INT64_MAX;
    for(size_t i=0;i<kTxPendingCapacity;++i)
        if(txPending[i].used && (!mac || memcmp(txPending[i].mac,mac,6)==0) &&
           txPending[i].enqueueUs<oldest){match=i;oldest=txPending[i].enqueueUs;}
    if(match==kTxPendingCapacity)++txCallbackUnmatched;
    else {
        const int64_t elapsed=now-txPending[match].enqueueUs;
        txPending[match].used=false;
        if(elapsed>=0 && elapsed<=UINT32_MAX){
            txLatencyLastUs=static_cast<uint32_t>(elapsed);
            if(txLatencyLastUs>txLatencyMaxUs)txLatencyMaxUs=txLatencyLastUs;
            ++txLatencySamples;
            if(txPending[match].frameClass==kTxClassReply) {
                const uint32_t us=txLatencyLastUs;
                uint8_t bucket=0;
                while(bucket<Diagnostics::kProbeReplyDelayBuckets-1 && us>kProbeReplyDelayBucketUpperUs[bucket])++bucket;
                ++txReplyLatencyHist[bucket];
                ++txReplyLatencySamples;
                txReplyLatencyLastUs=us;
                if(us>txReplyLatencyMaxUs)txReplyLatencyMaxUs=us;
            }
        }
    }
    if(status!=ESP_NOW_SEND_SUCCESS)++txCallbackFailed;
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
uint32_t nextBroadcastMs=0,nextSeriesMs=0,nextProbeMs=0,txSequence=0,lastEpoch=0;
// Beta131-FIX: nextDiagnosticMs and diagnosticSequence already defined at lines 179-180
bool diagnosticMode=false;
// Manual short-rate experiment. No heap and no automatic startup.
uint32_t beaconExperimentUntilMs=0;
uint32_t beaconExperimentIntervalMs=0;
uint32_t beaconExperimentSent=0;
uint32_t beaconExperimentLastDurationMs=0;
bool beaconExperimentQuiet=false;
uint32_t captureGuardUntilMs=0;
uint32_t permissionUntilMs=0, nextGrantMs=0;
bool coordinatorPermissionIssued=false;
bool sendFrame(EspNowTimePackets::Frame& f,const uint8_t mac[6],
               int64_t* txStampOut=nullptr, bool replyStamp=false);
struct SlotPeer { uint8_t mac[6]={}; uint32_t boot=0; uint32_t seenMs=0; };
SlotPeer slotPeers[12];
uint8_t slotNextIndex=0;
int8_t slotOwner=-1;
uint32_t slotStartedMs=0, slotGuardUntilMs=0;
uint32_t slotGrantsSent=0, slotRotations=0;
void noteSlotPeer(const uint8_t mac[6], uint32_t boot) {
    if(!boot) return;
    const uint32_t now=millis();
    for(auto &p:slotPeers) {
        if(p.boot && memcmp(p.mac,mac,6)==0) {
            // A different boot on the same MAC is a different owner.
            if(p.boot!=boot && slotOwner==(&p-slotPeers)) {
                slotOwner=-1;slotGuardUntilMs=now+kSlotGuardMs;
            }
            p.boot=boot; p.seenMs=now; return;
        }
    }
    for(auto &p:slotPeers) if(!p.boot || uint32_t(now-p.seenMs)>kPeerActiveMs) {
        p={};memcpy(p.mac,mac,6);p.boot=boot;p.seenMs=now;return;
    }
}
bool slotIsOwner(const uint8_t mac[6], uint32_t boot) {
    return slotOwner>=0 && slotPeers[slotOwner].boot==boot &&
           memcmp(slotPeers[slotOwner].mac,mac,6)==0 &&
           uint32_t(millis()-slotStartedMs)<kOwnerSlotMs;
}
void sendSlotControl(uint32_t epoch,uint64_t tag,uint32_t boot,
                     uint32_t targetBoot,uint32_t leaseMs) {
    EspNowTimePackets::Frame f;
    f.type=EspNowTimePackets::Type::SyncControl;
    f.epoch=epoch;f.coordinatorTag=tag;f.senderBoot=boot;
    f.nonce=targetBoot;f.timeUs=leaseMs;
    if(sendFrame(f,kBroadcastMac) && leaseMs) ++slotGrantsSent;
}
void endSlot(uint32_t epoch,uint64_t tag,uint32_t boot) {
    if(slotOwner>=0) {
        sendSlotControl(epoch,tag,boot,slotPeers[slotOwner].boot,0);
        slotNextIndex=static_cast<uint8_t>((slotOwner+1)%12);
        ++slotRotations;
    }
    slotOwner=-1;
    slotGuardUntilMs=millis()+kSlotGuardMs;
}
void serviceSlots(uint32_t now,uint32_t epoch,uint64_t tag,uint32_t boot) {
    if(slotOwner>=0 && (uint32_t(now-slotStartedMs)>=kOwnerSlotMs ||
       uint32_t(now-slotPeers[slotOwner].seenMs)>kPeerActiveMs)) endSlot(epoch,tag,boot);
    if(slotOwner<0) {
        if(static_cast<int32_t>(now-slotGuardUntilMs)<0) return;
        for(unsigned offset=0;offset<12;++offset) {
            const uint8_t i=static_cast<uint8_t>((slotNextIndex+offset)%12);
            if(slotPeers[i].boot && uint32_t(now-slotPeers[i].seenMs)<=kPeerActiveMs) {
                slotOwner=i;slotStartedMs=now;nextGrantMs=now;break;
            }
        }
    }
    if(slotOwner>=0 && static_cast<int32_t>(now-nextGrantMs)>=0) {
        nextGrantMs=now+kGrantEveryMs;
        sendSlotControl(epoch,tag,boot,slotPeers[slotOwner].boot,kGrantLeaseMs);
    }
}
uint32_t localBootNonceForControl=0;
uint64_t lastTag=0;
// Shared exchange state is declared before benchmark service helpers.
// Beta131-FIX (Sixth cycle, Problem 1): Bind Probe/Reply/Final to sender identity
struct Pending {
    bool active=false;
    uint32_t nonce=0;
    int64_t t1=0,t2=0,t4ReplyRx=0;
    uint32_t startedMs=0;
    // CRITICAL: Expected sender identity for Reply/Final validation
    uint64_t expectedTag=0;     // Sender's coordinatorTag (broadcaster OR coordinator)
    uint64_t expectedBoot=0;    // Sender's boot nonce
    uint32_t expectedEpoch=0;   // Sender's epoch
    uint8_t expectedMac[6]={};  // Sender's MAC address
} pending;
uint32_t nonceSeed=0;
// Transport benchmark: 100 downlink packets and 100 uplink packets per peer.
// The result is *transport* delivery/ACK latency, not an absolute one-way time.
constexpr uint32_t kBenchGapMs=50;
constexpr uint32_t kBenchPhaseHoldMs=7500;
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
    slotOwner=-1;slotGuardUntilMs=now+kSlotGuardMs;
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
        // A real four-timestamp exchange, independent of the precision estimator.
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

EspNowTimePackets::ReplayWindow coordinatorReplay;
// Beta131-FIX (Fifth cycle, Problem 1): Track boot history for random nonce replay protection
struct BootHistory { uint64_t boot=0; uint32_t epoch=0; uint32_t lastSeenMs=0; };
struct PeerReplay {
    bool used=false;
    uint8_t mac[6]={};
    EspNowTimePackets::ReplayWindow replay;
    BootHistory bootHistory[3]; // Track last 3 (boot, epoch) sessions (both random, not sequential)
    uint8_t activeBootIndex=0; // Index of currently active boot in history
};
PeerReplay peerReplay[12];
ClusterEspNowTwoWay::Exchange exchanges[kMaxSamplesPerSeries];
uint8_t seriesCount=0;
uint8_t seriesTarget=kInitialSamples;
uint32_t seriesStartedMs=0;
ClusterEspNowRegression::Point driftHistory[ClusterEspNowRegression::kHistory];
size_t driftHistoryCount=0;
uint32_t nextReportMs=0;
bool seriesPrecisionAccepted=false;
NodeSyncReport remoteReports[12];
uint32_t remoteReceivedAt[12]={};
uint8_t remoteNext=0;
uint32_t reportCount=0;
uint32_t retrySerial=0;
uint32_t qualityRetryDelayMs() {
    // Different node boots and successive failures use different retry phases.
    // Only affects retry scheduling, not clock estimates or quality thresholds.
    const uint32_t x=localBootNonceForControl ^ (++retrySerial*0x9e3779b9UL);
    const uint32_t mixed=(x ^ (x>>16))*0x7feb352dUL;
    return kRetryBaseMs + ((mixed ^ (mixed>>15)) % kRetryJitterMs);
}
uint32_t failedSeries=0;
// Beta136 diagnostics: probe RX callback (t2) -> reply pre-send stamp (t3). Observation only.
void recordProbeReplyDelay(int64_t rxUs, int64_t replyStampUs) {
    if(replyStampUs<rxUs) return;
    const uint64_t delta=static_cast<uint64_t>(replyStampUs-rxUs);
    const uint32_t us=delta>UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(delta);
    uint8_t bucket=0;
    while(bucket<Diagnostics::kProbeReplyDelayBuckets-1 && us>kProbeReplyDelayBucketUpperUs[bucket]) ++bucket;
    ++stats.probeReplyDelayHist[bucket];
    ++stats.probeReplyDelaySamples;
    stats.probeReplyDelayLastUs=us;
    if(us>stats.probeReplyDelayMaxUs) stats.probeReplyDelayMaxUs=us;
    stats.probeReplyDelayTotalUs+=us;
}
uint32_t successfulSeries=0, lastCompletedSeriesMs=0, nextSeriesStatsReportMs=0;
uint32_t lastEstimateMs=0;
uint8_t lastCompletedSeriesResult=0;
uint8_t lastSeriesFailure=0; // 1=invalid/insufficient; 2=spread; 3=RTT; 4=series deadline
uint8_t lastRejectedReason=0;
uint32_t lastRejectedSeriesMs=0;
bool lastRejectedHasMetrics=false;
ClusterEspNowTwoWay::Estimate lastRejectedEstimate;
uint8_t consecutiveFailures=0;
uint32_t seriesRejectByReason[5]={};  // Beta136 diagnostics: index = lastSeriesFailure (1..4)
uint32_t nextReportDebugMs=0;

ClusterEspNowTwoWay::Estimate previousEstimate;
ClusterEspNowTwoWay::Estimate latestQualityEstimate;
int64_t referenceUtcUs=0;
uint32_t referenceMonoLow=0;
int64_t referenceReceivedNodeMono=0;
uint32_t lastPrecisionMs=0;
constexpr uint32_t kPrecisionLifetimeMs=120000;
constexpr int64_t kReferenceLifetimeUs=30000000;

String referenceId;
uint32_t goodDriftPairs=0;
// Persisted drift is a prior only: it must not mark the active clock synchronized.
void loadDriftPrior(const char *id) {
    referenceId = id ? String(id) : String();
    previousEstimate = {};
    latestQualityEstimate = {};
    lastPrecisionMs = 0;
    goodDriftPairs = 0;
    driftHistoryCount = 0;
    seriesTarget=kInitialSamples;
    stats.driftValid = false;
    stats.storedDriftAvailable = false;
    stats.driftPpm = 0;
    stats.driftUncertaintyPpm = 0;
    stats.driftSamples = 0;
    ClusterEspNowDrift prior;
    if (clusterEspNowDriftLoad(referenceId, prior)) {
        stats.storedDriftAvailable = true;
        stats.driftPpm = prior.ppm;
        stats.driftUncertaintyPpm = prior.uncertaintyPpm;
        stats.driftSamples = prior.samples;
    }
}
void acceptEstimate(const ClusterEspNowTwoWay::Estimate &e) {
    lastEstimateMs=millis();
    stats.lastExchangeValid = e.valid;
    // Diagnostic snapshot only; no effect on estimate acceptance, retries or slots.
    stats.lastSeriesEstimateAvailable = e.valid && e.selected > 0;
    stats.lastSeriesSelected=e.selected;
    stats.lastSeriesSpreadUs=e.spreadUs;
    stats.lastSeriesRttMinUs=e.selectedRttMinUs;
    stats.lastSeriesRttMaxUs=e.selectedRttMaxUs;
    stats.lastSeriesOffsetP10Us=e.selectedOffsetP10Us;
    stats.lastSeriesOffsetP90Us=e.selectedOffsetP90Us;
    seriesPrecisionAccepted=false;
    lastCompletedSeriesMs=millis();
    lastCompletedSeriesResult=2;
    if (!e.valid) { ++failedSeries; lastSeriesFailure=1; ++seriesRejectByReason[1]; lastRejectedReason=1; lastRejectedSeriesMs=millis(); lastRejectedHasMetrics=false; lastRejectedEstimate=e; consecutiveFailures=static_cast<uint8_t>(std::min(8U,unsigned(consecutiveFailures)+1U)); return; }
    stats.lastOffsetUs = e.offsetUs;
    stats.lastRttUs = e.medianRttUs;
    // A wide offset range indicates RF/software timing jitter, not calibrated drift.
    if (!std::isfinite(e.spreadUs) || e.spreadUs > 250.0 || e.accepted < 8 ||
        !std::isfinite(e.medianRttUs) || e.medianRttUs > 30000.0) {
        ++failedSeries;
        lastSeriesFailure=(e.accepted<8?1:(e.spreadUs>250.0?2:3));
        ++seriesRejectByReason[lastSeriesFailure];
        lastRejectedEstimate=e;
        lastRejectedReason=lastSeriesFailure; lastRejectedSeriesMs=millis(); lastRejectedHasMetrics=true;
        consecutiveFailures=static_cast<uint8_t>(std::min(8U,unsigned(consecutiveFailures)+1U));
        return;
    }
    lastSeriesFailure=0;
    ++successfulSeries;
    lastCompletedSeriesResult=1;
    consecutiveFailures=0;
    latestQualityEstimate=e;
    seriesPrecisionAccepted=true;
    lastPrecisionMs=millis();

    // Beta131-FIX (Twentieth cycle): Forward-propagate beacon reference from Probe/Reply/Final
    // CRITICAL: Broadcaster cannot receive beacons during lease (coordinator suppresses them).
    // Solution: Update beacon reference based on successful precision series to keep it fresh.
    // MATHEMATICS: Use actual offset difference (offsetNew - offsetOld), NOT drift approximation.
    // This ensures exact UTC tracking even with offset jumps (RF jitter, clock steps).
    // Condition: broadcaster context (lease exists) AND valid beacon reference AND moving forward
    if (broadcasterLeaseExpiresMs && referenceUtcUs && referenceReceivedNodeMono) {
        const int64_t elapsedNode = e.nodeReferenceUs - referenceReceivedNodeMono;
        // Only update if moving forward and within reasonable bounds (prevent wraparound issues)
        if (elapsedNode > 0 && elapsedNode < static_cast<int64_t>(kPrecisionLifetimeMs) * 1000LL) {
            // Calculate new coordinator monotonic from current precision estimate
            // offsetUs = coordinatorMono - nodeMono (from cluster_espnow_twoway.h:22)
            const int64_t newCoordMono = e.nodeReferenceUs + static_cast<int64_t>(llround(e.offsetUs));

            // Reconstruct old coordinator monotonic from 32-bit referenceMonoLow
            // Use same wraparound logic as precisionUtcNow() for consistency
            const int32_t delta = static_cast<int32_t>(referenceMonoLow - static_cast<uint32_t>(newCoordMono));
            const int64_t oldCoordMono = newCoordMono + delta;

            // Calculate elapsed coordinator time using ACTUAL offset difference
            // elapsedCoord = (nodeNew + offsetNew) - (nodeOld + offsetOld)
            //              = elapsedNode + (offsetNew - offsetOld)
            // This correctly handles offset jumps without drift approximation error
            const int64_t elapsedCoord = newCoordMono - oldCoordMono;

            // Update UTC reference by adding exact elapsed coordinator time
            referenceUtcUs += elapsedCoord;

            // Update coordinator monotonic reference (32-bit low)
            referenceMonoLow = static_cast<uint32_t>(newCoordMono);

            // Update node reception timestamp
            referenceReceivedNodeMono = e.nodeReferenceUs;
        }
    }

    // Regression across multiple *series*, not two potentially noisy cycles.
    if (driftHistoryCount && e.nodeReferenceUs<=driftHistory[driftHistoryCount-1].nodeTimeUs)
        driftHistoryCount=0;
    if (driftHistoryCount==ClusterEspNowRegression::kHistory) {
        for(size_t i=1;i<driftHistoryCount;++i) driftHistory[i-1]=driftHistory[i];
        --driftHistoryCount;
    }
    driftHistory[driftHistoryCount++]={e.nodeReferenceUs,e.offsetUs,e.spreadUs};
    const auto fit=ClusterEspNowRegression::fit(driftHistory,driftHistoryCount);
    if(fit.valid && (!stats.driftValid || fabs(fit.ppm-stats.driftPpm)<=100.0)) {
        stats.driftPpm=fit.ppm;
        stats.driftUncertaintyPpm=std::min(100.0,
            fit.residualUs*1000000.0/(double)fit.spanUs);
        stats.driftValid=true;
        ++goodDriftPairs;
        stats.driftSamples=static_cast<uint32_t>(fit.count);
        const time_t utc=time(nullptr);
        if (goodDriftPairs>=2 && utc>1600000000 &&
            stats.driftUncertaintyPpm<=100.0 && !referenceId.isEmpty()) {
            ClusterEspNowDrift value;
            value.coordinatorId=referenceId;
            value.ppm=stats.driftPpm;
            value.uncertaintyPpm=stats.driftUncertaintyPpm;
            value.samples=stats.driftSamples;
            value.calibratedUtcUs=static_cast<int64_t>(utc)*1000000LL;
            if(clusterEspNowDriftStore(value)) ++stats.driftPersisted;
        }
    }
    previousEstimate = e;
}

uint64_t idTag(const char* id) {
    uint64_t x=14695981039346656037ULL;
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(id);p && *p;++p)
        x=(x^*p)*1099511628211ULL;
    return x ? x : 1;
}
void onReceive(const esp_now_recv_info_t* info,const uint8_t* data,int length) {
    if(!info||!info->src_addr||!data)return;
    // Beta133-FIX: Grant frames (96 bytes) - unique size, no conflict
    if(length==static_cast<int>(ClusterTimeGrantProtocol::kWireBytes)) {
        portENTER_CRITICAL(&queueMux);
        const uint8_t next=static_cast<uint8_t>((grantHead+1)%kGrantQueueSize);
        if(next==grantTail) ++grantDiag.dropped;
        else {
            memcpy(grantQueue[grantHead].bytes,data,ClusterTimeGrantProtocol::kWireBytes);
            memcpy(grantQueue[grantHead].mac,info->src_addr,6);
            grantHead=next;
        }
        portEXIT_CRITICAL(&queueMux);
        return;
    }
    // Beta133-FIX: 80-byte frames - distinguish Diagnostic ('SFDB') vs Time ('SFEN') by magic
    // CRITICAL: Both ClusterTimeDiagnosticBeacon and EspNowTimePackets are 80 bytes
    // Wire format byte 2: Diagnostic='D' (0x44), Time='E' (0x45)
    // Magic is HMAC-protected, preventing type spoofing attacks
    if(length==static_cast<int>(ClusterTimeDiagnosticBeacon::kWireBytes)) {
        // Diagnostic Beacon: magic 'SFDB' (S=0x53, F=0x46, D=0x44, B=0x42)
        if(length>=3 && data[0]=='S' && data[1]=='F' && data[2]=='D') {
            const int64_t rx=esp_timer_get_time();
            portENTER_CRITICAL(&queueMux);
            const uint8_t next=static_cast<uint8_t>((diagnosticHead+1)%kDiagnosticQueueSize);
            if(next==diagnosticTail) ++diagnosticRx.dropped;
            else {
                memcpy(diagnosticQueue[diagnosticHead].bytes,data,ClusterTimeDiagnosticBeacon::kWireBytes);
                memcpy(diagnosticQueue[diagnosticHead].mac,info->src_addr,6);
                diagnosticQueue[diagnosticHead].rxUs=rx;
                diagnosticHead=next;
            }
            portEXIT_CRITICAL(&queueMux);
            return;
        }
        // Time Packet: magic 'SFEN' (S=0x53, F=0x46, E=0x45, N=0x4E)
        // Fall through to time packet queue below if magic matches
        if(length<3 || data[0]!='S' || data[1]!='F' || data[2]!='E') {
            // Invalid magic for 80-byte packet
            return;
        }
        // Valid time packet magic - continue to time queue
    } else {
        // Wrong size (not 80, not 96)
        static uint32_t wrongSizePackets = 0;
        ++wrongSizePackets;
        if(wrongSizePackets % 10 == 1) {
            Serial.printf("[RX-REJECT] Wrong size: got %d bytes, expected %zu or %zu. Count: %u\n",
                length, ClusterTimeDiagnosticBeacon::kWireBytes, ClusterTimeGrantProtocol::kWireBytes, wrongSizePackets);
        }
        return;
    }
    // Time packet queue (80-byte 'SFEN' packets only reach here)
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
bool popDiagnostic(DiagnosticQueued &v) {
    bool ok=false;
    portENTER_CRITICAL(&queueMux);
    if(diagnosticTail!=diagnosticHead){
        v=diagnosticQueue[diagnosticTail];
        diagnosticTail=static_cast<uint8_t>((diagnosticTail+1)%kDiagnosticQueueSize);
        ok=true;
    }
    portEXIT_CRITICAL(&queueMux);
    return ok;
}
bool pop(Queued &v) {
    bool ok=false;
    portENTER_CRITICAL(&queueMux);
    if(tail!=head){v=queue[tail];tail=static_cast<uint8_t>((tail+1)%kQueueSize);ok=true;}
    portEXIT_CRITICAL(&queueMux);
    return ok;
}
void clearQueue(){portENTER_CRITICAL(&queueMux);head=tail=0;diagnosticHead=diagnosticTail=0;grantHead=grantTail=0;drops=0;portEXIT_CRITICAL(&queueMux);}
bool start(uint8_t channel) {
    if(esp_now_init()!=ESP_OK)return false;
    if(esp_now_register_recv_cb(onReceive)!=ESP_OK){esp_now_deinit();return false;}
    txTimingReset();
    if(esp_now_register_send_cb(onSent)!=ESP_OK){esp_now_deinit();return false;}
    esp_now_peer_info_t peer={};
    memcpy(peer.peer_addr,kBroadcastMac,6);peer.channel=0;peer.ifidx=WIFI_IF_STA;
    if(esp_now_add_peer(&peer)!=ESP_OK){esp_now_deinit();return false;}
    stats.rxQueueDelayLastUs=stats.rxQueueDelayMaxUs=0;
    stats.replyFinalDispatchGapLastUs=stats.replyFinalDispatchGapMaxUs=0;
    stats.probeTxPreSendLastUs=stats.probeTxPreSendMaxUs=stats.probeTxPreSendSamples=0;
    stats.replyTxPreSendLastUs=stats.replyTxPreSendMaxUs=stats.replyTxPreSendSamples=0;
    stats.probeTxPreSendTotalUs=stats.replyTxPreSendTotalUs=0;
    initialized=true;boundChannel=channel;stats.active=true;stats.channel=channel;
    nextBroadcastMs=millis()+1000;nextSeriesMs=millis()+2000;nextGrantMs=millis();
    permissionUntilMs=0;coordinatorPermissionIssued=false;nextReportMs=millis()+1500UL;
    for(auto &p:slotPeers)p={};slotNextIndex=0;slotOwner=-1;
    slotStartedMs=0;slotGuardUntilMs=millis()+kSlotGuardMs;
    slotGrantsSent=slotRotations=0;
    return true;
}
uint32_t nextSequence(){++txSequence;if(!txSequence)++txSequence;return txSequence;}
bool sendFrame(EspNowTimePackets::Frame& f,const uint8_t mac[6],
               int64_t* txStampOut, bool replyStamp) {
    f.sequence=nextSequence();
    // Prepare callback bookkeeping BEFORE sampling the transmission timestamp.
    // Otherwise queue bookkeeping unnecessarily biases t1/t3.
    const int64_t queuedAtUs=esp_timer_get_time();
    const size_t slot=txPendingRegister(mac,
        f.type==EspNowTimePackets::Type::Reply ? kTxClassReply : kTxClassOther,queuedAtUs);
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

    // V2: Set preSendDelayUs ONLY for Final (carries Reply's measured delay)
    // Probe cannot carry its own delay (not yet measured), so preSendDelayUs=0
    if(f.type==EspNowTimePackets::Type::Final) {
        f.preSendDelayUs=lastReplyPreSendUs;  // Measured delay from Reply (already sent)
    } else {
        f.preSendDelayUs=0;  // Probe/Reply/others: no delay in packet
    }

    uint8_t b[EspNowTimePackets::kPacketSize];
    if(!EspNowTimePackets::encode(f,b,clusterEspNowTimeSign,nullptr)) {
        txPendingCancel(slot,queuedAtUs);
        return false;
    }
    // V2: Send packet FIRST, then measure delay (timestamp to esp_now_send completion)
    if(esp_now_send(mac,b,sizeof(b))!=ESP_OK) {
        txPendingCancel(slot,queuedAtUs);
        return false;
    }
    // V2: Measure delay AFTER esp_now_send() completes
    if(txStampOut) {
        const int64_t preSendUs=esp_timer_get_time()-stampUs;
        if(preSendUs>=0 && preSendUs<=UINT32_MAX) {
            const uint32_t delta=static_cast<uint32_t>(preSendUs);
            // V2: Store measured delay for local correction (Probe) or next packet (Reply→Final)
            if(f.type==EspNowTimePackets::Type::Probe) {
                lastProbePreSendUs=delta;  // Current Probe's delay (for local correction)
            } else if(f.type==EspNowTimePackets::Type::Reply) {
                lastReplyPreSendUs=delta;  // Current Reply's delay (for Final packet)
            }
            // Update statistics
            uint32_t &last=replyStamp ? stats.replyTxPreSendLastUs : stats.probeTxPreSendLastUs;
            uint32_t &max=replyStamp ? stats.replyTxPreSendMaxUs : stats.probeTxPreSendMaxUs;
            uint32_t &count=replyStamp ? stats.replyTxPreSendSamples : stats.probeTxPreSendSamples;
            uint64_t &total=replyStamp ? stats.replyTxPreSendTotalUs : stats.probeTxPreSendTotalUs;
            last=delta;
            if(delta>max)max=delta;
            ++count;
            total+=delta;
        }
    }
    ++stats.sent;return true;
}
bool ensurePeer(const uint8_t mac[6]) {
    if(esp_now_is_peer_exist(mac))return true;
    esp_now_peer_info_t p={};memcpy(p.peer_addr,mac,6);p.channel=0;p.ifidx=WIFI_IF_STA;
    return esp_now_add_peer(&p)==ESP_OK;
}
// Reports share the HMAC/replay protection and Coordinator epoch of Probes.
// timeUs is two packed 32-bit values, NOT a timestamp for report frames.
void acceptNodeReport(const EspNowTimePackets::Frame &f) {
    const uint32_t boot=static_cast<uint32_t>(f.senderBoot);
    if(!boot)return;
    size_t slot=12;
    for(size_t i=0;i<12;++i)if(remoteReports[i].bootNonce==boot){slot=i;break;}
    if(slot==12){for(size_t i=0;i<12;++i)if(!remoteReports[i].bootNonce){slot=i;break;}}
    if(slot==12)slot=(remoteNext++%12);
    auto &r=remoteReports[slot];
    if(r.bootNonce!=boot)r={};
    r.bootNonce=boot;
    const uint64_t bits=static_cast<uint64_t>(f.timeUs);
    // Extra report type marker, carried inside the existing authenticated frame.
    // Checked before the legacy success/failure formats; never changes the clock model.
    if(f.type==EspNowTimePackets::Type::NodeSyncReport && (f.nonce&0x2000U)!=0) {
        r.hasLastRejectedSeries=true;
        r.lastRejectedReason=static_cast<uint8_t>((f.nonce>>16)&0x0fU);
        r.lastRejectedAgeMs=((f.nonce>>20)&0x0fffU)*1000UL;
        r.lastRejectedMetricsValid=r.lastRejectedReason!=1 && r.lastRejectedReason!=4;
        r.lastRejectedSpreadUs=static_cast<uint32_t>(bits>>32);
        r.lastRejectedRttUs=static_cast<uint32_t>(bits);
        remoteReceivedAt[slot]=millis();++reportCount;return;
    }
    if(f.type==EspNowTimePackets::Type::NodeSyncReport && (f.nonce&0x1000U)!=0) {
        r.successfulSeries=static_cast<uint32_t>(bits>>32);
        r.rejectedSeries=static_cast<uint32_t>(bits);
        r.lastSeriesResult=static_cast<uint8_t>((f.nonce>>16)&0x0fU);
        const uint32_t ageSeconds=(f.nonce>>20)&0x0fffU;
        r.lastSeriesAgeMs=ageSeconds*1000UL;
        r.hasSeriesStatistics=true;
        remoteReceivedAt[slot]=millis();++reportCount;return;
    }
    if(f.type==EspNowTimePackets::Type::NodeDriftReport && (f.nonce&0x800U)!=0){
        r.failedMinRttOffsetUs=static_cast<int32_t>(static_cast<uint32_t>(bits>>32));
        r.failedSelectedOffsetUs=static_cast<int32_t>(static_cast<uint32_t>(bits));
        r.hasFailureOffsets=true;
        remoteReceivedAt[slot]=millis();++reportCount;return;
    }
    if(f.type==EspNowTimePackets::Type::NodeDriftReport && (f.nonce&0x400U)!=0){
        r.failedSpreadUs=static_cast<uint32_t>(bits>>32);
        r.failedRttUs=static_cast<uint32_t>(bits);
        r.failedAccepted=static_cast<uint8_t>((f.nonce>>16)&0xffU);
        r.failedSelected=static_cast<uint8_t>((f.nonce>>24)&0xffU);
        r.hasFailureMetrics=true;
        remoteReceivedAt[slot]=millis();++reportCount;return;
    }
    if(f.type==EspNowTimePackets::Type::NodeSyncReport){
        if ((f.nonce&0x200U)!=0) { // Diagnostic-only frame: NOT a successful clock sync.
            r.precisionAccepted=false;
            r.hasDriftReport=false;
            r.acceptedSamples=0;
            r.sampleAgeMs=0;
            r.offsetUs=0;
            r.rttUs=0;
            r.probesSent=static_cast<uint32_t>(bits>>32);
            r.exchangesComplete=static_cast<uint32_t>(bits);
            r.failureCode=static_cast<uint8_t>((f.nonce>>16)&0xffU);
            r.failedSeries=static_cast<uint8_t>((f.nonce>>24)&0xffU);
            remoteReceivedAt[slot]=millis();++reportCount;
            return;
        }
        r.failureCode=0;
        r.offsetUs=static_cast<int32_t>(static_cast<uint32_t>(bits>>32));
        r.rttUs=static_cast<uint32_t>(bits);
        r.acceptedSamples=static_cast<uint8_t>(f.nonce&0xffU);
        r.precisionAccepted=(f.nonce&0x100U)!=0;
        r.sampleAgeMs=(f.nonce>>16)*1000UL;
    } else {
        r.driftMilliPpm=static_cast<int32_t>(static_cast<uint32_t>(bits));
        r.spreadUs=static_cast<uint16_t>((bits>>32)&0xffffU);
        r.driftValid=(f.nonce&1U)!=0;
        r.usingEspNowTime=(f.nonce&2U)!=0;
        r.hasTimeSource=true;
        r.hasDriftReport=true;
    }
    remoteReceivedAt[slot]=millis();++reportCount;
}
void sendSeriesStatistics(uint32_t epoch,uint64_t tag,uint32_t boot) {
    // One additional small authenticated frame per 15 s. No SD/NVS writes.
    EspNowTimePackets::Frame f;
    f.type=EspNowTimePackets::Type::NodeSyncReport;
    f.epoch=epoch;f.coordinatorTag=tag;f.senderBoot=boot;
    f.timeUs=static_cast<int64_t>((static_cast<uint64_t>(successfulSeries)<<32)|failedSeries);
    const uint32_t ageSeconds=lastCompletedSeriesMs ?
        std::min(4095UL,static_cast<unsigned long>((millis()-lastCompletedSeriesMs)/1000UL)) : 0;
    f.nonce=0x1000U | (static_cast<uint32_t>(lastCompletedSeriesResult)<<16) | (ageSeconds<<20);
    sendFrame(f,kBroadcastMac);
    // Additive authenticated last-rejection telemetry. Never affects grants,
    // timestamp math, slot ownership or the current successful clock model.
    if(lastRejectedSeriesMs && failedSeries){
        EspNowTimePackets::Frame reject=f;
        const uint32_t ageSeconds=std::min(4095UL,static_cast<unsigned long>((millis()-lastRejectedSeriesMs)/1000UL));
        reject.nonce=0x2000U | (static_cast<uint32_t>(lastRejectedReason)<<16) | (ageSeconds<<20);
        // Metrics may be unavailable for deadlines and invalid estimators.
        const uint32_t spread=lastRejectedHasMetrics && std::isfinite(lastRejectedEstimate.spreadUs) ?
            static_cast<uint32_t>(llround(std::max(0.0,std::min(4294967295.0,lastRejectedEstimate.spreadUs)))) : 0;
        const uint32_t rtt=lastRejectedHasMetrics && std::isfinite(lastRejectedEstimate.medianRttUs) ?
            static_cast<uint32_t>(llround(std::max(0.0,std::min(4294967295.0,lastRejectedEstimate.medianRttUs)))) : 0;
        reject.timeUs=static_cast<int64_t>((static_cast<uint64_t>(spread)<<32)|rtt);
        sendFrame(reject,kBroadcastMac);
        if(lastRejectedHasMetrics){
            // Existing offset-pair report format; only its publication timing changes.
            EspNowTimePackets::Frame offsets=f;
            offsets.type=EspNowTimePackets::Type::NodeDriftReport;
            offsets.nonce=0x800U;
            const int32_t minOff=static_cast<int32_t>(llround(std::max(-2147483647.0,std::min(2147483647.0,lastRejectedEstimate.minRttOffsetUs))));
            const int32_t selectedOff=static_cast<int32_t>(llround(std::max(-2147483647.0,std::min(2147483647.0,lastRejectedEstimate.offsetUs))));
            offsets.timeUs=static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(minOff))<<32)|static_cast<uint32_t>(selectedOff));
            sendFrame(offsets,kBroadcastMac);
        }
    }
}
void sendNodeReports(uint32_t epoch,uint64_t tag,uint32_t boot) {
    if(!lastPrecisionMs){
        // Report progress/failures even without a valid precision estimate.
        // 0x200 flags diagnostics; offset/rtt wire fields carry counters, not time.
        EspNowTimePackets::Frame d;
        d.epoch=epoch;d.coordinatorTag=tag;d.senderBoot=boot;
        d.type=EspNowTimePackets::Type::NodeSyncReport;
        d.timeUs=static_cast<int64_t>((static_cast<uint64_t>(stats.probesSent)<<32)|stats.exchangesComplete);
        d.nonce=0x200U | (static_cast<uint32_t>(lastSeriesFailure)<<16) |
           (static_cast<uint32_t>(std::min(255UL,static_cast<unsigned long>(failedSeries)))<<24);
        sendFrame(d,kBroadcastMac);
        if(lastRejectedEstimate.valid || lastRejectedEstimate.accepted){
            EspNowTimePackets::Frame m=d;
            m.type=EspNowTimePackets::Type::NodeDriftReport;
            const uint32_t spread=static_cast<uint32_t>(llround(std::max(0.0,std::min(4294967295.0,lastRejectedEstimate.spreadUs))));
            const uint32_t rtt=static_cast<uint32_t>(llround(std::max(0.0,std::min(4294967295.0,lastRejectedEstimate.medianRttUs))));
            m.timeUs=static_cast<int64_t>((static_cast<uint64_t>(spread)<<32)|rtt);
            m.nonce=0x400U | (static_cast<uint32_t>(lastRejectedEstimate.accepted)<<16) |
                (static_cast<uint32_t>(lastRejectedEstimate.selected)<<24);
            sendFrame(m,kBroadcastMac);
            EspNowTimePackets::Frame o=m;
            const int32_t minOffset=static_cast<int32_t>(llround(std::max(-2147483647.0,std::min(2147483647.0,lastRejectedEstimate.minRttOffsetUs))));
            const int32_t selectedOffset=static_cast<int32_t>(llround(std::max(-2147483647.0,std::min(2147483647.0,lastRejectedEstimate.offsetUs))));
            o.timeUs=static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(minOffset))<<32)|static_cast<uint32_t>(selectedOffset));
            o.nonce=0x800U;
            sendFrame(o,kBroadcastMac);
        }
        return;
    }
    const uint32_t age=static_cast<uint32_t>(millis()-lastPrecisionMs);
    if(age>65535000UL)return;
    EspNowTimePackets::Frame f;
    f.epoch=epoch;f.coordinatorTag=tag;f.senderBoot=boot;
    f.type=EspNowTimePackets::Type::NodeSyncReport;
    const int32_t offset=static_cast<int32_t>(llround(std::max(-2147483647.0,std::min(2147483647.0,latestQualityEstimate.offsetUs))));
    const uint32_t rtt=static_cast<uint32_t>(llround(std::max(0.0,std::min(4294967295.0,latestQualityEstimate.medianRttUs))));
    f.timeUs=static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(offset))<<32)|rtt);
    f.nonce= (std::min(65535UL,static_cast<unsigned long>(age/1000UL))<<16) |
        0x100U | static_cast<uint32_t>(std::min(255U,static_cast<unsigned>(latestQualityEstimate.accepted)));
    sendFrame(f,kBroadcastMac);
    f.type=EspNowTimePackets::Type::NodeDriftReport;
    const int32_t drift=static_cast<int32_t>(llround(std::max(-2147483647.0,std::min(2147483647.0,stats.driftPpm*1000.0))));
    const uint32_t spread=static_cast<uint32_t>(llround(std::max(0.0,std::min(65535.0,latestQualityEstimate.spreadUs))));
    f.timeUs=static_cast<int64_t>((static_cast<uint64_t>(spread)<<32)|static_cast<uint32_t>(drift));
    int64_t candidate=0;uint32_t candidateAge=0;double candidateRtt=0;
    const bool usingEsp=precisionUtcNow(epoch,referenceId.c_str(),candidate,candidateAge,candidateRtt);
    f.nonce=(stats.driftValid?1U:0U)|(usingEsp?2U:0U);
    sendFrame(f,kBroadcastMac);
}
bool acceptPeer(const uint8_t mac[6],const EspNowTimePackets::Frame &f) {
    PeerReplay* slot=nullptr;
    for(auto &p:peerReplay)if(p.used&&memcmp(p.mac,mac,6)==0){slot=&p;break;}
    if(!slot)for(auto &p:peerReplay)if(!p.used){slot=&p;p.used=true;memcpy(p.mac,mac,6);break;}
    if(!slot)return false;

    const uint32_t now=millis();

    // Beta134-FIX: Bind every time frame to the locally confirmed Coordinator epoch.
    // All roles call service() with currentCoordinatorEpoch (Coordinator: its own),
    // and an epoch change stops/restarts the radio, so legitimate frames always carry
    // lastEpoch. Frames of older or foreign epochs can never open or touch a session,
    // even after their session was evicted from the bounded history below.
    // Equality only - epochs are random, never compare them numerically.
    if(!lastEpoch || f.epoch!=lastEpoch) return false;

    // Beta131-FIX (Fifth cycle, Problem 1): Replay protection for RANDOM boot nonces
    // CRITICAL: Boot IDs are esp_random(), NOT sequential! Cannot use numeric comparison.
    // Beta133-FIX: Coordinator epochs are ALSO esp_random() (makeCoordinatorEpoch() in
    // cluster.cpp), so the former "epoch must never decrease" check rejected a new epoch
    // with ~50% probability - permanently, for every frame of that sender MAC.
    // STRATEGY: A session is (boot, epoch). Track the last 3 sessions with timestamps.
    //           Accept the active session + never-seen sessions; reject recently seen,
    //           no longer active sessions (replay of an older boot or epoch context).

    // Check if this is a known session
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
            if(knownBootIndex >= 0 && knownBootIndex != slot->activeBootIndex) {
                // REJECT: Old session (was seen before, but not currently active)
                // This is a replay attack - sender is trying to use an old boot/epoch context
                return false;
            }
            // Accept: New session (never seen before) - legitimate restart or new epoch
            // Add to history, evict oldest if needed
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
            slot->replay = {};  // Reset replay window for new session
        }
    } else {
        // First packet from this peer - initialize
        slot->activeBootIndex = 0;
        slot->bootHistory[0].boot = f.senderBoot;
        slot->bootHistory[0].epoch = f.epoch;
        slot->bootHistory[0].lastSeenMs = now;
    }

    // Update last seen time for active session
    if(knownBootIndex == slot->activeBootIndex) {
        slot->bootHistory[slot->activeBootIndex].lastSeenMs = now;
    }

    return EspNowTimePackets::acceptSequence(slot->replay,f);
}
void resetTiming(){
    // Beta131-CORRECTED: Do NOT clear replay windows on timing reset (Problem 5 FIX)
    // SECURE: Replay protection must persist across source switches to prevent replay attacks
    // SECURE: Each sender (coordinator/broadcaster) maintains separate replay window via acceptPeer()
    pending={};seriesCount=0;seriesTarget=kInitialSamples;seriesStartedMs=0;nextProbeMs=0;
    // NOTE: peerReplay and coordinatorReplay are NOT cleared - replay protection persists
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
        return; // Do not update the production estimator or drift model.
    }
    if(seriesCount<kMaxSamplesPerSeries)
       exchanges[seriesCount++]={pending.t1,pending.t2,t3,t4};
    pending={};++stats.exchangesComplete;
    if(seriesCount>=seriesTarget) {
        // Beta131-FIX (Twenty-first cycle): Fixed 16-sample series
        // REMOVED: Adaptive extension from 16→32→64 samples
        // REASON: Drift accumulation over long series (up to 12s) causes spread increase
        // Example: -22 ppm drift × 12s = 264 µs additional spread (exceeds 250 µs limit)
        // Fixed 16 samples: ~0.72s × 22 µs/s = 15.8 µs drift impact (well within limit)
        auto e=ClusterEspNowTwoWay::estimate(exchanges,seriesCount);
        acceptEstimate(e);
        seriesCount=0;seriesTarget=kInitialSamples;seriesStartedMs=0;
        nextSeriesMs=millis()+(seriesPrecisionAccepted ? 45000UL : qualityRetryDelayMs());
    }
}
}
bool syncPermissionValid() {
    return initialized && !wasCoordinator && permissionUntilMs != 0 &&
           static_cast<int32_t>(millis()-permissionUntilMs)<0 && !capturePaused();
}
void revokePermissionBroadcast() {
    if (!initialized || !wasCoordinator || !lastEpoch || !lastTag) return;
    EspNowTimePackets::Frame f;
    f.type=EspNowTimePackets::Type::SyncControl;
    f.epoch=lastEpoch;f.coordinatorTag=lastTag;
    // Uses the same authenticated, per-boot replay-protected packet framing.
    f.senderBoot=localBootNonceForControl;
    f.timeUs=0; // Zero = revoke. Positive = lease duration in milliseconds.
    f.nonce=(slotOwner>=0 ? slotPeers[slotOwner].boot : 0);
    sendFrame(f,kBroadcastMac);
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
        slotNextIndex=static_cast<uint8_t>((slotOwner+1)%12);
        slotOwner=-1;
        slotGuardUntilMs=captureGuardUntilMs+kSlotGuardMs;
    }
    coordinatorPermissionIssued=false;
    permissionUntilMs=0;
    // Drop incomplete samples immediately, but preserve valid holdover state.
    pending = {};
    seriesCount = 0;seriesTarget=kInitialSamples;seriesStartedMs=0;
    nextSeriesMs = captureGuardUntilMs + 1000UL;
    nextProbeMs = 0;
}
bool capturePaused() {
    return captureGuardUntilMs != 0 &&
           static_cast<int32_t>(millis() - captureGuardUntilMs) < 0;
}
// Eight prior signed samples; small static memory, no heap or additional RF.
struct DiagnosticPredictionPoint { int64_t tx=0, rx=0; };
constexpr size_t kDiagnosticPredictionHistory=8;
DiagnosticPredictionPoint diagnosticPredictionHistory[kDiagnosticPredictionHistory];
size_t diagnosticPredictionCount=0, diagnosticPredictionWrite=0;
uint64_t diagnosticPredictionTag=0, diagnosticPredictionBoot=0;
uint32_t diagnosticPredictionEpoch=0, diagnosticPredictionSequence=0;
DiagnosticPredictionStats diagnosticPrediction{};
void observeDiagnosticPrediction(const ClusterTimeDiagnosticBeacon::Message &msg,int64_t rx){
    const bool same=msg.senderTag==diagnosticPredictionTag &&
        msg.senderBoot==diagnosticPredictionBoot && msg.epoch==diagnosticPredictionEpoch;
    if(!same || msg.sequence!=diagnosticPredictionSequence+1U){
        diagnosticPredictionCount=diagnosticPredictionWrite=0;
        diagnosticPredictionTag=msg.senderTag;diagnosticPredictionBoot=msg.senderBoot;
        diagnosticPredictionEpoch=msg.epoch;
    }
    diagnosticPredictionSequence=msg.sequence;
    bool valid=false;
    double residual=0;
    if(diagnosticPredictionCount>=4){
        const size_t first=(diagnosticPredictionWrite+kDiagnosticPredictionHistory-diagnosticPredictionCount)%kDiagnosticPredictionHistory;
        const int64_t x0=diagnosticPredictionHistory[first].tx;
        const int64_t y0=diagnosticPredictionHistory[first].rx;
        double sx=0,sy=0,sxx=0,sxy=0;
        for(size_t i=0;i<diagnosticPredictionCount;++i){
            const auto &p=diagnosticPredictionHistory[(first+i)%kDiagnosticPredictionHistory];
            const double x=double(p.tx-x0),y=double(p.rx-y0);
            sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;
        }
        const double n=double(diagnosticPredictionCount),den=n*sxx-sx*sx;
        if(den>1.0 && msg.monotonicUs>x0 && rx>y0){
            const double slope=(n*sxy-sx*sy)/den;
            if(std::isfinite(slope) && fabs(slope-1.0)<=0.0005){
                const double pred=double(y0)+(sy-slope*sx)/n+slope*double(msg.monotonicUs-x0);
                residual=double(rx)-pred;
                valid=std::isfinite(residual) && fabs(residual)<=1000000.0;
            }
        }
        if(!valid)++diagnosticPrediction.invalid;
    }else ++diagnosticPrediction.warmup;
    if(valid){
        const int32_t e=static_cast<int32_t>(llround(residual));
        const uint32_t n=++diagnosticPrediction.valid;
        if(n==1){diagnosticPrediction.minErrorUs=e;diagnosticPrediction.maxErrorUs=e;}
        else {if(e<diagnosticPrediction.minErrorUs)diagnosticPrediction.minErrorUs=e;
              if(e>diagnosticPrediction.maxErrorUs)diagnosticPrediction.maxErrorUs=e;}
        const double d=double(e)-diagnosticPrediction.meanErrorUs;
        diagnosticPrediction.meanErrorUs+=d/double(n);
        diagnosticPrediction.errorM2+=d*(double(e)-diagnosticPrediction.meanErrorUs);
        diagnosticPrediction.meanAbsErrorUs+=(fabs(double(e))-diagnosticPrediction.meanAbsErrorUs)/double(n);
    }
    diagnosticPredictionHistory[diagnosticPredictionWrite]={msg.monotonicUs,rx};
    diagnosticPredictionWrite=(diagnosticPredictionWrite+1)%kDiagnosticPredictionHistory;
    if(diagnosticPredictionCount<kDiagnosticPredictionHistory)++diagnosticPredictionCount;
}
DiagnosticPredictionStats diagnosticPredictionStats(){return diagnosticPrediction;}
void stop() {
    benchAbortInternal(true);
    benchRawCount=0;
    beaconExperimentUntilMs=beaconExperimentIntervalMs=beaconExperimentSent=0;
    captureGuardUntilMs = 0;
    permissionUntilMs=0;coordinatorPermissionIssued=false;localBootNonceForControl=0;
    if(initialized){esp_now_deinit();initialized=false;}
    txTimingReset();
    boundChannel=0;lastEpoch=0;lastTag=0;diagnosticMode=false;nextDiagnosticMs=0;stats.active=false;
    stats.lastExchangeValid=false;stats.lastOffsetUs=0;stats.lastRttUs=0;
    stats.lastSeriesEstimateAvailable=false;stats.lastSeriesEstimateAgeMs=0;
    stats.lastSeriesSelected=0;stats.lastSeriesSpreadUs=0;
    stats.lastSeriesRttMinUs=stats.lastSeriesRttMaxUs=0;
    stats.lastSeriesOffsetP10Us=stats.lastSeriesOffsetP90Us=0;
    resetTiming();clearQueue();diagnosticRx={};
    diagnosticPredictionCount=diagnosticPredictionWrite=0;
    diagnosticPredictionTag=diagnosticPredictionBoot=0;
    diagnosticPredictionEpoch=diagnosticPredictionSequence=0;
    diagnosticPrediction={};
    diagnosticLastTag=diagnosticLastBoot=0;diagnosticLastEpoch=diagnosticLastSequence=0;
    diagnosticLastReceivedMs=diagnosticConsecutive=0;
    shadowHandover.reset();
    grantDiag={};observedBroadcasterTag=observedBroadcasterBoot=0;
    observedBroadcasterAt=grantNextMs=grantTransaction=grantSequence=0;
    grantPendingTransaction=grantPendingSequence=grantPendingAt=0;
    grantPendingTag=grantPendingBoot=0;
    grantLastCoordinatorTag=grantLastCoordinatorBoot=0;
    grantLastCoordinatorEpoch=grantLastCoordinatorSequence=0;
    // Beta131-FIX (Sixth cycle, Problem 3): Clear boot history on broadcaster reset
    for(auto &h : grantCoordinatorBootHistory) h = {};
    grantActiveBootIndex = 0;
    memset(observedBroadcasterMac,0,6);memset(grantPendingMac,0,6);
    // Beta131-CORRECTED: Reset broadcaster and source tracking state
    broadcasterLeaseActive=false;broadcasterLeaseActivationMs=0;
    broadcasterLeaseExpiresMs=0;broadcasterLeaseGrantedAt=0;
    broadcasterProductionBeaconsSent=0;
    broadcasterActivationTransaction=0;broadcasterTimeBaseValid=false;
    coordinatorGrantedLeaseActive=false;coordinatorGrantedLeaseExpiresMs=0;
    coordinatorSuppressionStartMs=0;  // Beta131-FIX (Tenth cycle): Clear suppression timing
    coordinatorBeaconsSuppressed=0;coordinatorLeaseTransaction=0;
    coordinatorLeaseBroadcasterTag=0;coordinatorLeaseBroadcasterBoot=0;
    lastProductionBeaconFromLeasedBroadcasterMs=0;  // Beta132-V2-FIX: Clear broadcaster tracking
    coordinatorLastSlotSuppressed=false;
    activeTimeSourceTag=0;activeTimeSourceBoot=0;activeTimeSourceEpoch=0;
    // Beta131-FIX (Ninth cycle, Problem 2): Increment sourceChanges on stop, NOT reset
    // CRITICAL: Ensures pending captures become invalid after radio restart
    // Monotonic generation prevents old captures from matching new generation values
    ++sourceChanges;
    memset(activeTimeSourceMac,0,6);  // Beta131-FIX (Seventh cycle, Problem 2)
    for(auto &p:slotPeers)p={};slotNextIndex=0;slotOwner=-1;
    slotStartedMs=slotGuardUntilMs=0;slotGrantsSent=slotRotations=0;
    previousEstimate={};latestQualityEstimate={};lastPrecisionMs=0;failedSeries=0;for(auto &r:seriesRejectByReason)r=0;lastSeriesFailure=0;lastRejectedReason=0;lastRejectedSeriesMs=0;lastRejectedHasMetrics=false;lastRejectedEstimate={};consecutiveFailures=0;
    successfulSeries=0;lastCompletedSeriesMs=0;lastCompletedSeriesResult=0;nextSeriesStatsReportMs=0;lastEstimateMs=0;
    referenceUtcUs=0;referenceMonoLow=0;referenceReceivedNodeMono=0;
    beaconObsCount=beaconObsWrite=0;beaconStats={};sharedEventCount=sharedEventWrite=0;sharedEventBoot=0;sharedEventEpoch=0;
    referenceId=String();goodDriftPairs=0;driftHistoryCount=0;
    for(auto &r:remoteReports)r={};for(auto &x:remoteReceivedAt)x=0;remoteNext=0;reportCount=0;
    stats.driftValid=false;stats.storedDriftAvailable=false;stats.driftPpm=0;
    stats.driftUncertaintyPpm=0;stats.driftSamples=0;
}
// One-step-ahead least-squares model. It uses only earlier authenticated
// beacons from the same sender boot/epoch, and leaves the capture clock alone.
// Offsets are intentionally local: this models intervals, not absolute UTC.
void predictBeacon(const EspNowTimePackets::Frame &f,int64_t nodeRx,BeaconObservation &out){
    if(beaconObsCount<4){out.predictionStatus=1;return;}
    const size_t first=(beaconObsWrite+kBeaconObservations-beaconObsCount)%kBeaconObservations;
    const BeaconObservation &origin=beaconObservations[first];
    const uint32_t span=static_cast<uint32_t>(f.nonce-origin.coordinatorMonoLow);
    if(f.senderBoot!=origin.coordinatorBoot || f.epoch!=origin.epoch){out.predictionStatus=2;return;}
    if(span>600000000UL){out.predictionStatus=3;return;}
    if(nodeRx<=origin.nodeRxCallbackUs){out.predictionStatus=4;return;}
    double sx=0,sy=0,sxx=0,sxy=0;
    for(size_t i=0;i<beaconObsCount;++i){
        const auto &o=beaconObservations[(first+i)%kBeaconObservations];
        if(o.coordinatorBoot!=f.senderBoot || o.epoch!=f.epoch){out.predictionStatus=5;return;}
        const uint32_t dx=static_cast<uint32_t>(o.coordinatorMonoLow-origin.coordinatorMonoLow);
        if(dx>span){out.predictionStatus=5;return;}
        const double x=static_cast<double>(dx);
        const double y=static_cast<double>(o.nodeRxCallbackUs-origin.nodeRxCallbackUs);
        sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;
    }
    const double n=static_cast<double>(beaconObsCount);
    const double denominator=n*sxx-sx*sx;
    if(denominator<1.0){out.predictionStatus=6;return;}
    const double slope=(n*sxy-sx*sy)/denominator;
    if(!std::isfinite(slope) || fabs(slope-1.0)>0.0005){out.predictionStatus=7;return;}
    const double intercept=(sy-slope*sx)/n;
    const double predicted=static_cast<double>(origin.nodeRxCallbackUs)+
        intercept+slope*static_cast<double>(span);
    const double error=static_cast<double>(nodeRx)-predicted;
    const double ppm=(slope-1.0)*100000000.0;
    if(!std::isfinite(error) || fabs(error)>2147483647.0 || !std::isfinite(ppm)){out.predictionStatus=8;return;}
    out.predictionErrorUs=static_cast<int32_t>(llround(error));
    out.driftPpmX100=static_cast<int32_t>(llround(ppm));
    out.predictionSamples=static_cast<uint8_t>(beaconObsCount);
}
size_t beaconSharedEventCount(){return sharedEventCount;}
bool beaconSharedEventAt(size_t index, BeaconSharedEvent &out){
    if(index>=sharedEventCount)return false;
    const size_t first=(sharedEventWrite+kSharedEventCapacity-sharedEventCount)%kSharedEventCapacity;
    out=sharedEvents[(first+index)%kSharedEventCapacity];
    return true;
}
uint64_t beaconSharedEventBoot(){return sharedEventBoot;}
BeaconPredictionStats beaconPredictionStats(){return beaconStats;}
size_t beaconObservationCount(){return beaconObsCount;}
bool beaconObservationAt(size_t index, BeaconObservation &out){
    if(index>=beaconObsCount)return false;
    const size_t first=(beaconObsWrite+kBeaconObservations-beaconObsCount)%kBeaconObservations;
    out=beaconObservations[(first+index)%kBeaconObservations];
    return true;
}
bool precisionUtcNow(uint32_t epoch, const char *coordinatorId,
                    int64_t &utcUs, uint32_t &sampleAgeMs, double &rttUs) {
    // Beta131-DIAGNOSIS (Seventeenth cycle): Detailed guard tracking
    if (!initialized || !coordinatorId || !referenceUtcUs || !referenceReceivedNodeMono) {
        ++precisionGuardOther;  // Basic initialization/validity
        return false;
    }
    if (wasCoordinator) {
        ++precisionGuardWasCoordinator;
        return false;
    }
    if (epoch != lastEpoch) {
        ++precisionGuardEpochMismatch;
        return false;
    }
    if (idTag(coordinatorId) != lastTag) {
        ++precisionGuardTagMismatch;
        return false;
    }
    if (!latestQualityEstimate.valid) {
        ++precisionGuardNotValid;
        return false;
    }
    if (!lastPrecisionMs || (uint32_t)(millis()-lastPrecisionMs)>kPrecisionLifetimeMs) {
        ++precisionGuardOther;  // Precision lifetime expired
        return false;
    }
    const int64_t now=esp_timer_get_time();
    // Beta131-FIX (Nineteenth/Twentieth cycle): Broadcaster-specific extended reference lifetime
    // CRITICAL: Broadcaster receives beacons BEFORE activation but coordinator suppresses
    // beacons DURING broadcaster lease. Forward-propagation (line 590-613) keeps reference fresh.
    // Fallback: Extended lifetime if forward-propagation fails or no new precision series accepted.
    // Use broadcasterLeaseExpiresMs (not broadcasterLeaseActive) to detect broadcaster context
    // even after lease expires or gets revoked, preventing Catch-22 where revocation disables fix.
    // Normal nodes: 30s (fresh beacons), Broadcaster: 120s (forward-propagated or holdover)
    const int64_t maxReferenceAge = broadcasterLeaseExpiresMs ?
        static_cast<int64_t>(kPrecisionLifetimeMs) * 1000LL : kReferenceLifetimeUs;
    if (now<referenceReceivedNodeMono || now-referenceReceivedNodeMono>maxReferenceAge) {
        ++precisionGuardRefLifetime;  // Reference lifetime expired (30s node, 120s broadcaster)
        return false;
    }
    const double age=static_cast<double>(now-latestQualityEstimate.nodeReferenceUs);
    if (age<0 || age>static_cast<double>(kPrecisionLifetimeMs)*1000.0) {
        ++precisionGuardEstimateAge;  // Estimate age exceeds lifetime (120s)
        return false;
    }
    const double drift=stats.driftValid ? stats.driftPpm : 0.0;
    if (!std::isfinite(drift) || fabs(drift)>500.0) {
        ++precisionGuardDrift;  // Drift invalid or excessive
        return false;
    }
    const double coordinatorNow=static_cast<double>(now)+latestQualityEstimate.offsetUs+
        age*drift/1000000.0;
    // Reconstruct 64-bit coordinator monotonic time from the beacon's low
    // 32 bits, choosing the unique wrap nearest to the current estimate.
    const int64_t approx=static_cast<int64_t>(llround(coordinatorNow));
    const int32_t delta=static_cast<int32_t>(referenceMonoLow-static_cast<uint32_t>(approx));
    const int64_t referenceMono=approx+delta;
    // Beta131-FIX (Twentieth cycle): Use broadcaster-aware limit for reconstruction check
    // Must match maxReferenceAge logic to prevent inconsistent failures
    if (referenceMono<=0 || llabs(referenceMono-approx)>maxReferenceAge) {
        ++precisionGuardMonoReconstruct;  // Reference mono reconstruction failed
        return false;
    }
    const long double target=(long double)referenceUtcUs+(long double)coordinatorNow-referenceMono;
    if(target<1577836800000000.0L || target>4102444800000000.0L) {
        ++precisionGuardUtcRange;  // UTC target out of valid range
        return false;
    }
    utcUs=static_cast<int64_t>(llround(target));
    sampleAgeMs=(uint32_t)(millis()-lastPrecisionMs);
    rttUs=latestQualityEstimate.medianRttUs;
    return true;
}
size_t nodeSyncReports(NodeSyncReport* out,size_t capacity){
    if(!out||!capacity||!initialized||!wasCoordinator)return 0;
    size_t count=0;
    for(size_t i=0;i<12&&count<capacity;++i){
        if(!remoteReports[i].bootNonce)continue;
        out[count]=remoteReports[i];
        out[count].receivedAgeMs=static_cast<uint32_t>(millis()-remoteReceivedAt[i]);
        ++count;
    }
    return count;
}
DiagnosticBeaconRxStats diagnosticBeaconRxStats(){
    DiagnosticBeaconRxStats out=diagnosticRx;
    if(diagnosticLastReceivedMs){
        out.candidateAgeMs=static_cast<uint32_t>(millis()-diagnosticLastReceivedMs);
        out.candidateFresh=diagnosticConsecutive>=3 && out.candidateAgeMs<=3500U;
    }
    out.candidateConsecutive=diagnosticConsecutive;
    // Observation-only state transitions. Snapshot does not touch ESP-NOW TX.
    shadowHandover.observe(out.candidateFresh, diagnosticConsecutive,
        diagnosticLastTag, diagnosticLastBoot, diagnosticLastEpoch);
    const auto shadow=shadowHandover.state();
    out.shadowActive=shadow.simulatedActive;
    out.shadowActivations=shadow.simulatedActivations;
    out.shadowFallbacks=shadow.simulatedFallbacks;
    out.shadowSourceChanges=shadow.sourceChanges;
    return out;
}
bool popGrant(GrantQueued &v) {
    bool ok=false;
    portENTER_CRITICAL(&queueMux);
    if(grantTail!=grantHead) {
        v=grantQueue[grantTail];
        grantTail=static_cast<uint8_t>((grantTail+1)%kGrantQueueSize);
        ok=true;
    }
    portEXIT_CRITICAL(&queueMux);
    return ok;
}
// Beta131-CORRECTED: Secure Grant/Ack protocol with transaction confirmation and device binding.
// Fixes Problem 3 (Secure Handover) and Problem 4 (Trusted Device/Role Binding).
// - SECURE: Transaction-based confirmation ensures both sides activate at same boundary
// - SECURE: Device identity (tag + boot + MAC) prevents impersonation
// - SECURE: Replay protection via sequence numbers and transaction IDs
// - FAIL-CLOSED: Invalid/lost messages result in safe fallback to coordinator
void serviceGrantDiagnostic(uint32_t localBoot) {
    if(!initialized || capturePaused() || !lastEpoch || !lastTag || !localBoot) return;
    const uint32_t now=millis();
    if(grantPendingTransaction && uint32_t(now-grantPendingAt)>2000U) {
        ++grantDiag.timeouts;grantPendingTransaction=0;
        // Beta131-CORRECTED: Timeout clears pending state but does NOT revoke lease (Problem 2 FIX)
        // CRITICAL: Coordinator keeps beacons suppressed until lease expires naturally
        // This prevents overlap if Ack was lost but broadcaster activated successfully
        // Lease will expire via normal expiry check (line 1893)
        Serial.printf("[COORDINATOR] Grant timeout (txn %u) - no Ack, but lease continues until expiry\n",
            coordinatorLeaseTransaction);
    }
    GrantQueued item{};
    unsigned budget=0;
    while(budget++<3 && popGrant(item)) {
        ++grantDiag.received;
        ClusterTimeGrantProtocol::Message m{};
        if(!ClusterTimeGrantProtocol::decode(item.bytes,sizeof(item.bytes),m,clusterTimeGrantVerify)) {
            ++grantDiag.rejected;continue;
        }
        ++grantDiag.authenticated;

        // Beta131-CORRECTED (Third cycle): Broadcaster receives Grant (Problem 2 FIX)
        // CRITICAL: Separate broadcaster's OWN identity from coordinator identity
        // - broadcasterOwnTag: THIS device (checked against m.broadcasterTag)
        // - lastTag: Time source identity (coordinator, for sync)
        const bool isBroadcaster = !wasCoordinator && broadcasterOwnTag &&
                                   m.action==ClusterTimeGrantProtocol::Action::Grant;
        if(isBroadcaster) {
            // SECURE: Verify device identity binding - Grant addressed to THIS broadcaster
            // Problem 2 FIX: Check against broadcasterOwnTag, NOT lastTag (which is coordinator)
            if(m.broadcasterTag!=broadcasterOwnTag || m.broadcasterBoot!=localBoot) {
                ++grantDiag.rejected;
                Serial.printf("[BROADCASTER] Grant REJECTED - wrong identity (expected %llx:%u, got %llx:%u)\n",
                    broadcasterOwnTag, localBoot, m.broadcasterTag, (uint32_t)m.broadcasterBoot);
                continue;
            }

            // SECURE: Verify coordinator identity consistency
            // lastTag is coordinator's tag (time source identity)
            if(m.coordinatorTag!=lastTag || m.coordinatorEpoch!=lastEpoch) {
                ++grantDiag.rejected;
                Serial.printf("[BROADCASTER] Grant REJECTED - coordinator mismatch (expected %llx:%u, got %llx:%u)\n",
                    lastTag, lastEpoch, m.coordinatorTag, m.coordinatorEpoch);
                continue;
            }

            // Beta131-FIX (Sixth cycle, Problem 3): Boot history replay protection for Grant/Ack
            // CRITICAL: Allow legitimate coordinator reboots while rejecting replay attacks
            // Similar logic to acceptPeer() but for Grant/Ack protocol path

            // Check if coordinator boot is in boot history
            int8_t knownBootIndex = -1;
            for(uint8_t i=0; i<3; ++i) {
                if(grantCoordinatorBootHistory[i].boot == m.coordinatorBoot &&
                   grantCoordinatorBootHistory[i].boot != 0) {
                    knownBootIndex = i;
                    break;
                }
            }

            const bool bootChanged = (grantLastCoordinatorTag != 0) &&
                                     (m.coordinatorBoot != grantLastCoordinatorBoot);
            const bool tagOrEpochChanged = (grantLastCoordinatorTag != 0) &&
                                           (m.coordinatorTag != grantLastCoordinatorTag ||
                                            m.coordinatorEpoch != grantLastCoordinatorEpoch);

            // REJECT: Tag/Epoch changed (different coordinator)
            if(tagOrEpochChanged) {
                ++grantDiag.rejected;
                Serial.printf("[BROADCASTER] Grant REJECTED - coordinator tag/epoch changed (old %llx:%u, new %llx:%u)\n",
                    grantLastCoordinatorTag, grantLastCoordinatorEpoch, m.coordinatorTag, m.coordinatorEpoch);
                continue;
            }

            // REJECT: Old boot session (replay attack)
            // Boot was seen before but is not the currently active boot
            if(bootChanged && knownBootIndex >= 0 && knownBootIndex != grantActiveBootIndex) {
                ++grantDiag.replay;
                Serial.printf("[BROADCASTER] Grant REJECTED - old boot session replay (boot %llu, active index %u, known index %d)\n",
                    m.coordinatorBoot, grantActiveBootIndex, knownBootIndex);
                continue;
            }

            // ACCEPT: New boot session (legitimate coordinator reboot)
            if(bootChanged && knownBootIndex < 0) {
                // Find oldest boot entry to evict
                uint8_t oldestIndex = 0;
                uint32_t oldestTime = grantCoordinatorBootHistory[0].lastSeenMs;
                for(uint8_t i=1; i<3; ++i) {
                    if(grantCoordinatorBootHistory[i].lastSeenMs < oldestTime) {
                        oldestTime = grantCoordinatorBootHistory[i].lastSeenMs;
                        oldestIndex = i;
                    }
                }
                grantActiveBootIndex = oldestIndex;
                grantCoordinatorBootHistory[oldestIndex].boot = m.coordinatorBoot;
                grantCoordinatorBootHistory[oldestIndex].lastSeenMs = now;
                grantLastCoordinatorSequence = 0;  // Reset sequence tracking for new boot
                Serial.printf("[BROADCASTER] Grant ACCEPTED - new boot session (boot %llu, index %u)\n",
                    m.coordinatorBoot, grantActiveBootIndex);
            }
            // ACCEPT: Same boot session (continuation)
            else if(!bootChanged || (knownBootIndex == grantActiveBootIndex)) {
                // Update last seen time for active boot
                if(knownBootIndex == grantActiveBootIndex) {
                    grantCoordinatorBootHistory[grantActiveBootIndex].lastSeenMs = now;
                } else if(grantLastCoordinatorTag == 0) {
                    // First Grant ever - initialize boot history
                    grantActiveBootIndex = 0;
                    grantCoordinatorBootHistory[0].boot = m.coordinatorBoot;
                    grantCoordinatorBootHistory[0].lastSeenMs = now;
                }

                // SECURE: Replay protection via sequence numbers (within same boot)
                if(grantLastCoordinatorTag && m.sequence <= grantLastCoordinatorSequence) {
                    ++grantDiag.replay;
                    Serial.printf("[BROADCASTER] Grant REJECTED - sequence replay (seq %u <= %u)\n",
                        m.sequence, grantLastCoordinatorSequence);
                    continue;
                }
            }

            // Track coordinator identity and sequence
            grantLastCoordinatorTag=m.coordinatorTag;
            grantLastCoordinatorBoot=m.coordinatorBoot;
            grantLastCoordinatorEpoch=m.coordinatorEpoch;
            grantLastCoordinatorSequence=m.sequence;

            // Beta131-FIX (Thirteenth cycle): Bootstrap coordinator identity from Grant
            // CRITICAL: After broadcaster restart, coordinator suppresses beacons (active lease)
            // Grant contains authenticated coordinator identity - use for bootstrap
            // This enables Probe/Reply/Final exchanges without prior beacon reception
            // Beta131-DIAGNOSIS (Fourteenth cycle): Detect beacon-grant race condition
            if(!activeTimeSourceTag) {
                activeTimeSourceTag = m.coordinatorTag;
                activeTimeSourceBoot = m.coordinatorBoot;
                activeTimeSourceEpoch = m.coordinatorEpoch;
                memcpy(activeTimeSourceMac, item.mac, 6);  // Coordinator's MAC from Grant sender
                Serial.printf("[BOOTSTRAP] Coordinator identity from Grant: tag=%llx boot=%llx epoch=%u\n",
                    activeTimeSourceTag, activeTimeSourceBoot, activeTimeSourceEpoch);
            } else {
                // DIAGNOSIS: Grant-Bootstrap skipped because activeTimeSourceTag already set
                Serial.printf("[GRANT-RX] Bootstrap SKIPPED - activeTimeSource already set:\n");
                Serial.printf("  Existing: tag=%llx boot=%llx epoch=%u\n",
                    activeTimeSourceTag, activeTimeSourceBoot, activeTimeSourceEpoch);
                Serial.printf("  Grant:    tag=%llx boot=%llx epoch=%u\n",
                    m.coordinatorTag, m.coordinatorBoot, m.coordinatorEpoch);
                if(activeTimeSourceEpoch != m.coordinatorEpoch) {
                    Serial.printf("  ⚠️  EPOCH MISMATCH: existing=%u grant=%u (delta=%d)\n",
                        activeTimeSourceEpoch, m.coordinatorEpoch,
                        static_cast<int32_t>(m.coordinatorEpoch - activeTimeSourceEpoch));
                }
            }

            // Beta131-CORRECTED (Third cycle): Problem 3 FIX - Lease renewal support
            // Beta131-FIX (Tenth cycle, CORRECTED): Activation delay MUST exceed coordinator suppression
            // CRITICAL: Distinguish initial activation from renewal
            // Initial: Schedule activation after 1500ms (coordinator suppresses at 1000ms)
            // Renewal: Already active, just extend expiry time
            // TIMING: Coordinator suppression = Grant_send + 1000ms
            //         Broadcaster activation = Grant_receive + 1500ms
            //         With RF_delay ≤ 50ms: Activation at ~1550ms > Suppression at 1000ms
            //         Safety margin: 550ms (prevents overlap)
            if(m.leaseMs>=500U && m.leaseMs<=10000U && m.transaction) {
                const bool isRenewal = broadcasterLeaseActive;

                if(isRenewal) {
                    // Lease renewal - already active, just extend expiry
                    broadcasterLeaseExpiresMs=now+m.leaseMs;
                    Serial.printf("[BROADCASTER] Lease RENEWED (txn %u): expiry extended to +%u ms\n",
                        m.transaction, m.leaseMs);
                } else {
                    // Initial activation - schedule with guard time to prevent coordinator overlap
                    constexpr uint32_t kActivationDelayMs = 1500U;  // MUST be > coordinator suppression delay
                    broadcasterActivationTransaction=m.transaction;
                    broadcasterLeaseActivationMs=now+kActivationDelayMs;
                    broadcasterLeaseExpiresMs=now+m.leaseMs;
                    broadcasterLeaseGrantedAt=now;
                    Serial.printf("[BROADCASTER] Grant ACCEPTED (txn %u): activation at +%u ms, expiry at +%u ms\n",
                        m.transaction, kActivationDelayMs, m.leaseMs);
                }
            } else {
                ++grantDiag.rejected;
                Serial.println("[BROADCASTER] Grant REJECTED - invalid lease parameters");
                continue;
            }

            // Send Ack with transaction confirmation
            m.action=ClusterTimeGrantProtocol::Action::Ack;
            uint8_t wire[ClusterTimeGrantProtocol::kWireBytes]={};
            if(ClusterTimeGrantProtocol::encode(m,wire,sizeof(wire),clusterTimeGrantSign) &&
               ensurePeer(item.mac) && espNowSendTracked(item.mac,wire,sizeof(wire))==ESP_OK)
                ++grantDiag.acksSent;
            else {
                ++grantDiag.sendFailed;
                // FAIL-CLOSED: Ack failed, cancel activation
                broadcasterActivationTransaction=0;
                broadcasterLeaseActivationMs=0;
                Serial.println("[BROADCASTER] Ack FAILED - activation cancelled");
            }
        }
        // Beta131-CORRECTED: Coordinator receives Ack (Problem 3 & 4)
        else if(wasCoordinator && m.action==ClusterTimeGrantProtocol::Action::Ack) {
            // SECURE: Verify transaction matches pending grant (Problem 3)
            if(!grantPendingTransaction || uint32_t(now-grantPendingAt)>2000U ||
               m.transaction!=grantPendingTransaction || m.sequence!=grantPendingSequence) {
                ++grantDiag.rejected;
                Serial.println("[COORDINATOR] Ack REJECTED - transaction mismatch or timeout");
                continue;
            }

            // SECURE: Verify coordinator identity (this device) (Problem 4)
            if(m.coordinatorTag!=lastTag || m.coordinatorBoot!=localBoot || m.coordinatorEpoch!=lastEpoch) {
                ++grantDiag.rejected;continue;
            }

            // SECURE: Verify broadcaster identity matches granted device (Problem 4)
            if(m.broadcasterTag!=grantPendingTag || m.broadcasterBoot!=grantPendingBoot ||
               memcmp(item.mac,grantPendingMac,6)!=0) {
                ++grantDiag.rejected;
                Serial.println("[COORDINATOR] Ack REJECTED - broadcaster identity mismatch");
                continue;
            }

            ++grantDiag.acksVerified;
            grantPendingTransaction=0;

            // Beta131-FIX (Tenth cycle, CORRECTED): Ack only confirms broadcaster receipt
            // TIMING: Suppression already scheduled on Grant send (1000ms delay)
            // No timing changes needed here - just log confirmation
            Serial.printf("[COORDINATOR] Ack VERIFIED (txn %u) - broadcaster confirmed (suppression already scheduled)\n",
                m.transaction);
        } else {
            ++grantDiag.rejected;  // Unknown action or role mismatch
        }
    }
    // Beta131-CORRECTED: Coordinator sends Grant (Problem 3 & 4)
    // REMOVED diagnosticMode check - broadcaster detection now uses diagnostic beacon observation
    // SECURE: Only grant to observed broadcaster with fresh beacons
    // Beta131-FIX (Eighth cycle, Problem 1): No grants during startup guard
    if(!wasCoordinator || !observedBroadcasterAt ||
       diagnosticConsecutive<3 || uint32_t(now-observedBroadcasterAt)>3500U ||
       (grantNextMs && static_cast<int32_t>(now-grantNextMs)<0) ||
       grantPendingTransaction ||
       (coordinatorStartupGuardUntilMs && now < coordinatorStartupGuardUntilMs)) return;

    // SECURE: Verify broadcaster is not the coordinator itself (Problem 4)
    if(observedBroadcasterTag==lastTag) return;

    // Beta131-FIX (Fourth cycle, Problem 4): SECURITY LIMITATION DOCUMENTED
    // LIMITATION: Cluster HMAC proves cluster membership, NOT individual device identity
    // LIMITATION: Tag/Boot/MAC are self-reported, not cryptographically bound to device
    // LIMITATION: Any cluster member can CLAIM to be broadcaster (no PKI, no device-specific keys)
    // MITIGATION: Broadcaster role requires explicit mode configuration (cfg_operating_mode="time_broadcaster")
    // MITIGATION: Production deployments should use firewall/network isolation for broadcaster device
    // MITIGATION: Diagnostic beacons require consecutive authenticity (diagnosticConsecutive>=3)
    // RISK: Malicious cluster member could impersonate broadcaster, inject false time
    // ACCEPTED: Current design trades PKI complexity for operational simplicity in trusted environments

    // Beta131-FIX (Sixth cycle, Problem 3): BOOT HISTORY REPLAY PROTECTION LIMITATION
    // LIMITATION: 3-entry boot history prevents replay after ≤3 reboots
    // LIMITATION: Attacker with >3 old boot contexts could replay after sufficient coordinator reboots
    // SCENARIO: Coordinator reboots 4 times → oldest boot evicted → attacker can replay boot #1
    // MITIGATION: Boot history timestamps track "last seen" to detect stale replays
    // MITIGATION: Legitimate reboots happen infrequently (minutes/hours apart, not seconds)
    // MITIGATION: Replay window resets per boot (sequence numbers prevent in-session replay)
    // RISK: Sophisticated attacker with multiple captured boots + frequent forced reboots
    // ACCEPTED: 3-entry limit balances memory (24 bytes) vs. attack complexity in trusted environment

    // Beta131-CORRECTED (Third cycle): Problem 3 FIX - Lease renewal timing
    // CRITICAL: Grant interval MUST be < lease duration for seamless renewal
    // Grant every 2s, Lease 3s → Broadcaster gets renewal 1s BEFORE current lease expires
    grantNextMs=now+2000U;
    ClusterTimeGrantProtocol::Message m{};
    m.action=ClusterTimeGrantProtocol::Action::Grant;
    m.coordinatorTag=lastTag;m.coordinatorBoot=localBoot;
    m.broadcasterTag=observedBroadcasterTag;  // SECURE: Bind to specific device identity
    m.broadcasterBoot=observedBroadcasterBoot;  // SECURE: Bind to specific boot instance
    m.coordinatorEpoch=lastEpoch;
    m.transaction=++grantTransaction;if(!m.transaction)m.transaction=++grantTransaction;
    m.sequence=++grantSequence;if(!m.sequence)m.sequence=++grantSequence;
    m.leaseMs=3000; // Beta131-CORRECTED: 3-second production lease
    uint8_t wire[ClusterTimeGrantProtocol::kWireBytes]={};
    if(!ClusterTimeGrantProtocol::encode(m,wire,sizeof(wire),clusterTimeGrantSign) ||
       !ensurePeer(observedBroadcasterMac) ||
       espNowSendTracked(observedBroadcasterMac,wire,sizeof(wire))!=ESP_OK) {
        ++grantDiag.sendFailed;return;
    }
    ++grantDiag.grantsSent;
    // SECURE: Store pending grant details for Ack verification (Problem 3 & 4)
    grantPendingTransaction=m.transaction;grantPendingSequence=m.sequence;grantPendingAt=now;
    grantPendingTag=m.broadcasterTag;grantPendingBoot=m.broadcasterBoot;
    memcpy(grantPendingMac,observedBroadcasterMac,6);

    // Beta131-FIX (Fourth cycle, Problem 2+3): Conservative lease boundaries with renewal logic
    // CRITICAL: Add 1000ms safety margin to prevent coordinator resuming before broadcaster stops
    // CRITICAL: Only extend lease if renewal (active) AND expiry soon, prevents unbounded extension
    // Coordinator lease = Grant send time + leaseMs + 1000ms margin
    // Broadcaster lease = Grant receive time + leaseMs (no margin)
    // Guarantees gap (no overlap) even with RF delays, queue delays, clock drift
    const uint32_t kCoordinatorSafetyMarginMs = 1000U;
    const uint32_t newExpiryMs = now+m.leaseMs+kCoordinatorSafetyMarginMs;
    const bool isRenewal = coordinatorGrantedLeaseActive;
    const uint32_t remainingMs = isRenewal && now<coordinatorGrantedLeaseExpiresMs ?
                                  coordinatorGrantedLeaseExpiresMs-now : 0;

    // Beta131-FIX (Tenth cycle, Problem 1): Bootstrap-safe beacon suppression
    // CRITICAL: Don't suppress on INITIAL grant - broadcaster needs beacons to build time base
    // Only suppress on RENEWAL (broadcaster already active and sending production beacons)
    // Initial Grant: Send grant but DON'T suppress coordinator beacons yet
    // Renewal: Broadcaster already active, extend suppression
    if(isRenewal) {
        // Renewal with ≤2s remaining: extend lease
        if(remainingMs<=2000U) {
            coordinatorGrantedLeaseExpiresMs=newExpiryMs;
            Serial.printf("[COORDINATOR] Grant RENEWAL (txn %u) - suppression extended to +%u ms\n",
                m.transaction, m.leaseMs+kCoordinatorSafetyMarginMs);
        } else {
            // Renewal with sufficient time remaining: don't extend (prevents unbounded extension)
            Serial.printf("[COORDINATOR] Grant SENT (txn %u) - suppression continues (%u ms remaining)\n",
                m.transaction, remainingMs);
        }
    } else {
        // Beta132-V2-FIX: Grace period + condition-based suppression for V2 bootstrap
        // CRITICAL: V2 requires successful 16-sample series before broadcaster can activate
        // TIMING: Grace period = 10000ms from Grant send (allows multiple series attempts)
        //         After grace: Coordinator only suppresses when broadcaster CONFIRMED active
        //         No timer-based trust: Coordinator verifies production beacons before suppressing
        // SAFETY: Zero double production (suppress only when broadcaster confirmed)
        //         Zero deadlock (coordinator continues if broadcaster fails)
        constexpr uint32_t kSuppressionDelayMs = 10000U;  // V2: Grace period for bootstrap
        coordinatorLeaseTransaction=m.transaction;
        coordinatorLeaseBroadcasterTag=m.broadcasterTag;
        coordinatorLeaseBroadcasterBoot=m.broadcasterBoot;
        coordinatorGrantedLeaseExpiresMs=newExpiryMs;
        coordinatorGrantedLeaseActive=true;  // Activate lease tracking
        coordinatorSuppressionStartMs=now+kSuppressionDelayMs;  // Schedule condition check
        Serial.printf("[COORDINATOR] Grant INITIAL (txn %u) - grace period %u ms, then condition-based suppression\n",
            m.transaction, kSuppressionDelayMs);
    }
}
GrantDiagnosticStats grantDiagnosticStats() {return grantDiag;}
void serviceDiagnosticReceive() {
    if(!initialized || diagnosticMode || capturePaused())return;
    DiagnosticQueued item{};
    unsigned budget=0;
    while(budget++<3 && popDiagnostic(item)) {
        ++diagnosticRx.received;
        ClusterTimeDiagnosticBeacon::Message msg{};
        if(!ClusterTimeDiagnosticBeacon::decode(item.bytes,sizeof(item.bytes),msg,
                                                clusterEspNowDiagnosticBeaconVerify)) {
            ++diagnosticRx.rejected;
            continue;
        }
        if(msg.senderTag==lastTag || msg.epoch!=lastEpoch) {
            ++diagnosticRx.rejected;continue;
        }
        if(msg.senderTag==diagnosticLastTag && msg.senderBoot==diagnosticLastBoot &&
           msg.epoch==diagnosticLastEpoch && msg.sequence<=diagnosticLastSequence) {
            ++diagnosticRx.replay;continue;
        }
        const bool sameDiagnosticSource=(msg.senderTag==diagnosticLastTag &&
            msg.senderBoot==diagnosticLastBoot && msg.epoch==diagnosticLastEpoch);
        const uint32_t nowMs=millis();
        const bool contiguous=sameDiagnosticSource &&
            msg.sequence==diagnosticLastSequence+1U && diagnosticLastReceivedMs &&
            static_cast<uint32_t>(nowMs-diagnosticLastReceivedMs)<=3500U;
        diagnosticConsecutive=contiguous ?
            (diagnosticConsecutive<UINT32_MAX ? diagnosticConsecutive+1U : UINT32_MAX) : 1U;
        diagnosticLastReceivedMs=nowMs;
        const uint32_t previousDiagnosticSequence=diagnosticLastSequence;
        const int64_t previousDiagnosticLocalUs=diagnosticRx.lastRxCallbackUs;
        const int64_t previousDiagnosticRemoteUs=diagnosticRx.lastRemoteMonotonicUs;
        // A signed beacon is only a candidate MAC correlation, not proof of
        // role or device ownership. Reset outstanding diagnostic transactions.
        if (!sameDiagnosticSource ||
            (observedBroadcasterAt && memcmp(observedBroadcasterMac,item.mac,6)!=0)) {
            grantPendingTransaction=0;
        }
        memcpy(observedBroadcasterMac,item.mac,6);
        observedBroadcasterTag=msg.senderTag;
        observedBroadcasterBoot=msg.senderBoot;
        observedBroadcasterAt=nowMs;
        diagnosticLastTag=msg.senderTag;diagnosticLastBoot=msg.senderBoot;
        diagnosticLastEpoch=msg.epoch;diagnosticLastSequence=msg.sequence;
        ++diagnosticRx.authenticated;
        diagnosticRx.lastSequence=msg.sequence;
        diagnosticRx.lastEpoch=msg.epoch;
        diagnosticRx.lastSenderTag=msg.senderTag;
        diagnosticRx.lastSenderBoot=msg.senderBoot;
        // Compare consecutive local RX intervals with consecutive sender intervals.
        // Never subtract absolute clocks (unknown inter-device offset).
        // Reset across source/boot/epoch changes or sequence gaps.
        if (sameDiagnosticSource && msg.sequence == previousDiagnosticSequence + 1U &&
            item.rxUs > previousDiagnosticLocalUs && msg.monotonicUs > previousDiagnosticRemoteUs) {
            const int64_t localDelta=item.rxUs-previousDiagnosticLocalUs;
            const int64_t remoteDelta=msg.monotonicUs-previousDiagnosticRemoteUs;
            const int64_t residual=localDelta-remoteDelta;
            if (residual>=-1000000LL && residual<=1000000LL) {
                const int32_t r=static_cast<int32_t>(residual);
                const uint32_t n=++diagnosticRx.intervalSamples;
                if(n==1 || r<diagnosticRx.intervalMinUs)diagnosticRx.intervalMinUs=r;
                if(n==1 || r>diagnosticRx.intervalMaxUs)diagnosticRx.intervalMaxUs=r;
                const double delta=static_cast<double>(r)-diagnosticRx.intervalMeanUs;
                diagnosticRx.intervalMeanUs+=delta/static_cast<double>(n);
                diagnosticRx.intervalM2+=delta*(static_cast<double>(r)-diagnosticRx.intervalMeanUs);
            } else ++diagnosticRx.intervalSkipped;
        } else if (diagnosticRx.authenticated>1) ++diagnosticRx.intervalSkipped;
        observeDiagnosticPrediction(msg,item.rxUs);
        diagnosticRx.lastRxCallbackUs=item.rxUs;
        diagnosticRx.lastRemoteMonotonicUs=msg.monotonicUs;
        diagnosticRx.lastRemoteUtcUs=msg.utcUs;
    }
}
Diagnostics diagnostics(){Diagnostics out=stats;
    portENTER_CRITICAL(&txMux);
    out.txCallbackLatencyLastUs=txLatencyLastUs;
    out.txCallbackLatencyMaxUs=txLatencyMaxUs;
    out.txCallbackSamples=txLatencySamples;
    out.txCallbackFailed=txCallbackFailed;
    out.txPendingOverflow=txPendingOverflow;
    out.txCallbackUnmatched=txCallbackUnmatched;
    out.txReplyLatencySamples=txReplyLatencySamples;
    out.txReplyLatencyLastUs=txReplyLatencyLastUs;
    out.txReplyLatencyMaxUs=txReplyLatencyMaxUs;
    for(uint8_t i=0;i<Diagnostics::kProbeReplyDelayBuckets;++i)out.txReplyLatencyHist[i]=txReplyLatencyHist[i];
    portEXIT_CRITICAL(&txMux);
    out.lastSeriesEstimateAgeMs=lastEstimateMs ? uint32_t(millis()-lastEstimateMs) : 0;    out.lastAcceptedAgeMs=lastPrecisionMs ? uint32_t(millis()-lastPrecisionMs) : 0;out.precisionRecentlyAccepted=lastPrecisionMs && out.lastAcceptedAgeMs<=kPrecisionLifetimeMs;out.capturePauseRemainingMs=capturePaused() ? uint32_t(captureGuardUntilMs-millis()) : 0;out.syncPermission=syncPermissionValid();out.syncPermissionRemainingMs=out.syncPermission?uint32_t(permissionUntilMs-millis()):0;portENTER_CRITICAL(&queueMux);out.dropped=drops;portEXIT_CRITICAL(&queueMux);
    // Beta131: Production broadcaster handover state
    const uint32_t now=millis();
    out.handoverEnabled=true;
    out.broadcasterLeaseActive=broadcasterLeaseActive;
    out.broadcasterLeaseRemainingMs=broadcasterLeaseActive && now<broadcasterLeaseExpiresMs ? broadcasterLeaseExpiresMs-now : 0;
    // Beta136: actual suppression state (same predicate as the beacon slot), lease separately.
    out.coordinatorSuppressingBeacons=wasCoordinator &&
        ClusterTimeSourcePolicy::coordinatorShouldSuppressBeacon(coordinatorGrantedLeaseActive,
            coordinatorSuppressionStartMs,lastProductionBeaconFromLeasedBroadcasterMs,now);
    out.coordinatorBroadcasterLeaseActive=coordinatorGrantedLeaseActive;
    out.coordinatorBroadcasterBeaconAgeMs=lastProductionBeaconFromLeasedBroadcasterMs ?
        uint32_t(now-lastProductionBeaconFromLeasedBroadcasterMs) : 0;
    out.coordinatorBeaconsSent=coordinatorProductionBeaconsSent;
    out.coordinatorEarlyResumes=coordinatorEarlyResumes;
    out.seriesAccepted=successfulSeries;
    out.seriesRejected=failedSeries;
    out.seriesRejectInvalid=seriesRejectByReason[1];
    out.seriesRejectSpread=seriesRejectByReason[2];
    out.seriesRejectRtt=seriesRejectByReason[3];
    out.seriesRejectDeadline=seriesRejectByReason[4];
    out.seriesConsecutiveFailures=consecutiveFailures;
    out.seriesLastFailureReason=lastSeriesFailure;
    out.seriesLastRejectedAgeMs=lastRejectedSeriesMs ? uint32_t(now-lastRejectedSeriesMs) : 0;
    out.seriesLastRejectedSpreadUs=lastRejectedHasMetrics && std::isfinite(lastRejectedEstimate.spreadUs) ?
        static_cast<uint32_t>(llround(std::max(0.0,std::min(4294967295.0,lastRejectedEstimate.spreadUs)))) : 0;
    out.seriesLastCompletedAgeMs=lastCompletedSeriesMs ? uint32_t(now-lastCompletedSeriesMs) : 0;
    out.seriesNextInMs=nextSeriesMs && static_cast<int32_t>(nextSeriesMs-now)>0 ? nextSeriesMs-now : 0;
    out.coordinatorBeaconsSuppressed=coordinatorBeaconsSuppressed;
    out.activeTimeSource=activeTimeSourceTag;
    out.sourceChanges=sourceChanges;
    out.broadcasterProductionBeaconsSent=broadcasterProductionBeaconsSent;
    // Beta131-DIAGNOSIS (Seventeenth cycle): Production beacon blocking diagnostics
    out.broadcasterTimeBaseInvalid=broadcasterTimeBaseInvalid;
    out.broadcasterPrecisionFailed=broadcasterPrecisionFailed;
    out.precisionGuardWasCoordinator=precisionGuardWasCoordinator;
    out.precisionGuardEpochMismatch=precisionGuardEpochMismatch;
    out.precisionGuardTagMismatch=precisionGuardTagMismatch;
    out.precisionGuardNotValid=precisionGuardNotValid;
    out.precisionGuardOther=precisionGuardOther;
    // Beta131-DIAGNOSIS (Nineteenth cycle): Separate "other" guards
    out.precisionGuardRefLifetime=precisionGuardRefLifetime;
    out.precisionGuardEstimateAge=precisionGuardEstimateAge;
    out.precisionGuardDrift=precisionGuardDrift;
    out.precisionGuardMonoReconstruct=precisionGuardMonoReconstruct;
    out.precisionGuardUtcRange=precisionGuardUtcRange;
    return out;}
bool beaconExperimentStart(uint32_t intervalMs, bool quiet){
    if(intervalMs!=50 && intervalMs!=100 && intervalMs!=200 && intervalMs!=500)return false;
    if(!initialized || !wasCoordinator || capturePaused() || bench.running || beaconExperimentUntilMs)return false;
    // A single phase is deliberately bounded; the Node's 128-event ring
    // retains its latest events, even at the fastest interval.
    beaconExperimentIntervalMs=intervalMs;
    beaconExperimentQuiet=quiet;
    // Revoke the current owner once. No further grants during quiet mode.
    if(quiet && slotOwner>=0)endSlot(lastEpoch,lastTag,localBootNonceForControl);
    beaconExperimentLastDurationMs=15000;
    beaconExperimentSent=0;
    beaconExperimentUntilMs=millis()+beaconExperimentLastDurationMs;
    nextBroadcastMs=millis();
    return true;
}
void beaconExperimentAbort(){beaconExperimentUntilMs=0;beaconExperimentIntervalMs=0;beaconExperimentQuiet=false;nextBroadcastMs=millis()+kBroadcastEveryMs;}
BeaconExperimentState beaconExperimentSnapshot(){
    BeaconExperimentState out{};
    out.active=beaconExperimentUntilMs!=0;
    out.quiet=out.active && beaconExperimentQuiet;
    out.intervalMs=beaconExperimentIntervalMs;
    out.sent=beaconExperimentSent;
    out.remainingMs=out.active ? static_cast<uint32_t>(beaconExperimentUntilMs-millis()) : 0;
    return out;
}
bool benchmarkStart(uint32_t intervalMs,uint32_t targetBoot){
    if(!initialized||!wasCoordinator||capturePaused()||bench.running||beaconExperimentUntilMs||
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
    if(slotOwner>=0)endSlot(lastEpoch,lastTag,localBootNonceForControl);
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


// Beta131-CORRECTED: Broadcaster now runs as NODE (synchronizes WITH coordinator)
// AND sends production beacons when authorized. This fixes Problem 1 & 2.
// - Broadcaster performs Probe/Reply/Final exchanges with coordinator
// - Broadcaster builds latestQualityEstimate like any node
// - Production beacons use CLUSTER-SYNCHRONIZED time, NOT monotonic time
// - Production beacons are ONLY sent when time base is valid and fresh
void serviceDiagnosticBroadcaster(bool allowed, uint32_t epoch,
                                  const char *broadcasterDeviceId, const char *coordinatorId,
                                  uint32_t localBootNonce) {
    // Beta131-CORRECTED (Third cycle): Broadcaster does THREE things:
    // 1. Sends DIAGNOSTIC beacons (broadcasterDeviceId) so coordinator can observe and grant
    // 2. Syncs WITH coordinator (coordinatorId) as node via Probe/Reply/Final
    // 3. Sends PRODUCTION beacons when lease active (integrated in service())

    if(!allowed || !epoch || !localBootNonce ||
       !broadcasterDeviceId || !*broadcasterDeviceId ||
       !coordinatorId || !*coordinatorId ||
       WiFi.status()!=WL_CONNECTED || WiFi.getMode()!=WIFI_MODE_STA) {
        if(initialized) stop();
        return;
    }

    uint8_t channel=0; wifi_second_chan_t secondary=WIFI_SECOND_CHAN_NONE;
    if(esp_wifi_get_channel(&channel,&secondary)!=ESP_OK || !channel) {
        if(initialized) stop();
        return;
    }

    // CRITICAL: Track BROADCASTER's own identity (separate from coordinator!)
    // Problem 2 FIX: Maintain three separate identities:
    // 1. broadcasterOwnTag/Id - THIS device (for diagnostic beacons, Grant checks)
    // 2. coordinatorTag/Id - Time authority (for sync validation in precisionUtcNow)
    // 3. activeTimeSourceTag - Current active source (switches during handover)
    const uint64_t ownTag = idTag(broadcasterDeviceId);
    if(broadcasterOwnTag != ownTag) {
        broadcasterOwnTag = ownTag;
        broadcasterOwnId = String(broadcasterDeviceId);
        Serial.printf("[BROADCASTER] Own identity set: %s (tag %llx)\n",
            broadcasterDeviceId, broadcasterOwnTag);
    }

    // Send diagnostic beacons (1 Hz) so coordinator can observe us
    const uint32_t now=millis();
    if(!nextDiagnosticMs) nextDiagnosticMs=now+1000;
    if(static_cast<int32_t>(now-nextDiagnosticMs)>=0) {
        nextDiagnosticMs=now+1000;

        ClusterTimeDiagnosticBeacon::Message beacon{};
        beacon.epoch=epoch;
        beacon.sequence=++diagnosticSequence;
        if(!beacon.sequence) beacon.sequence=++diagnosticSequence;
        beacon.senderTag=broadcasterOwnTag;  // BROADCASTER's tag, not coordinator's!
        beacon.senderBoot=localBootNonce;
        beacon.monotonicUs=esp_timer_get_time();

        struct timeval tv={};
        if(gettimeofday(&tv,nullptr)==0 && tv.tv_sec>=1577836800LL)
            beacon.utcUs=static_cast<int64_t>(tv.tv_sec)*1000000LL+tv.tv_usec;

        uint8_t packet[ClusterTimeDiagnosticBeacon::kWireBytes]={};
        if(ClusterTimeDiagnosticBeacon::encode(beacon,packet,clusterEspNowDiagnosticBeaconSign)) {
            if(espNowSendTracked(kBroadcastMac,packet,sizeof(packet))==ESP_OK) {
                ++stats.sent;
                Serial.printf("[BROADCASTER] Diagnostic beacon sent (seq %u, tag %llx)\n",
                    beacon.sequence, beacon.senderTag);
            }
        }
    }

    // Now run normal node service to sync WITH coordinator
    // coordinatorId is the TIME AUTHORITY for synchronization
    service(allowed, false, epoch, coordinatorId, localBootNonce);
}

void service(bool allowed,bool coordinator,uint32_t epoch,const char* id,uint32_t localBootNonce) {
    if(!allowed||!epoch||!localBootNonce||!id||!*id||WiFi.status()!=WL_CONNECTED||
       WiFi.getMode()!=WIFI_MODE_STA){if(initialized)stop();return;}
    uint8_t channel=0;wifi_second_chan_t secondary=WIFI_SECOND_CHAN_NONE;
    if(esp_wifi_get_channel(&channel,&secondary)!=ESP_OK||!channel){if(initialized)stop();return;}
    uint64_t tag=idTag(id);
    if(initialized&&(diagnosticMode||boundChannel!=channel||lastEpoch!=epoch||lastTag!=tag||wasCoordinator!=coordinator))stop();
    const bool freshStart = !initialized;
    if(freshStart){
        if(!start(channel))return;
        if(!coordinator)loadDriftPrior(id);
    }
    wasCoordinator=coordinator;lastEpoch=epoch;lastTag=tag;localBootNonceForControl=localBootNonce;

    // Beta131-FIX (Eighth cycle, Problem 1): Coordinator startup guard
    // CRITICAL: After reboot, suppress production beacons until old leases expire
    // Guard duration: LEASE_DURATION (3000) + SAFETY_MARGIN (1000) + RF_DELAY (50) + MARGIN (1000) = 5050ms
    // Conservative: 6000ms to handle clock drift and delayed packets
    if(freshStart && wasCoordinator) {
        constexpr uint32_t kStartupGuardMs = 6000U;
        coordinatorStartupGuardUntilMs = millis() + kStartupGuardMs;
        Serial.printf("[COORDINATOR] Startup guard active for %u ms (until T=%u)\n",
            kStartupGuardMs, coordinatorStartupGuardUntilMs);
    }
    if (capturePaused()) {
        if(beaconExperimentUntilMs)beaconExperimentAbort();
        return; // Capture always wins.
    }
    if (captureGuardUntilMs) {
        captureGuardUntilMs = 0;
        clearQueue(); // Discard frames received while processing was paused.
        nextBroadcastMs = millis() + kBroadcastEveryMs;
        nextSeriesMs = millis() + 1000UL;
    }
    serviceDiagnosticReceive();
    serviceGrantDiagnostic(localBootNonce);
    Queued item;unsigned budget=0;
    while(budget++<5&&pop(item)) {
        // RX was timestamped at callback entry. Record service/queue latency.
        const int64_t serviceRxUs=esp_timer_get_time();
        if(serviceRxUs>=item.rxUs){
            const uint64_t delta=static_cast<uint64_t>(serviceRxUs-item.rxUs);
            const uint32_t waitUs=delta>UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(delta);
            stats.rxQueueDelayLastUs=waitUs;
            if(waitUs>stats.rxQueueDelayMaxUs)stats.rxQueueDelayMaxUs=waitUs;
        }
        ++stats.received;
        EspNowTimePackets::Frame f;
        // Beta131-FIX (Fifteenth cycle): REMOVE premature epoch validation
        // CRITICAL: This rejected ALL packets (including Replies/Finals from coordinator)
        // that didn't match the local device epoch - breaking exchanges completely!
        // Epoch validation is packet-type-specific and happens later:
        // - Replies: Line ~2234 (bootstrap-safe, validates against activeTimeSourceEpoch)
        // - Finals: Line ~2262 (bootstrap-safe, validates against activeTimeSourceEpoch)
        // - Beacons: No local epoch validation needed (sets activeTimeSourceEpoch)
        if(!EspNowTimePackets::decode(item.bytes,sizeof(item.bytes),f,clusterEspNowTimeVerify,nullptr)){
            ++stats.rejected;
            continue;
        }

        // Beta131-CORRECTED: Source switching moved AFTER full validation (Problem 5)
        // DO NOT switch source here - must validate packet type and role first
        // Source switching happens ONLY for validated Beacon packets below

        if(coordinator) {
            if((f.type==EspNowTimePackets::Type::BenchUp || f.type==EspNowTimePackets::Type::BenchAck) &&
               acceptPeer(item.mac,f)) {
                ++stats.authenticated;
                if(bench.running && bench.peerIndex<bench.peerCount &&
                   f.senderBoot==bench.peers[bench.peerIndex].boot &&
                   memcmp(item.mac,benchMac[bench.peerIndex],6)==0) {
                    if(f.type==EspNowTimePackets::Type::BenchAck && f.nonce==UINT32_MAX &&
                       f.timeUs==benchControlValue(bench.phase)) {
                        if(!benchCtrlAck){benchCtrlAck=true;benchPhaseStart=millis();}
                        continue;
                    }
                    if(f.nonce>=100 || !benchCtrlAck)continue;
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
                continue;
            }
            // Beta135-FIX: The coordinator must SEE the leased broadcaster's production beacons,
            // otherwise lastProductionBeaconFromLeasedBroadcasterMs never updates, suppression never
            // engages and the coordinator keeps beaconing every kBroadcastEveryMs. Nodes then switch
            // broadcaster->coordinator->broadcaster on every coordinator beacon (2 source_changes/10 s).
            // Previously all beacons were rejected by the type filter below, leaving the node-path
            // tracking unreachable. Liveness only: the beacon is never used as a time reference here.
            if(f.type==EspNowTimePackets::Type::Beacon) {
                if(ClusterTimeSourcePolicy::isLeasedBroadcasterBeacon(coordinatorGrantedLeaseActive,
                       coordinatorLeaseBroadcasterTag,coordinatorLeaseBroadcasterBoot,
                       f.coordinatorTag,f.senderBoot) &&
                   acceptPeer(item.mac,f)) {
                    ++stats.authenticated;
                    lastProductionBeaconFromLeasedBroadcasterMs=millis();
                } else ++stats.rejected;
                continue;
            }
            if((f.type!=EspNowTimePackets::Type::Probe &&
                f.type!=EspNowTimePackets::Type::NodeSyncReport &&
                f.type!=EspNowTimePackets::Type::NodeDriftReport)||!acceptPeer(item.mac,f)){
                ++stats.rejected;continue;
            }
            ++stats.authenticated;
            // Beta131-FIX (Sixteenth cycle): Coordinator must answer broadcaster's own Probes
            // CRITICAL: Broadcaster needs time base from coordinator BEFORE it can handle node Probes
            // Only skip Probes NOT addressed to coordinator (i.e., addressed to broadcaster)
            // PREVIOUS BUG: Skipped ALL Probes when lease active, including broadcaster's own
            // This created DEADLOCK: Broadcaster couldn't get time base, couldn't activate, lease expired
            if(coordinatorGrantedLeaseActive && f.type==EspNowTimePackets::Type::Probe && f.coordinatorTag != tag) {
                continue;  // Let broadcaster handle Probes addressed to broadcaster (from nodes)
            }
            // Fall through: Handle Probes addressed to coordinator (from broadcaster itself during bootstrap)
            if(f.type==EspNowTimePackets::Type::NodeSyncReport ||
               f.type==EspNowTimePackets::Type::NodeDriftReport){
                noteSlotPeer(item.mac,static_cast<uint32_t>(f.senderBoot));
                acceptNodeReport(f);continue;
            }
            // Beta131-FIX (Seventh cycle, Problem 1): Verify Probe is addressed to THIS coordinator
            // CRITICAL: Only respond to Probes explicitly addressed to coordinator's sender identity
            // Prevents responding to Probes meant for broadcaster or other time sources
            if(f.coordinatorTag != tag) {
                ++stats.rejected;
                continue;  // Probe addressed to different sender (e.g., broadcaster)
            }

            // Beta131-FIX (Sixteenth cycle): Check if Probe is from observed broadcaster
            // CRITICAL: Broadcaster needs time base from coordinator (not a slot owner!)
            // Broadcaster is special role, not regular node in slot system
            const bool isObservedBroadcaster = (observedBroadcasterAt != 0 &&
                                                memcmp(item.mac, observedBroadcasterMac, 6) == 0 &&
                                                f.senderBoot == observedBroadcasterBoot);

            // Only slot owner, benchmark target, or observed broadcaster may receive timestamp replies
            // Authentication and replay validation above still apply for every received packet
            if(!slotIsOwner(item.mac,static_cast<uint32_t>(f.senderBoot)) &&
               !(bench.running && bench.phase==3 && bench.peerIndex<bench.peerCount &&
                 f.senderBoot==bench.peers[bench.peerIndex].boot &&
                 memcmp(item.mac,benchMac[bench.peerIndex],6)==0) &&
               !isObservedBroadcaster) continue;
            // Four-timestamp exchange: t1=Node Probe TX, t2=Coordinator Probe RX,
            // t3=Coordinator REPLY TX call, t4=Node REPLY RX callback.
            // Final delivers t3 metadata only; Final RX is NEVER t4.
            // Software call timestamps are not RF/PHY TX timestamps.
            if(!ensurePeer(item.mac)){++stats.rejected;continue;}
            EspNowTimePackets::Frame r;
            r.type=EspNowTimePackets::Type::Reply;r.epoch=epoch;r.coordinatorTag=tag;
            r.senderBoot=localBootNonce;r.nonce=f.nonce;r.timeUs=item.rxUs;
            int64_t replyDispatchBeforeUs=0;
            // t3 denotes the Reply send path, NOT the later Final send call.
            // Reply.timeUs continues to carry t2 unchanged.
            if(!sendFrame(r,item.mac,&replyDispatchBeforeUs,true))continue;
            recordProbeReplyDelay(item.rxUs,replyDispatchBeforeUs);
            const int64_t finalDispatchUs=esp_timer_get_time();
            if(finalDispatchUs>=replyDispatchBeforeUs){
                const uint64_t delta=static_cast<uint64_t>(finalDispatchUs-replyDispatchBeforeUs);
                const uint32_t gapUs=delta>UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(delta);
                stats.replyFinalDispatchGapLastUs=gapUs;
                if(gapUs>stats.replyFinalDispatchGapMaxUs)stats.replyFinalDispatchGapMaxUs=gapUs;
            }
            r.type=EspNowTimePackets::Type::Final;r.timeUs=replyDispatchBeforeUs;
            if(sendFrame(r,item.mac))++stats.probesAnswered;
            continue;
        }

        // Beta131-CORRECTED: Broadcaster Probe handling when lease active (Problem 4 FIX)
        // Broadcaster must respond to Probes from nodes for full 4-timestamp synchronization
        // Beta133-DIAGNOSIS: Detailed broadcaster probe handling tracking
        static uint32_t diagProbesTotal = 0;
        static uint32_t diagLeaseInactive = 0;
        static uint32_t diagTimeInvalid = 0;
        static uint32_t diagAuthFailed = 0;
        static uint32_t diagTagMismatch = 0;
        static uint32_t diagPeerFailed = 0;
        static uint32_t diagReplySent = 0;
        static uint32_t diagReplyFailed = 0;
        static uint32_t diagLastLogMs = 0;

        // Diagnosis is passive: it only counts. Probes failing the gate below fall
        // through to the type filter exactly as before (++stats.rejected there).
        const bool diagProbe = f.type==EspNowTimePackets::Type::Probe && broadcasterOwnTag;
        if(diagProbe) {
            ++diagProbesTotal;
            if(!broadcasterLeaseActive) ++diagLeaseInactive;
            else if(!broadcasterTimeBaseValid) ++diagTimeInvalid;
        }

        bool broadcasterProbeAccepted=false;
        if(broadcasterLeaseActive && broadcasterTimeBaseValid && f.type==EspNowTimePackets::Type::Probe) {
            broadcasterProbeAccepted=acceptPeer(item.mac,f);
            if(!broadcasterProbeAccepted && diagProbe) ++diagAuthFailed;
        }
        if(broadcasterProbeAccepted) {
            ++stats.authenticated;

            // Beta131-FIX (Seventh cycle, Problem 1): Verify Probe is addressed to THIS broadcaster
            // CRITICAL: Only respond if Probe explicitly addresses broadcaster's sender identity
            // During lease, nodes should probe broadcasterOwnTag, not coordinator tag
            if(f.coordinatorTag != broadcasterOwnTag) {
                ++stats.rejected;
                ++diagTagMismatch;
                continue;  // Probe addressed to different sender (e.g., coordinator or other broadcaster)
            }

            // Broadcaster handles Probes when authorized and time base is valid
            // This enables nodes to perform full Probe/Reply/Final exchanges with broadcaster
            // Four-timestamp exchange: t1=Node Probe TX, t2=Broadcaster Probe RX,
            // t3=Broadcaster REPLY TX, t4=Node REPLY RX
            if(!ensurePeer(item.mac)){
                ++stats.rejected;
                ++diagPeerFailed;
                continue;
            }
            EspNowTimePackets::Frame r;
            r.type=EspNowTimePackets::Type::Reply;r.epoch=epoch;
            // Beta131-FIX (Fifth cycle, Problem 2): Broadcaster uses OWN tag, not coordinator tag
            // CRITICAL: Reply/Final must match production beacon sender identity (broadcasterOwnTag)
            // Nodes calculate offset to BROADCASTER's clock, not coordinator's clock
            r.coordinatorTag=broadcasterOwnTag;  // Broadcaster's identity, not coordinator's
            r.senderBoot=localBootNonce;r.nonce=f.nonce;r.timeUs=item.rxUs;
            int64_t replyDispatchBeforeUs=0;
            // t3 denotes the Reply send path, broadcaster uses own monotonic clock
            if(!sendFrame(r,item.mac,&replyDispatchBeforeUs,true)){
                ++diagReplyFailed;
                continue;
            }
            ++diagReplySent;
            recordProbeReplyDelay(item.rxUs,replyDispatchBeforeUs);
            const int64_t finalDispatchUs=esp_timer_get_time();
            if(finalDispatchUs>=replyDispatchBeforeUs){
                const uint64_t delta=static_cast<uint64_t>(finalDispatchUs-replyDispatchBeforeUs);
                const uint32_t gapUs=delta>UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(delta);
                stats.replyFinalDispatchGapLastUs=gapUs;
                if(gapUs>stats.replyFinalDispatchGapMaxUs)stats.replyFinalDispatchGapMaxUs=gapUs;
            }
            r.type=EspNowTimePackets::Type::Final;r.timeUs=replyDispatchBeforeUs;
            if(sendFrame(r,item.mac))++stats.probesAnswered;
            continue;
        }

        // Beta133-DIAGNOSIS: Output broadcaster probe handling stats every 5s
        if(broadcasterOwnTag && (millis() - diagLastLogMs) >= 5000) {
            Serial.printf("[BROADCASTER-PROBE-DIAG] total=%u lease_off=%u time_invalid=%u auth_fail=%u tag_mismatch=%u peer_fail=%u reply_sent=%u reply_fail=%u\n",
                diagProbesTotal, diagLeaseInactive, diagTimeInvalid, diagAuthFailed,
                diagTagMismatch, diagPeerFailed, diagReplySent, diagReplyFailed);
            diagLastLogMs = millis();
            // Reset counters for next interval
            diagProbesTotal = diagLeaseInactive = diagTimeInvalid = diagAuthFailed =
                diagTagMismatch = diagPeerFailed = diagReplySent = diagReplyFailed = 0;
        }

        // Beta131-CORRECTED: Per-sender replay protection (Problem 5 FIX)
        // SECURE: Track replay windows per sender identity (MAC+boot+epoch), not globally
        // SECURE: Beacons MUST have replay protection (prevents replay-based source switch attacks)
        // SECURE: Each time source (coordinator OR broadcaster) has separate replay window
        if(f.type!=EspNowTimePackets::Type::SyncControl&&
           f.type!=EspNowTimePackets::Type::BenchControl&&
           f.type!=EspNowTimePackets::Type::BenchDown&&
           f.type!=EspNowTimePackets::Type::BenchAck&&
           f.type!=EspNowTimePackets::Type::Reply&&
           f.type!=EspNowTimePackets::Type::Final&&
           f.type!=EspNowTimePackets::Type::Beacon){++stats.rejected;continue;}
        // Use acceptPeer() for per-sender replay protection (MAC+boot+epoch)
        if(!acceptPeer(item.mac,f)){++stats.replay;continue;}
        ++stats.authenticated;
        if(f.type==EspNowTimePackets::Type::BenchControl) {
            // Target boot prevents other nodes from participating. The coordinator
            // epoch and replay window have already been authenticated above.
            if(f.nonce!=localBootNonceForControl)continue;
            const uint32_t phase=static_cast<uint16_t>(f.timeUs);
            const uint32_t gap=(static_cast<uint32_t>(f.timeUs)>>16)&0xffffu;
            if(gap!=5 && gap!=10 && gap!=50)continue;
            if(phase==0){benchClearNode();continue;}
            const uint32_t incomingRun=static_cast<uint32_t>(static_cast<uint64_t>(f.timeUs)>>32);
            if((phase==benchNodePhase && incomingRun==benchNodeRun) ||
               (phase==1) ||
               (phase==2 && benchNodePhase==1 && incomingRun==benchNodeRun) ||
               (phase==3 && benchNodePhase==2 && incomingRun==benchNodeRun)) {
                // Acknowledge before data traffic; duplicates do not restart a phase.
                benchSend(EspNowTimePackets::Type::BenchAck,UINT32_MAX,f.timeUs,kBroadcastMac);
                if(phase==benchNodePhase && incomingRun==benchNodeRun)continue;
            } else continue;
            if(phase==1){
                benchNodeRun=static_cast<uint32_t>(static_cast<uint64_t>(f.timeUs)>>32);
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
            } else if(phase==3 && benchNodePhase==2 &&
                      benchNodeRun==static_cast<uint32_t>(static_cast<uint64_t>(f.timeUs)>>32)) {
                benchNodePhase=3;benchNodeDeadlineMs=millis()+benchNodeGapMs*100+4500;
                benchTimeProbes=0;benchTimeNextMs=millis();pending={};
            } else if(phase==2 && benchNodePhase==1 &&
                      benchNodeRun==static_cast<uint32_t>(static_cast<uint64_t>(f.timeUs)>>32)) {
                benchNodePhase=2;benchNodeDeadlineMs=millis()+benchNodeGapMs*100+4500;
                benchNodeNextMs=millis();benchNodeNextSeq=0;
            }
            continue;
        }
        if(f.type==EspNowTimePackets::Type::BenchDown){
            if(benchNodePhase==1 && f.timeUs==localBootNonceForControl && f.nonce<100){
                if(!benchNodeDownSeen[f.nonce]){benchNodeDownSeen[f.nonce]=1;++benchNodeDownReceived;}
                benchSend(EspNowTimePackets::Type::BenchAck,f.nonce,benchNodeRun,kBroadcastMac);
            }
            continue;
        }
        if(f.type==EspNowTimePackets::Type::BenchAck){
            if(benchNodePhase==2 && f.nonce<100 && f.timeUs==benchNodeRun && !benchNodeUpAckSeen[f.nonce]){
                benchNodeUpAckSeen[f.nonce]=1;++benchNodeUpAck;
                benchNodeAckRttUs[f.nonce]=static_cast<uint32_t>(esp_timer_get_time())-benchNodeTxUs[f.nonce];
            }
            continue;
        }
        if(f.type==EspNowTimePackets::Type::SyncControl) {
            // timeUs is a bounded lease length. Stop is immediate on authenticated receipt.
            if(f.nonce!=localBootNonceForControl) continue;
            if(f.timeUs==0) {
                permissionUntilMs=0;pending={};seriesCount=0;seriesTarget=kInitialSamples;seriesStartedMs=0;
                nextSeriesMs=millis()+1000UL;
            } else if(f.timeUs>0 && f.timeUs<=static_cast<int64_t>(kGrantLeaseMs)) {
                permissionUntilMs=millis()+static_cast<uint32_t>(f.timeUs);
            } else { ++stats.rejected; }
            continue;
        }
        stats.lastLocalRxUs=item.rxUs;stats.lastRemoteStampUs=f.timeUs;
        if(f.type==EspNowTimePackets::Type::Beacon && f.timeUs>=1577836800000000LL &&
           f.timeUs<4102444800000000LL) {
            // Beta131-FIX (Twelfth cycle): Ignore own beacons (broadcaster receiving its own production beacons)
            // CRITICAL: Broadcaster must NOT update activeTimeSourceTag to its own tag
            // Otherwise: broadcaster sends Probes to itself instead of coordinator → exchanges fail
            if(f.senderBoot == localBootNonce) {
                ++stats.rejected;
                continue;  // Ignore own beacon (self-reception)
            }
            // Beta135: Leased-broadcaster beacon tracking for condition-based suppression lives in the
            // coordinator RX block (this node path is never reached with coordinator=true).
            // For beacons only: timeUs=UTC, nonce=lower 32 bits of the
            // coordinator's monotonic timestamp sampled alongside UTC.
            referenceUtcUs=f.timeUs;
            referenceMonoLow=f.nonce;
            referenceReceivedNodeMono=item.rxUs;
            // Passive only; item.rxUs was sampled in the existing RX callback.
            // Predict before touching the ring: overwriting the oldest slot
            // previously corrupted the model once all 12 slots were full.
            BeaconObservation nextObs{};
            predictBeacon(f,item.rxUs,nextObs);
            BeaconObservation &obs=beaconObservations[beaconObsWrite];
            obs=nextObs;
            obs.sequence=f.sequence;obs.epoch=f.epoch;
            obs.coordinatorMonoLow=f.nonce;obs.coordinatorBoot=f.senderBoot;
            obs.coordinatorUtcUs=f.timeUs;obs.nodeRxCallbackUs=item.rxUs;
            beaconObsWrite=(beaconObsWrite+1)%kBeaconObservations;
            if(beaconObsCount<kBeaconObservations)++beaconObsCount;
            accumulateBeaconStats(obs);
            appendSharedEvent(obs);

            // Beta131-CORRECTED: Safe source switching AFTER full beacon validation (Problem 5)
            // SECURE: Only switch source after complete validation of permissible production time packet
            // SECURE: Track boot ID and epoch to prevent mixing timestamps from different sources
            // SECURE: Protect replay protection and capture clock from uncontrolled time jumps
            if(ClusterTimeSourcePolicy::isSourceChange(activeTimeSourceTag,activeTimeSourceBoot,
                   activeTimeSourceEpoch,f.coordinatorTag,f.senderBoot,f.epoch)) {
                // SECURE: Source change detected - reset timing state (but preserve replay protection)
                resetTiming();  // Clears pending samples, NOT replay windows (per-sender protection persists)
                latestQualityEstimate = {};  // Clears precision estimates
                lastPrecisionMs = 0;  // Invalidates precision time
                ++sourceChanges;
                Serial.printf("[NODE] Time source SWITCHED: tag %llx->%llx boot %llx->%llx epoch %u->%u\n",
                    activeTimeSourceTag, f.coordinatorTag,
                    activeTimeSourceBoot, f.senderBoot,
                    activeTimeSourceEpoch, f.epoch);
                // SECURE: Protect capture clock - prevent time jumps during planned GPIO actions
                // Any scheduled captures should check activeTimeSourceTag and abort on mismatch
            }
            // Update active source ONLY after successful beacon validation
            // Beta131-FIX (Seventh cycle, Problem 2): Track sender MAC from validated beacon
            // Beta131-DIAGNOSIS (Fourteenth cycle): Track beacon updates to detect race with Grant
            const uint64_t previousTag = activeTimeSourceTag;
            const uint32_t previousEpoch = activeTimeSourceEpoch;
            activeTimeSourceTag = f.coordinatorTag;
            activeTimeSourceBoot = f.senderBoot;
            activeTimeSourceEpoch = f.epoch;
            memcpy(activeTimeSourceMac, item.mac, 6);  // Store MAC for Reply/Final validation
            if(!previousTag) {
                Serial.printf("[BEACON-RX] FIRST activeTimeSource set: tag=%llx epoch=%u\n",
                    f.coordinatorTag, f.epoch);
            } else if(previousEpoch != f.epoch) {
                Serial.printf("[BEACON-RX] Epoch UPDATE: %u -> %u (delta=%d)\n",
                    previousEpoch, f.epoch, static_cast<int32_t>(f.epoch - previousEpoch));
            }
        }
        // Beta131-FIX (Sixth cycle, Problem 1): Validate Reply/Final sender identity
        // CRITICAL: Only accept Reply/Final from expected time source (prevents mixed timestamps)
        if(f.type==EspNowTimePackets::Type::Reply&&pending.active&&f.nonce==pending.nonce) {
            // SECURE: Verify sender identity matches expected time source
            // Beta131-FIX (Seventh cycle, Problem 2): Also validate sender MAC
            // Beta131-FIX (Tenth cycle, Problem 2): Bootstrap-safe boot validation
            // Beta131-FIX (Eleventh cycle): Bootstrap-safe epoch validation
            bool macMatches = true;
            bool macIsExpected = false;
            for(int i=0; i<6; ++i) {
                if(pending.expectedMac[i] != 0) macIsExpected = true;
                if(pending.expectedMac[i] != item.mac[i]) macMatches = false;
            }
            // BOOTSTRAP: If expectedBoot==0 (no beacon received yet), accept any boot from expected tag
            // NORMAL: If expectedBoot!=0, require exact boot match to prevent time source confusion
            const bool bootMatches = (pending.expectedBoot == 0) ? true : (f.senderBoot == pending.expectedBoot);
            // BOOTSTRAP: If expectedEpoch==0 (no beacon received yet), accept any epoch from expected tag
            // NORMAL: If expectedEpoch!=0, require exact epoch match to prevent time source confusion
            const bool epochMatches = (pending.expectedEpoch == 0) ? true : (f.epoch == pending.expectedEpoch);
            if(f.coordinatorTag==pending.expectedTag &&
               bootMatches &&
               epochMatches &&
               (!macIsExpected || macMatches)) {  // MAC must match if expected
                pending.t2=f.timeUs;
                pending.t4ReplyRx=item.rxUs; // t4: the Reply receive callback, not Final
            } else {
                // REJECT: Reply from wrong sender (different time source, boot/epoch, or MAC)
                // This prevents mixing timestamps from coordinator and broadcaster
                // Beta131-DIAGNOSIS (Fourteenth cycle): Log rejection reason
                Serial.printf("[REPLY-RX] REJECTED - Identity mismatch:\n");
                Serial.printf("  coordinatorTag: %llx (expected %llx) %s\n",
                    f.coordinatorTag, pending.expectedTag,
                    (f.coordinatorTag==pending.expectedTag) ? "✓" : "✗");
                Serial.printf("  senderBoot:     %llx (expected %llx) %s\n",
                    f.senderBoot, pending.expectedBoot,
                    bootMatches ? "✓" : "✗");
                Serial.printf("  epoch:          %u (expected %u) %s\n",
                    f.epoch, pending.expectedEpoch,
                    epochMatches ? "✓" : "✗");
                pending={};  // Abort transaction
                ++stats.rejected;
            }
        }
        if(f.type==EspNowTimePackets::Type::Final&&pending.active&&f.nonce==pending.nonce) {
            // SECURE: Verify sender identity matches expected time source
            // Beta131-FIX (Seventh cycle, Problem 2): Also validate sender MAC
            // Beta131-FIX (Tenth cycle, Problem 2): Bootstrap-safe boot validation
            // Beta131-FIX (Eleventh cycle): Bootstrap-safe epoch validation
            bool macMatches = true;
            bool macIsExpected = false;
            for(int i=0; i<6; ++i) {
                if(pending.expectedMac[i] != 0) macIsExpected = true;
                if(pending.expectedMac[i] != item.mac[i]) macMatches = false;
            }
            // BOOTSTRAP: If expectedBoot==0 (no beacon received yet), accept any boot from expected tag
            // NORMAL: If expectedBoot!=0, require exact boot match to prevent time source confusion
            const bool bootMatches = (pending.expectedBoot == 0) ? true : (f.senderBoot == pending.expectedBoot);
            // BOOTSTRAP: If expectedEpoch==0 (no beacon received yet), accept any epoch from expected tag
            // NORMAL: If expectedEpoch!=0, require exact epoch match to prevent time source confusion
            const bool epochMatches = (pending.expectedEpoch == 0) ? true : (f.epoch == pending.expectedEpoch);
            if(f.coordinatorTag==pending.expectedTag &&
               bootMatches &&
               epochMatches &&
               (!macIsExpected || macMatches)) {  // MAC must match if expected
                // V2: Apply software timestamp correction to t3 using delay from Coordinator's Reply
                int64_t t3_corrected = f.timeUs + static_cast<int64_t>(f.preSendDelayUs);
                finishSample(t3_corrected);
            } else {
                // REJECT: Final from wrong sender (different MAC, tag, boot, or epoch)
                pending={};  // Abort transaction
                ++stats.rejected;
            }
        }
    }
    uint32_t now=millis();

    // Beta131-CORRECTED: Broadcaster production service FIRST (Problem 1 FIX)
    // Must run BEFORE any early returns to ensure lease checks and beacon sending
    // This section executes for broadcaster role regardless of node state

    // Update time base validity (needs fresh sync from node role)
    broadcasterTimeBaseValid = latestQualityEstimate.valid && lastPrecisionMs &&
        (millis() - lastPrecisionMs) <= kPrecisionLifetimeMs;

    // Check broadcaster lease activation
    if(!broadcasterLeaseActive && broadcasterLeaseActivationMs && now>=broadcasterLeaseActivationMs &&
       broadcasterActivationTransaction) {
        if(broadcasterTimeBaseValid) {
            broadcasterLeaseActive=true;
            Serial.printf("[BROADCASTER] Production ACTIVATED (txn %u, time valid)\n",
                broadcasterActivationTransaction);
        } else {
            Serial.println("[BROADCASTER] Production BLOCKED - time base invalid");
            broadcasterLeaseActivationMs=0;
            broadcasterActivationTransaction=0;
        }
    }

    // Check lease expiry
    if(broadcasterLeaseActive && now>=broadcasterLeaseExpiresMs) {
        broadcasterLeaseActive=false;
        broadcasterLeaseActivationMs=0;
        broadcasterActivationTransaction=0;
        Serial.println("[BROADCASTER] Lease EXPIRED");
    }

    // Send production beacons when lease active AND time valid
    if(broadcasterLeaseActive && broadcasterTimeBaseValid) {
        if(!nextBroadcastMs) nextBroadcastMs=now+100;
        if(static_cast<int32_t>(now-nextBroadcastMs)>=0) {
            nextBroadcastMs=now+100;

            int64_t clusterUtcUs=0;
            uint32_t clusterAgeMs=0;
            double clusterRttUs=0;
            if(precisionUtcNow(epoch, id, clusterUtcUs, clusterAgeMs, clusterRttUs)) {
                // Beta131-CORRECTED (Third cycle): Problem 4 FIX - Consistent timestamp base
                // CRITICAL: Use broadcaster's OWN monotonic clock for ALL timestamps
                // - Production Beacons: nonce = broadcaster's monotonic time
                // - Probe/Reply/Final: timeUs = broadcaster's monotonic time
                // Nodes calculate offset to BROADCASTER's clock (not coordinator's)
                // This ensures mathematical consistency across all broadcaster messages
                // Beta138: TX-latency registration happens before the stamp, so the
                // stamp -> esp_now_send() path of the beacon is unchanged.
                const int64_t prodEnqueueUs = esp_timer_get_time();
                const size_t prodSlot = txPendingRegister(kBroadcastMac, kTxClassOther, prodEnqueueUs);
                const int64_t broadcasterMonoUs = esp_timer_get_time();
                const uint32_t broadcasterMonoLow = static_cast<uint32_t>(broadcasterMonoUs);

                EspNowTimePackets::Frame prodBeacon{};
                prodBeacon.type=EspNowTimePackets::Type::Beacon;
                prodBeacon.epoch=epoch;
                // Same sequence space as every other frame from this MAC/boot/epoch:
                // receivers keep ONE replay window per sender MAC, and txSequence is
                // never reset by stop(), so a radio restart cannot replay low sequences.
                prodBeacon.sequence=nextSequence();
                // Beta131-FIX (Fourth cycle, Problem 1): Use broadcaster's OWN tag as sender identity
                // Distinguishes: coordinator=time authority, broadcaster=physical sender+authorized source
                // Nodes track broadcaster as activeTimeSourceTag (not coordinator) when lease active
                prodBeacon.coordinatorTag=broadcasterOwnTag;  // Broadcaster's identity (authorized sender)
                prodBeacon.senderBoot=localBootNonce;  // Broadcaster's boot
                prodBeacon.timeUs=clusterUtcUs;  // Cluster-synchronized UTC (from coordinator)
                prodBeacon.nonce=broadcasterMonoLow;  // Broadcaster's own monotonic time

                uint8_t packet[EspNowTimePackets::kPacketSize];
                if(EspNowTimePackets::encode(prodBeacon,packet,clusterEspNowTimeSign,nullptr) &&
                   esp_now_send(kBroadcastMac,packet,sizeof(packet))==ESP_OK) {
                    ++stats.sent;
                    ++broadcasterProductionBeaconsSent;
                } else {
                    txPendingCancel(prodSlot, prodEnqueueUs);
                }
            } else {
                // Beta131-DIAGNOSIS (Seventeenth cycle): Track precisionUtcNow() failure
                ++broadcasterPrecisionFailed;
                // Beta131-FIX (Eighteenth cycle): Clear activation state on revocation
                // CRITICAL: Prevents re-activation loop with stale transaction
                // Without this, broadcasterLeaseActivationMs/Transaction remain set
                // and re-activate on next tick when broadcasterTimeBaseValid flips to true
                broadcasterLeaseActive=false;
                broadcasterLeaseActivationMs=0;
                broadcasterActivationTransaction=0;
                broadcasterTimeBaseValid=false;
                Serial.println("[BROADCASTER] Time base LOST - lease revoked");
            }
        }
    } else if(broadcasterLeaseActive && !broadcasterTimeBaseValid) {
        // Beta131-DIAGNOSIS (Seventeenth cycle): Track time base invalid (but lease active)
        ++broadcasterTimeBaseInvalid;
    }

    // Now continue with normal service flow
    if(beaconExperimentUntilMs && static_cast<int32_t>(now-beaconExperimentUntilMs)>=0)beaconExperimentAbort();
    if(coordinator && bench.running){benchTick(now);return;}
    if(!coordinator && benchNodePhase){benchNodeTick(now);return;}
    if(coordinator){
        // Beta131-FIX (Tenth cycle): Check if granted broadcaster lease has expired
        if(coordinatorGrantedLeaseActive && now>=coordinatorGrantedLeaseExpiresMs) {
            coordinatorGrantedLeaseActive=false;
            coordinatorSuppressionStartMs=0;  // Clear suppression timing
            Serial.println("[COORDINATOR] Broadcaster lease EXPIRED - resuming own beacons");
        }

        if(!(beaconExperimentUntilMs && beaconExperimentQuiet))serviceSlots(now,epoch,tag,localBootNonce);
        // Beta136: resume immediately (normal send path below) once suppression stops holding,
        // instead of leaving nodes without any beacon source for up to kBroadcastEveryMs.
        if(ClusterTimeSourcePolicy::coordinatorShouldResumeEarly(coordinatorLastSlotSuppressed,
               ClusterTimeSourcePolicy::coordinatorShouldSuppressBeacon(coordinatorGrantedLeaseActive,
                   coordinatorSuppressionStartMs,lastProductionBeaconFromLeasedBroadcasterMs,now))) {
            coordinatorLastSlotSuppressed=false;
            ++coordinatorEarlyResumes;
            nextBroadcastMs=now;
        }
        if(static_cast<int32_t>(now-nextBroadcastMs)>=0){
            nextBroadcastMs=now+(beaconExperimentUntilMs?beaconExperimentIntervalMs:kBroadcastEveryMs);

            // Beta131-FIX (Eighth cycle, Problem 1): Startup guard prevents overlap after reboot
            // CRITICAL: Suppress beacons until any old leases from previous run have expired
            if(coordinatorStartupGuardUntilMs && now < coordinatorStartupGuardUntilMs) {
                ++coordinatorBeaconsSuppressed;
                return;  // Startup guard active
            }
            // Clear guard once expired
            if(coordinatorStartupGuardUntilMs && now >= coordinatorStartupGuardUntilMs) {
                coordinatorStartupGuardUntilMs = 0;
                Serial.println("[COORDINATOR] Startup guard EXPIRED - resuming production beacons");
            }

            // Beta132-V2-FIX: Condition-based suppression ONLY when broadcaster confirmed active
            // CRITICAL: Prevents bootstrap deadlock AND prevents double production
            // TIMING: Grace period (10s) allows broadcaster to complete 16-sample series
            // CONDITION: Only suppress when broadcaster sends AUTHENTICATED production beacons
            // SAFETY: Coordinator automatically resumes if broadcaster stops (no timer-based trust)
            if(coordinatorGrantedLeaseActive && coordinatorSuppressionStartMs && now >= coordinatorSuppressionStartMs) {
                // Check if broadcaster is CONFIRMED active (recent production beacon within 2s)
                const bool broadcasterConfirmedActive = ClusterTimeSourcePolicy::coordinatorShouldSuppressBeacon(
                    coordinatorGrantedLeaseActive, coordinatorSuppressionStartMs,
                    lastProductionBeaconFromLeasedBroadcasterMs, now);

                if(broadcasterConfirmedActive) {
                    // SAFE: Broadcaster confirmed sending production beacons, suppress coordinator
                    ++coordinatorBeaconsSuppressed;
                    coordinatorLastSlotSuppressed=true;
                    return;  // Broadcaster active, suppressing coordinator beacons
                } else {
                    // SAFE: Broadcaster not confirmed - keep sending coordinator beacons
                    // This prevents deadlock: coordinator continues until broadcaster ready
                    // Log throttled (every 5s) to avoid spam
                    static uint32_t lastLogMs = 0;
                    if((now - lastLogMs) >= 5000) {
                        Serial.printf("[COORDINATOR] Broadcaster not confirmed active - continuing beacons (grace elapsed: %u ms)\n",
                            now - coordinatorSuppressionStartMs);
                        lastLogMs = now;
                    }
                    // Fall through to send beacon
                }
            }

            coordinatorLastSlotSuppressed=false;
            EspNowTimePackets::Frame b;
            b.type=EspNowTimePackets::Type::Beacon;b.epoch=epoch;
            b.coordinatorTag=tag;b.senderBoot=localBootNonce;
            // Software sample pair. This is NOT an RF airtime timestamp.
            const int64_t monoBefore=esp_timer_get_time();
            struct timeval tv={};
            const bool clockOk=gettimeofday(&tv,nullptr)==0 && tv.tv_sec>=1577836800LL;
            const int64_t monoAfter=esp_timer_get_time();
            if(clockOk && monoAfter-monoBefore<2000) {
                b.timeUs=static_cast<int64_t>(tv.tv_sec)*1000000LL+tv.tv_usec;
                b.nonce=static_cast<uint32_t>(monoBefore+(monoAfter-monoBefore)/2);
                // Beta132-DIAGNOSIS: Track coordinator production beacons sent (Beta136: exported)
                if(sendFrame(b,kBroadcastMac)) {
                    ++coordinatorProductionBeaconsSent;
                    if(beaconExperimentUntilMs) ++beaconExperimentSent;
                    // Log every 10th beacon to verify sending
                    if(coordinatorProductionBeaconsSent % 10 == 0) {
                        Serial.printf("[COORDINATOR] Production beacon #%u sent\n", coordinatorProductionBeaconsSent);
                    }
                }
            }
        }
        return;
    }
    if(static_cast<int32_t>(now-nextReportMs)>=0){
        nextReportMs=now+5000UL;
        sendNodeReports(epoch,tag,localBootNonce);
    }
    if(static_cast<int32_t>(now-nextSeriesStatsReportMs)>=0){
        nextSeriesStatsReportMs=now+15000UL;
        sendSeriesStatistics(epoch,tag,localBootNonce);
    }
    if(!syncPermissionValid()) {
        // Keep the receiver active and the last calibrated clock in holdover.
        pending={};seriesCount=0;seriesTarget=kInitialSamples;seriesStartedMs=0;nextSeriesMs=now+1000UL;
        return;
    }
    if(pending.active&&static_cast<uint32_t>(now-pending.startedMs)>=kExchangeTimeoutMs){
        pending={};++stats.exchangeTimeouts;
    }
    if(seriesCount==0&&static_cast<int32_t>(now-nextSeriesMs)<0)return;
    if(seriesCount>0 && uint32_t(now-seriesStartedMs)>kMaxSeriesDurationMs) {
        // Never extend a busy/failed measurement series indefinitely.
        seriesCount=0;seriesTarget=kInitialSamples;seriesStartedMs=0;
        pending={};++failedSeries;lastSeriesFailure=4;++seriesRejectByReason[4];lastRejectedReason=4;lastRejectedSeriesMs=now;lastRejectedHasMetrics=false;
        lastCompletedSeriesMs=now;lastCompletedSeriesResult=3;
        consecutiveFailures=static_cast<uint8_t>(std::min(8U,unsigned(consecutiveFailures)+1U));
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
    // Beta131-FIX (Fifth cycle, Problem 2): Probe uses ACTIVE time source, not coordinator
    // CRITICAL: When broadcaster is active, probe must reference broadcaster (activeTimeSourceTag)
    // Nodes sync with whoever is currently sending beacons (coordinator OR broadcaster)
    p.coordinatorTag=activeTimeSourceTag?activeTimeSourceTag:tag;  // Active source or fallback to coordinator
    p.senderBoot=localBootNonce;p.nonce=nonceSeed;
    int64_t probeStampUs=0;
    if(sendFrame(p,kBroadcastMac,&probeStampUs,false)) {
        pending.active=true;pending.nonce=p.nonce;
        // V2: Apply software timestamp correction using THIS Probe's measured delay
        // sendFrame() measured delay AFTER esp_now_send() and stored in lastProbePreSendUs
        // This is the CURRENT Probe's actual delay, not from a previous Probe
        pending.t1=p.timeUs + static_cast<int64_t>(lastProbePreSendUs);
        pending.startedMs=now;
        // Beta131-FIX (Sixth cycle, Problem 1): Store expected sender identity
        // CRITICAL: Reply/Final must come from same time source as Probe was sent to
        // Beta131-FIX (Eleventh cycle): Use active time source epoch, not local epoch
        pending.expectedTag=p.coordinatorTag;    // Active time source (broadcaster OR coordinator)
        pending.expectedBoot=activeTimeSourceBoot;   // Boot ID from last beacon
        pending.expectedEpoch=activeTimeSourceTag ? activeTimeSourceEpoch : 0;  // Active source epoch (0 if bootstrap)
        // Beta131-DIAGNOSIS (Fourteenth cycle): Log probe details for troubleshooting
        if(stats.probesSent % 10 == 0) {  // Every 10th probe to reduce log spam
            Serial.printf("[PROBE-TX] coordinatorTag=%llx expectedBoot=%llx expectedEpoch=%u\n",
                p.coordinatorTag, pending.expectedBoot, pending.expectedEpoch);
        }
        // Beta131-FIX (Seventh cycle, Problem 2): Store expected MAC from validated beacon
        // CRITICAL: Reply/Final MAC must match beacon sender to prevent MAC spoofing
        if(activeTimeSourceTag) {
            memcpy(pending.expectedMac, activeTimeSourceMac, 6);  // MAC from validated beacon
        } else {
            memset(pending.expectedMac, 0, 6);  // No active source yet (fallback)
        }
        ++stats.probesSent;
    }
    // Clock selection is performed by clusterTimeNowUs(), never by callback.
}

// Beta131-FIX (Eighth cycle, Problem 2): Time source generation for capture validation
// CRITICAL: Captures must detect source changes even within same epoch
// Returns generation counter that increments on every source switch
uint32_t timeSourceGeneration() {
    return sourceChanges;
}

}
#else
namespace ClusterEspNowRadio {
void service(bool,bool,uint32_t,const char*,uint32_t){}
void serviceDiagnosticBroadcaster(bool,uint32_t,const char*,const char*,uint32_t){}
GrantDiagnosticStats grantDiagnosticStats(){return {}; }
void stop(){}
void pauseForCapture(uint32_t){}
bool capturePaused(){return false;}
bool syncPermissionValid(){return false;}
bool precisionUtcNow(uint32_t,const char*,int64_t&,uint32_t&,double&){return false;}
size_t beaconSharedEventCount(){return 0;}
bool beaconSharedEventAt(size_t, BeaconSharedEvent&){return false;}
uint64_t beaconSharedEventBoot(){return 0;}
BeaconPredictionStats beaconPredictionStats(){return {};}
size_t beaconObservationCount(){return 0;}
bool beaconObservationAt(size_t,BeaconObservation&){return false;}
Diagnostics diagnostics(){return {};}
DiagnosticPredictionStats diagnosticPredictionStats(){return {};}
DiagnosticBeaconRxStats diagnosticBeaconRxStats(){return {};}
bool beaconExperimentStart(uint32_t,bool){return false;}
void beaconExperimentAbort(){}
BeaconExperimentState beaconExperimentSnapshot(){return {};}
bool benchmarkStart(uint32_t intervalMs,uint32_t targetBoot){return false;}
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
