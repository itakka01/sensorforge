#pragma once
#include <stdint.h>
#include <stddef.h>
namespace ClusterEspNowRadio {
// ESP-NOW cluster time (beta147, ESPNOW_TIME_ARCHITECTURE.md):
// - coordinator: signed beacon about once per second plus a follow-up with the
//   real send time; answers two-way probes of the current calibration slot owner
// - node: one-way beacon model (offset + drift, cluster_beacon_sync.h); the path
//   delay of the link is calibrated rarely by a two-way series, until then the
//   cluster median delay from the beacon is used
// Wi-Fi time remains the fallback. Called from the ordinary cluster loop, never
// from Wi-Fi callbacks.
void service(bool allowed, bool coordinator, uint32_t epoch,
             const char *coordinatorId, uint32_t localBootNonce);
void stop();
// Revoke short-lived permission, cancel samples, suppress TX during protected work.
// Keeps the last beacon model for holdover.
void pauseForCapture(uint32_t holdMs); // also used for coordinator list actions
bool capturePaused();
// A live, authenticated permission lease is required for node probes.
bool syncPermissionValid();
// Authenticated UTC from the beacon model of the current coordinator.
// False = use Wi-Fi time. sampleAgeMs = age of the newest beacon sample,
// jitterUs = RMS residual of the model (quality, NOT an accuracy bound).
bool precisionUtcNow(uint32_t epoch, const char *coordinatorId,
                     int64_t &utcUs, uint32_t &sampleAgeMs, double &jitterUs);

// Why precisionUtcNow() is false on a node (0 = valid).
enum class PrecisionState : uint8_t {
    Valid = 0, Inactive, Coordinator, NoBeacons, TooFewSamples, SpanTooShort,
    JitterTooHigh, DriftOutOfRange, Stale, NoDelay, NoUtc,
};
const char *precisionStateName(PrecisionState s);
// Delay used for the coordinator time: none, own two-way calibration, cluster median.
enum class DelaySource : uint8_t { None = 0, Own = 1, Cluster = 2 };
const char *delaySourceName(DelaySource s);

struct Diagnostics {
    uint32_t sent=0, received=0, authenticated=0, rejected=0, replay=0, dropped=0;
    uint32_t probesSent=0, probesAnswered=0, exchangesComplete=0, exchangeTimeouts=0;
    uint8_t channel=0;
    bool active=false;
    uint32_t capturePauseRemainingMs=0;

    // --- Beacon, coordinator side
    uint32_t beaconsSent=0, followUpsSent=0, followUpsWithoutTxDone=0;
    uint32_t beaconTxPathLastUs=0, beaconTxPathMinUs=0, beaconTxPathMaxUs=0;
    bool clusterDelayKnown=false;     // coordinator: median of node calibrations; node: from beacon
    double clusterDelayUs=0;
    uint32_t clusterDelayContributors=0;  // coordinator only

    // --- Beacon, node side
    uint32_t beaconsReceived=0, followUpsReceived=0, followUpsUnmatched=0;
    uint32_t beaconSamplesAccepted=0, beaconSamplesTxPathLate=0, beaconSamplesEarly=0;
    uint32_t beaconModelResets=0;
    bool beaconModelValid=false;
    uint8_t beaconModelSamples=0, beaconModelSelected=0;
    uint32_t beaconLastAgeMs=0;          // 0 = never
    uint32_t beaconModelSpanMs=0;
    double beaconOffsetUs=0;             // envelope offset (coordinator - node, without delay)
    double beaconDriftPpm=0;
    double beaconJitterUs=0;
    int32_t beaconTxPathMinNodeUs=-1;    // shortest TX path seen in follow-ups
    DelaySource delaySource=DelaySource::None;
    double delayUsedUs=0;
    bool ownDelayKnown=false;
    double ownDelayUs=0;
    uint32_t ownDelayAgeMs=0;
    uint8_t ownDelayCalibrations=0;
    uint32_t calibrationsDiscarded=0;    // two-way series without usable beacon model
    bool calibrationPending=false;
    PrecisionState precisionState=PrecisionState::Inactive;
    bool precisionValid=false;
    uint32_t sourceChanges=0;

    // Two-way calibration series (since radio start). Last failure reason:
    // 0=none, 1=invalid/insufficient, 2=spread, 3=RTT, 4=series deadline.
    uint32_t seriesAccepted=0, seriesRejected=0;
    uint8_t seriesLastFailureReason=0;
    // Rejected series by reason (1..4 of seriesLastFailureReason).
    uint32_t seriesRejectedInsufficient=0, seriesRejectedSpread=0, seriesRejectedRtt=0, seriesRejectedTimeout=0;
    // Node: beacon reports to the coordinator (unicast), acknowledged / not
    // acknowledged by its MAC layer, and repeats after a missing acknowledgement.
    uint32_t reportsAcked=0, reportsUnacked=0, reportRetries=0;

    // Coordinator only: calibration slot scheduling (cluster_slot_scheduler.h).
    // slotCycleEstimateMs = active peers x measured slot time per accepted series
    // (0 until a series was accepted). slotRotations = finished slots.
    uint32_t slotActivePeers=0, slotCycleEstimateMs=0, slotRotations=0;
    uint32_t slotRefreshMs=0, slotLifetimeMs=0;
    // Slot table size and whether an active node found no free slot within
    // the last kPeerActiveMs (it then gets no calibration).
    uint32_t slotCapacity=0;
    bool slotTableFullRecent=false;
    uint32_t slotSucceeded=0, slotFailed=0;
    // Active peers: due for a calibration now / without calibration / oldest calibration.
    uint32_t slotDuePeers=0, slotExpiredPeers=0, slotOldestAgeMs=0;
};
Diagnostics diagnostics();

// Last 128 beacons (PSRAM), for offline analysis across nodes: the same
// sequence identifies the same beacon on every node. Software timestamps only.
struct BeaconTraceEntry {
    uint32_t sequence=0;      // beacon frame sequence
    int64_t localUs=0;        // node: receive callback; coordinator: pre-send stamp
    int64_t coordUs=0;        // coordinator pre-send stamp (node: from the follow-up)
    int32_t txPathUs=-1;      // coordinator stamp -> TX-done callback, -1 unknown
    int32_t residualUs=0;     // node: sample minus envelope before adding (0 = no model)
    int64_t estCoordUs=0;     // node: estimated coordinator time at localUs before adding (0 = not valid)
    uint8_t result=0;         // node: ClusterBeaconSync::AddResult; 255 = follow-up missing
};
size_t beaconTraceCount();
bool beaconTraceAt(size_t index, BeaconTraceEntry &out);  // 0 = oldest
uint64_t beaconTraceCoordinatorBoot();

// Controlled authenticated bidirectional transport benchmark. Only the coordinator
// may start a run; Capture always aborts it. One discovered node at a time.
struct BenchmarkPeer {
    uint32_t boot=0, downReceived=0, upReceived=0, downRttMinUs=0, downRttMaxUs=0;
    uint64_t downRttTotalUs=0;
    uint8_t downSeen[100]={}, upSeen[100]={};
    uint32_t downRttUs[100]={};
};
struct BenchmarkState {
    bool running=false, finished=false, aborted=false;
    uint32_t runId=0, startedMs=0, completedMs=0, intervalMs=50;
    uint8_t peerCount=0, peerIndex=0, phase=0;
    BenchmarkPeer peers[3];
};
// Snapshot is bounded, has no heap ownership and does not alter measurement state.
bool benchmarkStart(uint32_t intervalMs=50,uint32_t targetBoot=0);
void benchmarkAbort();
BenchmarkState benchmarkSnapshot();
struct BenchmarkExchange {
    uint32_t run=0;
    uint16_t seq=0;
    int64_t t1=0,t2=0,t3=0,t4=0;
};
size_t benchmarkExchangeCount();
bool benchmarkExchangeAt(size_t index, BenchmarkExchange &out);
uint32_t benchmarkNodeRun();
void benchmarkLocalTransport(uint32_t &runId,uint32_t &downReceived,uint32_t &upAcknowledged);
bool benchmarkLocalPacket(size_t seq,bool &downReceived,bool &upAcked,uint32_t &upAckRttUs);

struct NodeSyncReport {
    uint32_t bootNonce=0, receivedAgeMs=0;
    // Two-way calibration (NodeSyncReport frames): sampleAgeMs = age of the
    // last successful calibration, precisionAccepted = node has its own delay.
    uint32_t sampleAgeMs=0;
    bool precisionAccepted=false;
    // Series counters of the node, per radio runtime.
    uint32_t successfulSeries=0, rejectedSeries=0;
    // Beacon sync (BeaconSyncReport frames).
    bool hasBeaconReport=false;
    uint32_t beaconReceivedAgeMs=0;    // local age of the beacon report
    bool beaconModelValid=false, precisionValid=false;
    PrecisionState precisionState=PrecisionState::Inactive;
    DelaySource delaySource=DelaySource::None;
    uint8_t beaconSamples=0;
    uint32_t beaconAgeMs=0;            // node: age of its newest beacon sample at report time
    double beaconJitterUs=0;
    int32_t driftMilliPpm=0;
    bool ownDelayKnown=false;
    double ownDelayUs=0;
    uint32_t ownDelayAgeS=0;
    // Coordinator-side statistics of this node boot, from its beacon reports.
    // A report gap longer than kReportGapMs is no precision loss; its time
    // beyond kReportGapMs is left out of monitoredS.
    uint32_t monitoredS=0;             // since the first beacon report, without gaps
    uint32_t preciseS=0;
    uint32_t precisionLosses=0;        // reported precise -> not precise
    uint32_t lastLossAgeS=UINT32_MAX;  // UINT32_MAX = no loss yet
    PrecisionState lastLossState=PrecisionState::Valid;  // state the node reported
    uint32_t reportGaps=0;             // reports missing for longer than kReportGapMs
    uint32_t lastGapAgeS=UINT32_MAX;   // UINT32_MAX = no gap yet
    double maxJitterUs=0;              // largest jitter reported while precise
};
// Beacon reports arrive every 10 s; a longer silence than this is a report gap.
constexpr uint32_t kReportGapMs = 30000;
// Coordinator-side table size for node reports, slots and replay state
// (100-node cluster plus rebooted/left nodes).
constexpr size_t kNodeReportCapacity = 128;
// Copy up to capacity reports, one per last authenticated node boot ID.
// Never infer a sync from Probes or from Wi-Fi status.
size_t nodeSyncReports(NodeSyncReport* out, size_t capacity);

// Monotonic counter that increments on every time source change (radio
// restart, coordinator boot change). Captures compare it to detect a switch.
uint32_t timeSourceGeneration();

}
