#pragma once
#include <stdint.h>
#include <stddef.h>
namespace ClusterEspNowRadio {
// ESP-NOW clock candidate with strict freshness/quality gate; Wi-Fi remains fallback.
// Called from ordinary cluster loop, not from Wi-Fi callbacks.
void service(bool allowed, bool coordinator, uint32_t epoch,
             const char *coordinatorId, uint32_t localBootNonce);
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
struct Diagnostics {
    uint32_t sent=0, received=0, authenticated=0, rejected=0, replay=0, dropped=0;
    uint32_t probesSent=0, probesAnswered=0, exchangesComplete=0, exchangeTimeouts=0;
    // Firmware service latency, NOT RF/MAC airtime; max values since radio start.
    uint32_t rxQueueDelayLastUs=0, rxQueueDelayMaxUs=0;
    uint32_t replyFinalDispatchGapLastUs=0, replyFinalDispatchGapMaxUs=0;
    // esp_now_send() enqueue-to-driver-callback timing (NOT PHY TX timestamp).
    uint32_t txCallbackLatencyLastUs=0, txCallbackLatencyMaxUs=0, txCallbackSamples=0;
    uint32_t txCallbackFailed=0, txPendingOverflow=0, txCallbackUnmatched=0;
    int64_t lastLocalRxUs=0;
    int64_t lastRemoteStampUs=0;
    double lastOffsetUs=0, lastRttUs=0;
    uint8_t channel=0;
    bool active=false;
    bool lastExchangeValid=false;
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
};
Diagnostics diagnostics();
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
    uint32_t failedSpreadUs=0, failedRttUs=0;
    uint8_t failedAccepted=0, failedSelected=0;
    int32_t failedMinRttOffsetUs=0, failedSelectedOffsetUs=0;
    bool hasFailureMetrics=false, hasFailureOffsets=false, hasTimeSource=false, usingEspNowTime=false;
};
// Copy up to capacity reports, one per last authenticated node boot ID.
// Never infer a sync from Probes or from Wi-Fi status.
size_t nodeSyncReports(NodeSyncReport* out, size_t capacity);

}
