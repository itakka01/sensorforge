#pragma once
#include <stdint.h>
#include <stddef.h>
namespace ClusterEspNowRadio {
// ESP-NOW clock candidate with strict freshness/quality gate; Wi-Fi remains fallback.
// Called from ordinary cluster loop, not from Wi-Fi callbacks.
void service(bool allowed, bool coordinator, uint32_t epoch,
             const char *coordinatorId, uint32_t localBootNonce);
// Beta131-CORRECTED (Third cycle): Broadcaster does THREE things:
// 1. Sends diagnostic beacons (own identity) so coordinator can observe
// 2. Syncs WITH coordinator (coordinatorId) as node via Probe/Reply/Final
// 3. Sends production beacons when lease active
// CRITICAL: Must receive BOTH broadcaster's own ID AND coordinator ID
void serviceDiagnosticBroadcaster(bool allowed, uint32_t epoch,
                                  const char *broadcasterDeviceId, const char *coordinatorId,
                                  uint32_t localBootNonce);
struct GrantDiagnosticStats {
    uint32_t received=0,authenticated=0,rejected=0,replay=0,dropped=0;
    uint32_t grantsSent=0,acksSent=0,acksVerified=0,timeouts=0,sendFailed=0;
};
GrantDiagnosticStats grantDiagnosticStats();
void stop();
// Revoke short-lived permission, cancel samples, suppress TX during protected work.
// Keeps the last accepted clock estimate for holdover.
void pauseForCapture(uint32_t holdMs); // also used for coordinator list actions
bool capturePaused();
// A live, authenticated permission lease is required for node probes.
bool syncPermissionValid();
// Authenticated UTC anchored to coordinator monotonic clock. False = use Wi-Fi.
bool precisionUtcNow(uint32_t epoch, const char *coordinatorId,
                     int64_t &utcUs, uint32_t &sampleAgeMs, double &rttUs);
// Passive diagnostic snapshots of existing authenticated beacons; not a clock source.
struct BeaconObservation {
    uint32_t sequence=0, epoch=0, coordinatorMonoLow=0;
    uint64_t coordinatorBoot=0;
    int64_t coordinatorUtcUs=0, nodeRxCallbackUs=0;
    // One-step-ahead, trained only on earlier beacons. Not a capture clock.
    int32_t predictionErrorUs=0;
    int32_t driftPpmX100=0;
    uint8_t predictionSamples=0; // 0 = no valid prediction
    uint8_t predictionStatus=0; // 0=valid, 1=warmup, 2=identity, 3=span, 4=node time, 5=history, 6=regression, 7=slope, 8=range

};
// Constant-memory lifetime statistics of passive beacon predictions.
// Aggregated values cannot replace synchronized per-beacon raw traces.
struct BeaconPredictionStats {
    uint32_t received=0, valid=0, firstSequence=0, lastSequence=0;
    uint32_t epoch=0;
    uint64_t coordinatorBoot=0;
    int32_t minErrorUs=0, maxErrorUs=0;
    double meanErrorUs=0, errorM2=0, meanAbsErrorUs=0;
    double meanDriftPpm=0;
    uint32_t statusCounts[9]={};
};
// Compact, passive, authenticated shared-event trace (last 128 beacons).
// Timestamp is ESP-NOW RX callback time, NOT PHY arrival or camera exposure.
struct BeaconSharedEvent {
    uint32_t sequence=0, epoch=0, coordinatorMonoLow=0;
    uint64_t nodeRxCallbackUs=0;
    uint32_t coordinatorBootLow=0;
};
size_t beaconSharedEventCount();
bool beaconSharedEventAt(size_t index, BeaconSharedEvent &out);
uint64_t beaconSharedEventBoot();
BeaconPredictionStats beaconPredictionStats();
size_t beaconObservationCount();
bool beaconObservationAt(size_t index, BeaconObservation &out);
struct Diagnostics {
    uint32_t sent=0, received=0, authenticated=0, rejected=0, replay=0, dropped=0;
    uint32_t probesSent=0, probesAnswered=0, exchangesComplete=0, exchangeTimeouts=0;
    // Firmware service latency, NOT RF/MAC airtime; max values since radio start.
    uint32_t rxQueueDelayLastUs=0, rxQueueDelayMaxUs=0;
    uint32_t replyFinalDispatchGapLastUs=0, replyFinalDispatchGapMaxUs=0;
    // esp_now_send() enqueue-to-driver-callback timing (NOT PHY TX timestamp).
    uint32_t txCallbackLatencyLastUs=0, txCallbackLatencyMaxUs=0, txCallbackSamples=0;
    uint32_t txCallbackFailed=0, txPendingOverflow=0, txCallbackUnmatched=0;
    // t1/t3 software timestamp -> immediately before esp_now_send() (NOT RF airtime).
    uint32_t probeTxPreSendLastUs=0, probeTxPreSendMaxUs=0, probeTxPreSendSamples=0;
    uint32_t replyTxPreSendLastUs=0, replyTxPreSendMaxUs=0, replyTxPreSendSamples=0;
    uint64_t probeTxPreSendTotalUs=0, replyTxPreSendTotalUs=0;
    int64_t lastLocalRxUs=0;
    int64_t lastRemoteStampUs=0;
    double lastOffsetUs=0, lastRttUs=0;
    uint8_t channel=0;
    bool active=false;
    bool lastExchangeValid=false;
    // Last completed estimate, including failed quality checks, local HTTP only.
    bool lastSeriesEstimateAvailable=false;
    uint32_t lastSeriesEstimateAgeMs=0;
    uint8_t lastSeriesSelected=0;
    double lastSeriesSpreadUs=0;
    double lastSeriesRttMinUs=0, lastSeriesRttMaxUs=0;
    double lastSeriesOffsetP10Us=0, lastSeriesOffsetP90Us=0;
    bool driftValid=false;             // derived from fresh precision series
    bool storedDriftAvailable=false;   // prior estimate; NOT synchronization
    double driftPpm=0;
    double driftUncertaintyPpm=0;
    uint32_t driftSamples=0;
    uint32_t driftPersisted=0;
    uint32_t lastAcceptedAgeMs=0;
    bool precisionRecentlyAccepted=false;
    uint32_t capturePauseRemainingMs=0;
    bool syncPermission=false;
    uint32_t syncPermissionRemainingMs=0;
    // Beta131: Production broadcaster handover state
    bool handoverEnabled=true;
    bool broadcasterLeaseActive=false;
    uint32_t broadcasterLeaseRemainingMs=0;
    bool coordinatorSuppressingBeacons=false;
    uint32_t coordinatorBeaconsSuppressed=0;
    uint64_t activeTimeSource=0;  // 0=none, or tag of active coordinator/broadcaster
    uint32_t sourceChanges=0;
    uint32_t broadcasterProductionBeaconsSent=0;
};
Diagnostics diagnostics();
// Diagnostic SFDB receiver only; never used by precisionUtcNow() or capture.
struct DiagnosticBeaconRxStats {
    uint32_t received=0, authenticated=0, rejected=0, replay=0, dropped=0;
    uint32_t lastSequence=0, lastEpoch=0;
    uint64_t lastSenderTag=0, lastSenderBoot=0;
    int64_t lastRxCallbackUs=0, lastRemoteMonotonicUs=0, lastRemoteUtcUs=0;
    // Observation only: consecutive signed frames and local freshness. NOT authority.
    uint32_t candidateConsecutive=0, candidateAgeMs=0;
    bool candidateFresh=false;
    // Passive simulation only: never grants TX authority or changes clocks.
    bool shadowActive=false;
    uint32_t shadowActivations=0, shadowFallbacks=0, shadowSourceChanges=0;
    // Constant-memory interval residuals, callback timestamps only; NOT clock accuracy.
    uint32_t intervalSamples=0, intervalSkipped=0;
    int32_t intervalMinUs=0, intervalMaxUs=0;
    double intervalMeanUs=0, intervalM2=0;
};
// Passive one-step-ahead prediction, separate from production clock/capture.
struct DiagnosticPredictionStats {
    uint32_t valid=0, warmup=0, invalid=0;
    int32_t minErrorUs=0, maxErrorUs=0;
    double meanErrorUs=0, errorM2=0, meanAbsErrorUs=0;
};
DiagnosticPredictionStats diagnosticPredictionStats();
DiagnosticBeaconRxStats diagnosticBeaconRxStats();
// Coordinator-only, manual beacon interval experiment; max 15 s, no Wi-Fi disconnect.
struct BeaconExperimentState { bool active=false, quiet=false; uint32_t intervalMs=0, sent=0, remainingMs=0; };
bool beaconExperimentStart(uint32_t intervalMs, bool quiet=false);
void beaconExperimentAbort();
BeaconExperimentState beaconExperimentSnapshot();
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
    uint32_t bootNonce=0, receivedAgeMs=0, sampleAgeMs=0;
    int32_t offsetUs=0;
    uint32_t rttUs=0;
    int32_t driftMilliPpm=0;
    uint16_t spreadUs=0;
    uint8_t acceptedSamples=0;
    bool precisionAccepted=false, driftValid=false, hasDriftReport=false;
    // Only meaningful when precisionAccepted=false and diagnostic frame flag set.
    uint32_t probesSent=0, exchangesComplete=0;
    uint8_t failureCode=0, failedSeries=0;
    // Authenticated, additive telemetry; per-node radio runtime, reset on radio restart.
    uint32_t successfulSeries=0, rejectedSeries=0;
    uint32_t lastSeriesAgeMs=0;
    uint8_t lastSeriesResult=0; // 0=no completed series, 1=accepted, 2=rejected, 3=deadline
    bool hasSeriesStatistics=false;
    bool hasLastRejectedSeries=false, lastRejectedMetricsValid=false;
    uint8_t lastRejectedReason=0;
    uint32_t lastRejectedAgeMs=0, lastRejectedSpreadUs=0, lastRejectedRttUs=0;
    uint32_t failedSpreadUs=0, failedRttUs=0;
    uint8_t failedAccepted=0, failedSelected=0;
    int32_t failedMinRttOffsetUs=0, failedSelectedOffsetUs=0;
    bool hasFailureMetrics=false, hasFailureOffsets=false, hasTimeSource=false, usingEspNowTime=false;
};
// Copy up to capacity reports, one per last authenticated node boot ID.
// Never infer a sync from Probes or from Wi-Fi status.
size_t nodeSyncReports(NodeSyncReport* out, size_t capacity);

}
