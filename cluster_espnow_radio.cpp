#include "cluster_espnow_radio.h"
#include "cluster_espnow_packets.h"
#include "cluster_espnow_twoway.h"
#include "cluster_espnow_drift.h"
#include "cluster_espnow_regression.h"
#include "cluster.h"
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
namespace ClusterEspNowRadio {
namespace {
constexpr uint32_t kBroadcastEveryMs=10000;
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
// Callback-safe TX latency sampling. Entries are inserted before esp_now_send().
// Match by destination MAC and FIFO; no per-packet sequence is supplied to TX callbacks.
constexpr size_t kTxPendingCapacity=24;
struct TxPending { bool used=false; uint8_t mac[6]={}; int64_t enqueueUs=0; };
TxPending txPending[kTxPendingCapacity];
portMUX_TYPE txMux=portMUX_INITIALIZER_UNLOCKED;
uint32_t txLatencyLastUs=0,txLatencyMaxUs=0,txLatencySamples=0;
uint32_t txCallbackFailed=0,txPendingOverflow=0,txCallbackUnmatched=0;
void txTimingReset() {
    portENTER_CRITICAL(&txMux);
    for(auto &entry:txPending)entry={};
    txLatencyLastUs=txLatencyMaxUs=txLatencySamples=0;
    txCallbackFailed=txPendingOverflow=txCallbackUnmatched=0;
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
    if(match==kTxPendingCapacity)++txCallbackUnmatched;
    else {
        const int64_t elapsed=now-txPending[match].enqueueUs;
        txPending[match].used=false;
        if(elapsed>=0 && elapsed<=UINT32_MAX){
            txLatencyLastUs=static_cast<uint32_t>(elapsed);
            if(txLatencyLastUs>txLatencyMaxUs)txLatencyMaxUs=txLatencyLastUs;
            ++txLatencySamples;
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
uint32_t captureGuardUntilMs=0;
uint32_t permissionUntilMs=0, nextGrantMs=0;
bool coordinatorPermissionIssued=false;
bool sendFrame(EspNowTimePackets::Frame& f,const uint8_t mac[6]);
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
EspNowTimePackets::ReplayWindow coordinatorReplay;
struct PeerReplay { bool used=false; uint8_t mac[6]={}; EspNowTimePackets::ReplayWindow replay; };
PeerReplay peerReplay[12];
struct Pending { bool active=false; uint32_t nonce=0; int64_t t1=0,t2=0,t4ReplyRx=0; uint32_t startedMs=0; } pending;
ClusterEspNowTwoWay::Exchange exchanges[kMaxSamplesPerSeries];
uint8_t seriesCount=0;
uint8_t seriesTarget=kInitialSamples;
uint32_t seriesStartedMs=0;
ClusterEspNowRegression::Point driftHistory[ClusterEspNowRegression::kHistory];
size_t driftHistoryCount=0;
uint32_t nonceSeed=0;
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
uint32_t successfulSeries=0, lastCompletedSeriesMs=0, nextSeriesStatsReportMs=0;
uint8_t lastCompletedSeriesResult=0;
uint8_t lastSeriesFailure=0; // 1=invalid/insufficient; 2=spread; 3=RTT; 4=series deadline
ClusterEspNowTwoWay::Estimate lastRejectedEstimate;
uint8_t consecutiveFailures=0;
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
    stats.lastExchangeValid = e.valid;
    seriesPrecisionAccepted=false;
    lastCompletedSeriesMs=millis();
    lastCompletedSeriesResult=2;
    if (!e.valid) { ++failedSeries; lastSeriesFailure=1; lastRejectedEstimate=e; consecutiveFailures=static_cast<uint8_t>(std::min(8U,unsigned(consecutiveFailures)+1U)); return; }
    stats.lastOffsetUs = e.offsetUs;
    stats.lastRttUs = e.medianRttUs;
    // A wide offset range indicates RF/software timing jitter, not calibrated drift.
    if (!std::isfinite(e.spreadUs) || e.spreadUs > 250.0 || e.accepted < 8 ||
        !std::isfinite(e.medianRttUs) || e.medianRttUs > 30000.0) {
        ++failedSeries;
        lastSeriesFailure=(e.accepted<8?1:(e.spreadUs>250.0?2:3));
        lastRejectedEstimate=e;
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
    if(!info||!info->src_addr||!data||length!=static_cast<int>(EspNowTimePackets::kPacketSize))return;
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
    if(esp_now_init()!=ESP_OK)return false;
    if(esp_now_register_recv_cb(onReceive)!=ESP_OK){esp_now_deinit();return false;}
    txTimingReset();
    if(esp_now_register_send_cb(onSent)!=ESP_OK){esp_now_deinit();return false;}
    esp_now_peer_info_t peer={};
    memcpy(peer.peer_addr,kBroadcastMac,6);peer.channel=0;peer.ifidx=WIFI_IF_STA;
    if(esp_now_add_peer(&peer)!=ESP_OK){esp_now_deinit();return false;}
    stats.rxQueueDelayLastUs=stats.rxQueueDelayMaxUs=0;
    stats.replyFinalDispatchGapLastUs=stats.replyFinalDispatchGapMaxUs=0;
    initialized=true;boundChannel=channel;stats.active=true;stats.channel=channel;
    nextBroadcastMs=millis()+1000;nextSeriesMs=millis()+2000;nextGrantMs=millis();
    permissionUntilMs=0;coordinatorPermissionIssued=false;nextReportMs=millis()+1500UL;
    for(auto &p:slotPeers)p={};slotNextIndex=0;slotOwner=-1;
    slotStartedMs=0;slotGuardUntilMs=millis()+kSlotGuardMs;
    slotGrantsSent=slotRotations=0;
    return true;
}
uint32_t nextSequence(){++txSequence;if(!txSequence)++txSequence;return txSequence;}
bool sendFrame(EspNowTimePackets::Frame& f,const uint8_t mac[6]) {
    f.sequence=nextSequence();
    uint8_t b[EspNowTimePackets::kPacketSize];
    if(!EspNowTimePackets::encode(f,b,clusterEspNowTimeSign,nullptr))return false;
    // Keep instrumentation bounded and non-blocking even under callback loss.
    const int64_t now=esp_timer_get_time();
    size_t slot=kTxPendingCapacity;
    portENTER_CRITICAL(&txMux);
    for(size_t i=0;i<kTxPendingCapacity;++i) {
        if(txPending[i].used && now-txPending[i].enqueueUs>2000000LL)txPending[i].used=false;
        if(!txPending[i].used && slot==kTxPendingCapacity)slot=i;
    }
    if(slot!=kTxPendingCapacity) {
        txPending[slot].used=true;
        memcpy(txPending[slot].mac,mac,6);
        txPending[slot].enqueueUs=now;
    } else ++txPendingOverflow;
    portEXIT_CRITICAL(&txMux);
    if(esp_now_send(mac,b,sizeof(b))!=ESP_OK) {
        if(slot!=kTxPendingCapacity){
            portENTER_CRITICAL(&txMux);
            // Do not clear a slot that has already been reused.
            if(txPending[slot].used && txPending[slot].enqueueUs==now)txPending[slot].used=false;
            portEXIT_CRITICAL(&txMux);
        }
        return false;
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
    if(slot->replay.initialized &&
       (slot->replay.boot!=f.senderBoot||slot->replay.epoch!=f.epoch))
        slot->replay={}; // authenticated new boot; no time state transferred
    return EspNowTimePackets::acceptSequence(slot->replay,f);
}
void resetTiming(){pending={};seriesCount=0;seriesTarget=kInitialSamples;seriesStartedMs=0;nextProbeMs=0;for(auto &p:peerReplay)p={};coordinatorReplay={};}
void finishSample(int64_t t3) {
    const int64_t t4=pending.t4ReplyRx;
    if(!pending.active||!pending.t2||!t4||t3<pending.t2||t4<pending.t1)return;
    if(seriesCount<kMaxSamplesPerSeries)
       exchanges[seriesCount++]={pending.t1,pending.t2,t3,t4};
    pending={};++stats.exchangesComplete;
    if(seriesCount>=seriesTarget) {
        auto e=ClusterEspNowTwoWay::estimate(exchanges,seriesCount);
        const bool quality=e.valid && e.accepted>=8 &&
            e.spreadUs<=250.0 && e.medianRttUs<=30000.0;
        if(!quality && seriesTarget<kMaxSamplesPerSeries &&
           uint32_t(millis()-seriesStartedMs)<kMaxSeriesDurationMs) {
            seriesTarget=seriesTarget==kInitialSamples ? kExpandedSamples : kMaxSamplesPerSeries;
        } else {
            acceptEstimate(e);
            seriesCount=0;seriesTarget=kInitialSamples;seriesStartedMs=0;
            nextSeriesMs=millis()+(seriesPrecisionAccepted ? 45000UL : qualityRetryDelayMs());
        }
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
void stop() {
    captureGuardUntilMs = 0;
    permissionUntilMs=0;coordinatorPermissionIssued=false;localBootNonceForControl=0;
    if(initialized){esp_now_deinit();initialized=false;}
    txTimingReset();
    boundChannel=0;lastEpoch=0;lastTag=0;stats.active=false;
    stats.lastExchangeValid=false;stats.lastOffsetUs=0;stats.lastRttUs=0;
    resetTiming();clearQueue();
    for(auto &p:slotPeers)p={};slotNextIndex=0;slotOwner=-1;
    slotStartedMs=slotGuardUntilMs=0;slotGrantsSent=slotRotations=0;
    previousEstimate={};latestQualityEstimate={};lastPrecisionMs=0;failedSeries=0;lastSeriesFailure=0;lastRejectedEstimate={};consecutiveFailures=0;
    successfulSeries=0;lastCompletedSeriesMs=0;lastCompletedSeriesResult=0;nextSeriesStatsReportMs=0;
    referenceUtcUs=0;referenceMonoLow=0;referenceReceivedNodeMono=0;
    referenceId=String();goodDriftPairs=0;driftHistoryCount=0;
    for(auto &r:remoteReports)r={};for(auto &x:remoteReceivedAt)x=0;remoteNext=0;reportCount=0;
    stats.driftValid=false;stats.storedDriftAvailable=false;stats.driftPpm=0;
    stats.driftUncertaintyPpm=0;stats.driftSamples=0;
}
bool precisionUtcNow(uint32_t epoch, const char *coordinatorId,
                    int64_t &utcUs, uint32_t &sampleAgeMs, double &rttUs) {
    if (!initialized || wasCoordinator || epoch != lastEpoch ||
        !coordinatorId || idTag(coordinatorId) != lastTag ||
        !latestQualityEstimate.valid || !referenceUtcUs ||
        !lastPrecisionMs || !referenceReceivedNodeMono ||
        (uint32_t)(millis()-lastPrecisionMs)>kPrecisionLifetimeMs) return false;
    const int64_t now=esp_timer_get_time();
    if (now<referenceReceivedNodeMono || now-referenceReceivedNodeMono>kReferenceLifetimeUs) return false;
    const double age=static_cast<double>(now-latestQualityEstimate.nodeReferenceUs);
    if (age<0 || age>static_cast<double>(kPrecisionLifetimeMs)*1000.0) return false;
    const double drift=stats.driftValid ? stats.driftPpm : 0.0;
    if (!std::isfinite(drift) || fabs(drift)>500.0) return false;
    const double coordinatorNow=static_cast<double>(now)+latestQualityEstimate.offsetUs+
        age*drift/1000000.0;
    // Reconstruct 64-bit coordinator monotonic time from the beacon's low
    // 32 bits, choosing the unique wrap nearest to the current estimate.
    const int64_t approx=static_cast<int64_t>(llround(coordinatorNow));
    const int32_t delta=static_cast<int32_t>(referenceMonoLow-static_cast<uint32_t>(approx));
    const int64_t referenceMono=approx+delta;
    if (referenceMono<=0 || llabs(referenceMono-approx)>kReferenceLifetimeUs) return false;
    const long double target=(long double)referenceUtcUs+(long double)coordinatorNow-referenceMono;
    if(target<1577836800000000.0L || target>4102444800000000.0L) return false;
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
Diagnostics diagnostics(){Diagnostics out=stats;
    portENTER_CRITICAL(&txMux);
    out.txCallbackLatencyLastUs=txLatencyLastUs;
    out.txCallbackLatencyMaxUs=txLatencyMaxUs;
    out.txCallbackSamples=txLatencySamples;
    out.txCallbackFailed=txCallbackFailed;
    out.txPendingOverflow=txPendingOverflow;
    out.txCallbackUnmatched=txCallbackUnmatched;
    portEXIT_CRITICAL(&txMux);
    out.lastAcceptedAgeMs=lastPrecisionMs ? uint32_t(millis()-lastPrecisionMs) : 0;out.precisionRecentlyAccepted=lastPrecisionMs && out.lastAcceptedAgeMs<=kPrecisionLifetimeMs;out.capturePauseRemainingMs=capturePaused() ? uint32_t(captureGuardUntilMs-millis()) : 0;out.syncPermission=syncPermissionValid();out.syncPermissionRemainingMs=out.syncPermission?uint32_t(permissionUntilMs-millis()):0;portENTER_CRITICAL(&queueMux);out.dropped=drops;portEXIT_CRITICAL(&queueMux);return out;}
void service(bool allowed,bool coordinator,uint32_t epoch,const char* id,uint32_t localBootNonce) {
    if(!allowed||!epoch||!localBootNonce||!id||!*id||WiFi.status()!=WL_CONNECTED||
       WiFi.getMode()!=WIFI_MODE_STA){if(initialized)stop();return;}
    uint8_t channel=0;wifi_second_chan_t secondary=WIFI_SECOND_CHAN_NONE;
    if(esp_wifi_get_channel(&channel,&secondary)!=ESP_OK||!channel){if(initialized)stop();return;}
    uint64_t tag=idTag(id);
    if(initialized&&(boundChannel!=channel||lastEpoch!=epoch||lastTag!=tag||wasCoordinator!=coordinator))stop();
    if(!initialized){
        if(!start(channel))return;
        if(!coordinator)loadDriftPrior(id);
    }
    wasCoordinator=coordinator;lastEpoch=epoch;lastTag=tag;localBootNonceForControl=localBootNonce;
    if (capturePaused()) return; // Never send Reply, Final, Probe or Beacon during a capture hold.
    if (captureGuardUntilMs) {
        captureGuardUntilMs = 0;
        clearQueue(); // Discard frames received while processing was paused.
        nextBroadcastMs = millis() + kBroadcastEveryMs;
        nextSeriesMs = millis() + 1000UL;
    }
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
        if(!EspNowTimePackets::decode(item.bytes,sizeof(item.bytes),f,clusterEspNowTimeVerify,nullptr)||
           f.coordinatorTag!=tag||f.epoch!=epoch){++stats.rejected;continue;}
        if(coordinator) {
            if((f.type!=EspNowTimePackets::Type::Probe &&
                f.type!=EspNowTimePackets::Type::NodeSyncReport &&
                f.type!=EspNowTimePackets::Type::NodeDriftReport)||!acceptPeer(item.mac,f)){
                ++stats.rejected;continue;
            }
            ++stats.authenticated;
            if(f.type==EspNowTimePackets::Type::NodeSyncReport ||
               f.type==EspNowTimePackets::Type::NodeDriftReport){
                noteSlotPeer(item.mac,static_cast<uint32_t>(f.senderBoot));
                acceptNodeReport(f);continue;
            }
            // Only the slot owner may receive timestamp replies. Authentication
            // and replay validation above still apply for every received packet.
            if(!slotIsOwner(item.mac,static_cast<uint32_t>(f.senderBoot))) continue;
            // Four-timestamp exchange: t1=Node Probe TX, t2=Coordinator Probe RX,
            // t3=Coordinator REPLY TX call, t4=Node REPLY RX callback.
            // Final delivers t3 metadata only; Final RX is NEVER t4.
            // Software call timestamps are not RF/PHY TX timestamps.
            if(!ensurePeer(item.mac)){++stats.rejected;continue;}
            EspNowTimePackets::Frame r;
            r.type=EspNowTimePackets::Type::Reply;r.epoch=epoch;r.coordinatorTag=tag;
            r.senderBoot=localBootNonce;r.nonce=f.nonce;r.timeUs=item.rxUs;
            const int64_t replyDispatchBeforeUs=esp_timer_get_time();
            // t3 denotes the Reply send call, NOT the later Final send call.
            if(!sendFrame(r,item.mac))continue;
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
        if(f.type!=EspNowTimePackets::Type::SyncControl&&
           f.type!=EspNowTimePackets::Type::Beacon&&
           f.type!=EspNowTimePackets::Type::Reply&&
           f.type!=EspNowTimePackets::Type::Final){++stats.rejected;continue;}
        if(!EspNowTimePackets::acceptSequence(coordinatorReplay,f)){++stats.replay;continue;}
        ++stats.authenticated;
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
            // For beacons only: timeUs=UTC, nonce=lower 32 bits of the
            // coordinator's monotonic timestamp sampled alongside UTC.
            referenceUtcUs=f.timeUs;
            referenceMonoLow=f.nonce;
            referenceReceivedNodeMono=item.rxUs;
        }
        if(f.type==EspNowTimePackets::Type::Reply&&pending.active&&f.nonce==pending.nonce) {
            pending.t2=f.timeUs;
            pending.t4ReplyRx=item.rxUs; // t4: the Reply receive callback, not Final
        }
        if(f.type==EspNowTimePackets::Type::Final&&pending.active&&f.nonce==pending.nonce)
            finishSample(f.timeUs);
    }
    uint32_t now=millis();
    if(coordinator){
        serviceSlots(now,epoch,tag,localBootNonce);
        if(static_cast<int32_t>(now-nextBroadcastMs)>=0){
            nextBroadcastMs=now+kBroadcastEveryMs;
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
                sendFrame(b,kBroadcastMac);
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
        pending={};++failedSeries;lastSeriesFailure=4;
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
    p.type=EspNowTimePackets::Type::Probe;p.epoch=epoch;p.coordinatorTag=tag;
    p.senderBoot=localBootNonce;p.timeUs=esp_timer_get_time();p.nonce=nonceSeed;
    if(sendFrame(p,kBroadcastMac)) {
        pending.active=true;pending.nonce=p.nonce;pending.t1=p.timeUs;
        pending.startedMs=now;++stats.probesSent;
    }
    // Clock selection is performed by clusterTimeNowUs(), never by callback.
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
size_t nodeSyncReports(NodeSyncReport*,size_t){return 0;}
}
#endif
