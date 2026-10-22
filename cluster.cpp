#include "cluster.h"
#include "cluster_time_grant_protocol.h"
#include "cluster_espnow_packets.h"
#include "cluster_espnow_radio.h"
#include "cluster_jobs.h"
#include "cluster_hw_time_pulse.h"
#include "cluster_hw_time_protocol.h"
#include "cluster_hw_time_gpio_lease.h"
#include "cluster_hw_time_series.h"
#include "cluster_hw_time_meter.h"
#include "cluster_capture_options.h"
#include "cluster_capture.h"
#include "drone_mode.h"
#include "webconfig.h"
#include "web_sd_maintenance.h"

#include "config.h"
#include "board_config.h"
#include "thermal.h"
#include <math.h>
#include "cluster_profiles.h"
#include "device_identity.h"
#include "logger.h"
#include "recorder.h"
#include "sensorforge_version.h"
#include "streamer.h"

#include <ESPmDNS.h>
#include <FS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <psa/crypto.h>
#include <esp_system.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <esp_timer.h>
#include "wifi_power_policy.h"
#include <stdlib.h>
#include <string.h>

// Owned by sensorforge.ino; read-only SD mount status.
extern bool sdReady;

namespace {

static const IPAddress CLUSTER_MULTICAST_GROUP(239, 255, 83, 70);
static const uint16_t CLUSTER_PORT = 39427;
static const uint16_t CLUSTER_COORDINATOR_PORT = 39428;
static const uint32_t CLUSTER_HEARTBEAT_MS = 10000UL;
static const uint32_t CLUSTER_DEFAULT_LEASE_SEC = 60UL;
static const uint32_t CLUSTER_COORDINATOR_HEARTBEAT_MS = 10000UL;
static const uint32_t CLUSTER_COORDINATOR_LEASE_SEC = 60UL;
static const uint32_t CLUSTER_DISCOVERY_SCAN_LEASE_MS = 12000UL;
static const uint32_t CLUSTER_DISCOVERY_NODE_TTL_MS = 35000UL;
static const uint32_t CLUSTER_LEGACY_PEER_TIMEOUT_MS = 60000UL;
static const uint32_t CLUSTER_RESOURCE_TTL_MAX_SEC = 3600UL;
static const size_t CLUSTER_MAX_PEERS = 16;
static const size_t CLUSTER_MAX_RESOURCES = 24;
static const size_t CLUSTER_MAX_DISCOVERY_NODES = 32;
static const size_t CLUSTER_DISCOVERY_PROOF_SLOTS = 8;
static const uint32_t CLUSTER_DISCOVERY_PROOF_TTL_MS = 35000UL;
static const uint32_t CLUSTER_DISCOVERY_PROOF_PAIR_WINDOW_MS = 3000UL;
static const size_t CLUSTER_PACKET_MAX = 511;
static const char *CLUSTER_SERVICE = "sfcluster";
static const char *CLUSTER_PROTO = "udp";
static const char *CLUSTER_DISCOVERY_MAGIC = "SFD1";
static const char *CLUSTER_PACKET_MAGIC = "SFC1";
static const char *CLUSTER_META_MAGIC = "SFM1";
static const char *CLUSTER_HEALTH_MAGIC = "SFH1";
static uint32_t healthLocalTotalMiB=0, healthLocalUsedMiB=0;
static bool healthForceSdRefresh=false;
static const char *CLUSTER_LEAVE_MAGIC = "SFL1";
static const char *CLUSTER_RESOURCE_MAGIC = "SFR1";
static const char *CLUSTER_COORDINATION_MAGIC = "SFCO1";
// Board capability bits carried in the existing (formerly always 0) SFCO1
// capabilities field. Display information only; never used for election/sync.
// Bit 0 marks the bitset as valid, so 0 from older firmware means "unknown".
static const uint32_t CLUSTER_CAP_BOARD_INFO = 1UL << 0;
static const uint32_t CLUSTER_CAP_PRECISION_SYNC = 1UL << 1;
static const uint32_t CLUSTER_LOCAL_CAPABILITIES =
    CLUSTER_CAP_BOARD_INFO | (BOARD_SUPPORTS_PRECISION_SYNC ? CLUSTER_CAP_PRECISION_SYNC : 0UL);

static const char *precisionSyncName(uint32_t capabilities)
{
    if (!(capabilities & CLUSTER_CAP_BOARD_INFO))
        return "unknown";
    return (capabilities & CLUSTER_CAP_PRECISION_SYNC) ? "yes" : "no";
}
static const char *CLUSTER_NODE_HEARTBEAT_MAGIC = "SFNH1";
static const char *CLUSTER_NODE_ACK_MAGIC = "SFNA1";
// Time is an OPTIONAL sidecar to the cluster: it never adjusts the system RTC.
static const char *CLUSTER_TIME_REQUEST_MAGIC = "SFTQ1";
static const char *CLUSTER_TIME_REPLY_MAGIC = "SFTR1";
static const char *CLUSTER_TIME_SYNC_COMMAND_MAGIC = "SFTS1";
static const char *CLUSTER_TIME_REPORT_MAGIC = "SFTD1";
// Beta72: authenticated, non-destructive transport probe only.
static const char *CLUSTER_JOB_PROBE_MAGIC = "SFJP1";
static const char *CLUSTER_JOB_ACK_MAGIC = "SFJA1";
// Beta75: explicit, single-node restart job. ACK confirms scheduling only.
static const char *CLUSTER_JOB_RESTART_MAGIC = "SFJR1";
static const char *CLUSTER_JOB_RESTART_ACK_MAGIC = "SFJRA1";
// Beta83: targeted shutdown, never broadcast or automatically resent.
static const char *CLUSTER_JOB_SHUTDOWN_MAGIC = "SFJS1";
static const char *CLUSTER_JOB_SHUTDOWN_ACK_MAGIC = "SFJSA1";
// Signed negative ACKs only after full coordinator authentication and replay consumption.
static const char *CLUSTER_JOB_REJECT_MAGIC = "SFJX1";
static const char *CLUSTER_JOB_HW_TIME_MAGIC="SFHT1";
static const char *CLUSTER_JOB_HW_TIME_ACK_MAGIC="SFHTA1"; // 1 armed; 2 rejected (not armed)
// Separate SFHT1 replay state; ordinary cluster-job sequences are unaffected.
static ClusterHwTimeProtocol::ReplayGuard hwTimeSeen;

static const char *CLUSTER_JOB_CAPTURE_MAGIC="SFJC1";
static const char *CLUSTER_JOB_CAPTURE_RESULT_MAGIC="SFJCR1";
static const char *CLUSTER_JOB_CAPTURE_RECEIPT_MAGIC="SFJCA1";
static const char *CLUSTER_JOB_DRONE_MAGIC = "SFJD1";
static const char *CLUSTER_JOB_DRONE_ACK_MAGIC = "SFJDA1";
static uint32_t droneSeenBoot=0,droneSeenEpoch=0,droneSeenSeq=0;
static const char *CLUSTER_JOB_WIPE_MAGIC = "SFJW1";
static const char *CLUSTER_JOB_WIPE_RESULT_MAGIC = "SFJWR1";
// UTC is resolved exactly once on acceptance. The deadline subsequently lives
// exclusively in the local monotonic esp_timer domain; NTP and new cluster
// synchronization samples cannot move an armed capture.
struct PendingCapture {
    bool active=false;
    uint32_t cb=0,epoch=0,seq=0;
    int64_t utc=0;
    int64_t anchorUtcUs=0,anchorMonoUs=0,deadlineMonoUs=0;
    // Beta131-FIX (Eighth cycle, Problem 2): Track time source generation
    uint32_t sourceGeneration=0;  // Detects source changes even within same epoch
    bool testPulse=false;  // SFJC1 option "tp1": GPIO marker in the capture path
};
static PendingCapture pendingCapture;
// Local diagnostic state of the optional capture test pulse (status JSON only).
static const char *captureTestPulseState="off";
bool clusterCaptureArmedFlag=false;
// Coordinator protection extends from first dispatched shot until 5 s after its deadline.
static uint32_t coordinatorCaptureGuardUntilMs=0;
static uint32_t captureSeenBoot=0,captureSeenEpoch=0,captureSeenSeq=0;
static uint32_t captureSent=0,captureAccepted=0,captureCompleted=0,captureFailed=0;
static int64_t captureLastDispatchOffsetUs=0;
static uint32_t captureLastSize=0;
static uint16_t captureLastError=0;
// At most one unacknowledged terminal result per node. A newly accepted
// capture invalidates any older result, matching PSRAM job ownership.
struct CaptureResultRetry {
    bool active=false;
    uint32_t cb=0,epoch=0,seq=0;
    uint16_t status=0,error=0;
    uint32_t bytes=0, lastSendMs=0;
    uint8_t attempts=0;
    char uuid[37]={};
    int64_t dispatch=0;
};
static CaptureResultRetry captureResultRetry;
static uint32_t captureResultRetrySent=0,captureResultReceiptReceived=0;
static uint32_t captureResultRetryExhausted=0;

static uint32_t wipeSeenBoot=0, wipeSeenEpoch=0, wipeSeenSeq=0;
static uint32_t wipeReceived=0, wipeQueued=0, wipeResultSent=0, wipeResultReceived=0;

static uint32_t shutdownSeenBoot=0, shutdownSeenEpoch=0, shutdownSeenSeq=0;
static uint32_t shutdownSent=0, shutdownReceived=0, shutdownAccepted=0, shutdownRejected=0;
static uint32_t shutdownAckReceived=0, shutdownAckAccepted=0, shutdownAckRejected=0;
static const char *shutdownLastReject="none", *shutdownAckLastReject="none";
static void rejectShutdown(const char *reason) {
    ++shutdownRejected; shutdownLastReject=reason;
    Serial.printf("[CLUSTER JOB] shutdown rejected: %s\n", reason);
}
static void rejectShutdownAck(const char *reason) {
    ++shutdownAckRejected; shutdownAckLastReject=reason;
    Serial.printf("[CLUSTER JOB] shutdown ACK rejected: %s\n", reason);
}

static uint32_t restartSeenBoot = 0, restartSeenEpoch = 0, restartSeenSeq = 0;
static uint32_t restartSent = 0, restartReceived = 0, restartAccepted = 0, restartRejected = 0;
static uint32_t restartAckReceived = 0, restartAckAccepted = 0, restartAckRejected = 0;
static const char *restartAckLastReject = "none";
static const char *restartLastReject = "none";
// Beta76: passive, RAM-only boot-change evidence for an issued restart.
// Evidence is deliberately independent of ACK and job timeout states.
struct RestartEvidence {
    uint64_t jobId;
    char nodeId[36];
    uint32_t expectedOldBoot;
    uint32_t observedNewBoot;
};
static RestartEvidence restartEvidence[ClusterJobs::kMaxJobs] = {};
static size_t restartEvidenceCount = 0;

static void rejectRestart(const char *reason) {
    ++restartRejected; restartLastReject = reason;
    Serial.printf("[CLUSTER JOB] restart rejected: %s\n", reason);
}
static void rejectRestartAck(const char *reason) {
    ++restartAckRejected; restartAckLastReject = reason;
    Serial.printf("[CLUSTER JOB] restart ACK rejected: %s\n", reason);
}
static uint32_t probeSeq = 0;
static uint32_t probeSeenBoot = 0, probeSeenEpoch = 0, probeSeenSeq = 0;
// Probe-only bounded retransmission. Never used for restart or other actions.
struct ProbeRetry {
    uint64_t id;
    uint32_t targetBoot;
    uint32_t lastSendMs;
    uint8_t attempts;
};
static ProbeRetry probeRetries[ClusterJobs::kMaxJobs] = {};
static uint32_t probeRetransmissions = 0;
static uint32_t probeJobsStarted = 0; // Counts successful job registration, independent of bounded snapshot.
static const uint32_t PROBE_RETRY_INTERVAL_MS = 3000UL;
static const uint8_t PROBE_MAX_ATTEMPTS = 3; // initial + two retransmissions

// Non-secret, volatile diagnostics for the harmless job probe transport.
static uint32_t probeReceived = 0, probeAckSent = 0, probeAckReceived = 0;
static uint32_t probeRejected = 0, probeAckRejected = 0;
static const char *probeLastReject = "none";
static const char *probeAckLastReject = "none";
static void rejectProbe(const char *reason) { ++probeRejected; probeLastReject = reason; }
static void rejectProbeAck(const char *reason) { ++probeAckRejected; probeAckLastReject = reason; }

static const uint32_t CLUSTER_TIME_INTERVAL_MS = 900000UL; // 15 minutes
static const uint32_t CLUSTER_TIME_RETRY_MS = 30000UL; // only before first valid sample
static const uint32_t CLUSTER_TIME_HOLDOVER_MS = 1800000UL; // max 30 minutes without fresh sample
static const int64_t CLUSTER_TIME_MAX_RTT_US = 250000LL;

enum CoordinatorPolicy : uint8_t {
    COORDINATOR_POLICY_NODE_ONLY = 0,
    COORDINATOR_POLICY_AUTO = 1,
    COORDINATOR_POLICY_PREFERRED = 2
};

struct ClusterPeer {
    bool used;
    char integrationId[36];
    char hostname[64];
    char mode[16];
    char release[32];
    IPAddress ip;
    uint32_t bootNonce;
    uint32_t sequence;
    uint32_t uptimeSeconds;
    int64_t epochSeconds;
    bool timeValid;
    bool recording;
    bool streaming;
    bool transportMode;
    uint32_t leaseSeconds;
    int32_t wifiRemainingSeconds;
    CoordinatorPolicy coordinatorPolicy;
    uint32_t coordinationCapabilities;
    char advertisedCoordinatorId[36];
    uint32_t advertisedCoordinatorEpoch;
    uint32_t coordinationSequence;
    uint32_t lastCoordinatorInfoMs;
    uint32_t directHeartbeatSequence;
    uint32_t lastDirectHeartbeatMs;
    uint32_t lastSeenMs;
    // Latest authenticated time report from this peer (volatile only).
    bool healthValid;
    uint32_t healthUpdatedMs;
    uint32_t sdTotalMiB;
    uint32_t sdUsedMiB;
    int32_t cpuTempDeciC;
    uint32_t freeHeapBytes;
    bool timeReportValid;
    uint32_t timeReportMs;
    uint32_t timeReportEpoch;
    uint32_t timeReportCommandSeq;
    uint32_t selectedSyncSeq;
    uint32_t selectedSyncRequestedMs;
    int64_t timeReportUtcUs;
    int64_t timeReportOffsetUs;
    int64_t timeReportRttUs;
    int64_t timeReportUncertaintyUs;
};

struct ClusterResource {
    bool used;
    char ownerIntegrationId[36];
    char resourceId[49];
    char resourceType[25];
    char locator[161];
    IPAddress sourceIp;
    int64_t timestampUs;
    uint32_t sizeBytes;
    uint32_t ttlSeconds;
    uint32_t lastSeenMs;
};

struct ClusterDiscoveryNode {
    bool used;
    char clusterId[36];
    char integrationId[36];
    char clusterName[64];
    uint32_t credentialEpoch;
    CoordinatorPolicy coordinatorPolicy;
    bool coordinator;
    char release[32];
    IPAddress sourceIp;
    uint32_t lastSeenMs;
};

// Short-lived proof cache for passive join validation. Only already-existing
// signed SFC1 Presence datagrams are retained, never passwords or plaintext
// cluster keys. Entries expire with the discovery lease and are RAM-only.
struct ClusterDiscoveryProof {
    bool used;
    IPAddress sourceIp;
    uint16_t packetLength;
    uint32_t lastSeenMs;
    char packet[CLUSTER_PACKET_MAX + 1];
};

static WiFiUDP clusterUdp;
static WiFiUDP coordinatorUdp;
static ClusterPeer peers[CLUSTER_MAX_PEERS] = {};
static ClusterResource resources[CLUSTER_MAX_RESOURCES] = {};
static ClusterDiscoveryNode discoveryNodes[CLUSTER_MAX_DISCOVERY_NODES] = {};
static ClusterDiscoveryProof discoveryProofs[CLUSTER_DISCOVERY_PROOF_SLOTS] = {};
static bool runtimeConfiguredEnabled = false;
static bool runtimeActive = false;
static bool mdnsServiceAdvertised = false;
static bool discoverySocketActive = false;
static uint32_t discoveryScanLeaseUntilMs = 0;
static bool runtimeProfileRemembered = false;
static uint32_t bootNonce = 0;
static uint32_t sequenceNumber = 0;
static uint32_t nextHeartbeatMs = 0;
static uint32_t nextCoordinatorHeartbeatMs = 0;
static uint32_t directHeartbeatSequence = 0;
static uint32_t lastCoordinatorAckMs = 0;
static uint32_t lastAckCoordinatorBoot = 0;
static uint32_t lastCoordinatorAckSequence = 0;
// Volatile time authority. A future IR backend can provide an independent
// high-precision clock source without changing the existing wall clock.
static uint32_t timeRequestSeq = 0;
static uint32_t timePendingSeq = 0;
static int64_t timePendingMonoUs = 0;
static uint32_t timeLastRequestMs = 0;
static bool timeEstimateValid = false;
static int64_t timeAnchorUtcUs = 0;
static int64_t timeAnchorMonoUs = 0;
static int64_t timeLastRttUs = 0;
static uint32_t timeLastSampleMs = 0;
static char timeAuthorityId[36] = {};
static uint32_t timeAuthorityEpoch = 0;
static uint32_t timeAcceptedSamples = 0;
static uint32_t timeRejectedSamples = 0;
// Coordinator-only manual sync: volatile monotonic command IDs per boot/epoch.
static uint32_t timeSyncCommandSeq = 0;
static uint32_t timeSyncLastReceivedSeq = 0;
static uint32_t timeSyncLastCoordinatorBoot = 0;
static uint32_t timeSyncLastCommandMs = 0;
static uint32_t timeSyncCommandsReceived = 0;
static uint32_t timeSyncCommandsSent = 0;

static void resetClusterTimeEstimate()
{
    timeLastRequestMs = 0; // new coordinator/epoch: synchronize immediately
    timeSyncLastReceivedSeq = 0;
    timeSyncLastCoordinatorBoot = 0;
    timePendingSeq = 0;
    timeEstimateValid = false;
    timeAnchorUtcUs = 0;
    timeAnchorMonoUs = 0;
    timeLastRttUs = 0;
    timeLastSampleMs = 0;
    timeAuthorityId[0] = '\0';
    timeAuthorityEpoch = 0;
}

static uint32_t diagCoordinatorChanges = 0;
static uint32_t diagCoordinatorEpochChanges = 0;
static uint32_t diagAckContextResets = 0;
static uint32_t diagIncompleteCoordinatorAnnouncements = 0;
// Bounded, RAM-only election trace. Contains no cluster secret or key material.
static constexpr size_t CLUSTER_ELECTION_TRACE_COUNT = 8;
static char diagElectionTrace[CLUSTER_ELECTION_TRACE_COUNT][196] = {};
static uint8_t diagElectionTraceNext = 0;
static uint8_t diagElectionTraceUsed = 0;

// Volatile diagnostic counters; never persisted and no secret data exposed.
static uint32_t diagHbSent = 0;
static uint32_t diagHbSendFailed = 0;
static uint32_t diagAckSendAttempted = 0;
static uint32_t diagAckSendFailed = 0;
static uint32_t diagAckAuthenticated = 0;
static uint32_t diagAckAccepted = 0;
static uint32_t diagAckRejected = 0;
static const char *diagAckLastReject = "none";

static int32_t localWifiRemainingSeconds = -1;
static CoordinatorPolicy runtimeCoordinatorPolicy = COORDINATOR_POLICY_AUTO;
static bool localIsCoordinator = false;
static uint32_t localCoordinatorEpoch = 0;
static char currentCoordinatorId[36] = {};
static IPAddress currentCoordinatorIp;
static uint32_t currentCoordinatorEpoch = 0;
// Coordinator-only one-shot diagnostic session. Never shares capture jobs.
struct HwTimeSession {
    bool active = false;
    int64_t targetUtcUs = 0;
    int64_t referenceMonoUs = 0;
    uint32_t epoch = 0;
    uint32_t startedMs = 0;
    uint32_t seq[3] = {};
    uint8_t armStatus[3] = {}; // 0 pending, 1 armed, 2 rejected
    String nodes[3];
};
static HwTimeSession hwSession;
static ClusterHwTimeSeries::History hwHistory;
static String hwSessionLastResult = "not_started";
static uint32_t hwSessionCounter = 0;
static void hwTimeSessionFinish(const char *result, bool saveEdges) {
    if (!hwSession.active) return;
    if (saveEdges) {
        ClusterHwTimeCapture::Edges edges = {};
        ClusterHwTimeGpioLease::snapshot(edges);
        ClusterHwTimeMath::Sample sample = {};
        sample.referenceUs = hwSession.referenceMonoUs;
        for (int i = 0; i < 3; ++i) {
            sample.nodeUs[i] = edges.firstRiseUs[i];
            sample.present[i] = edges.received[i] && hwSession.armStatus[i] == 1;
        }
        hwHistory.add(++hwSessionCounter, sample);
    }
    ClusterHwTimeGpioLease::release();
    hwSession.active = false;
    hwSessionLastResult = result;
}
static void hwTimeSessionService() {
    if (!hwSession.active) return;
    if (!runtimeActive || !localIsCoordinator || !droneModeEnabled() ||
        currentCoordinatorEpoch != hwSession.epoch) {
        hwTimeSessionFinish("aborted_mode_or_epoch", false);
        return;
    }
    const int64_t now = esp_timer_get_time();
    // Reserve enough time for the 2-ms pulses, UART/UDP jitter and ISR handling.
    if (now >= hwSession.referenceMonoUs + 250000LL) {
        hwTimeSessionFinish("complete", true);
    }
}


static bool coordinationDirty = false;
static psa_key_id_t runtimeHmacKey = 0;
static char runtimeClusterTag[17] = {};
static char runtimeClusterId[36] = {};
static uint32_t runtimeCredentialEpoch = 1;
static String runtimeClusterName;
static String runtimeClusterPassword;
static String lastError;

static void secureWipe(void *pointer, size_t length)
{
    if (!pointer)
        return;

    volatile uint8_t *p = static_cast<volatile uint8_t *>(pointer);
    while (length--)
        *p++ = 0;
}

static char hexDigit(uint8_t value)
{
    return value < 10 ? char('0' + value) : char('a' + (value - 10));
}

static int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void bytesToHex(const uint8_t *bytes, size_t length, char *output)
{
    for (size_t i = 0; i < length; ++i) {
        output[i * 2] = hexDigit((bytes[i] >> 4) & 0x0F);
        output[i * 2 + 1] = hexDigit(bytes[i] & 0x0F);
    }
    output[length * 2] = '\0';
}

static String textToHex(const String &value)
{
    static const char HEX_DIGITS[] = "0123456789abcdef";
    String out;
    out.reserve(value.length() * 2);
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t c = (uint8_t)value[i];
        out += HEX_DIGITS[(c >> 4) & 0x0F];
        out += HEX_DIGITS[c & 0x0F];
    }
    return out;
}

static bool hexToText(const char *text, String &value, size_t maxBytes)
{
    value = "";
    if (!text)
        return false;

    const size_t length = strlen(text);
    if ((length & 1U) != 0 || length / 2 > maxBytes)
        return false;

    value.reserve(length / 2);
    for (size_t i = 0; i < length; i += 2) {
        const int hi = hexValue(text[i]);
        const int lo = hexValue(text[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        const char c = (char)((hi << 4) | lo);
        if (c == '\0' || c == '\r' || c == '\n')
            return false;
        value += c;
    }
    return true;
}

static String legacyClusterIdFromName(const String &clusterName)
{
    if (!clusterName.length() || psa_crypto_init() != PSA_SUCCESS)
        return String();

    String seed = "SensorForgeClusterIdV1|" + clusterName;
    uint8_t hash[32] = {};
    size_t outputLength = 0;
    if (psa_hash_compute(
            PSA_ALG_SHA_256,
            reinterpret_cast<const uint8_t *>(seed.c_str()),
            seed.length(),
            hash,
            sizeof(hash),
            &outputLength
        ) != PSA_SUCCESS || outputLength != sizeof(hash)) {
        secureWipe(hash, sizeof(hash));
        return String();
    }

    char text[3 + 32 + 1];
    text[0] = 'c';
    text[1] = 'l';
    text[2] = '-';
    bytesToHex(hash, 16, text + 3);
    secureWipe(hash, sizeof(hash));
    return String(text);
}

static bool hexToBytes(const char *text, size_t byteLength, uint8_t *output)
{
    if (!text || strlen(text) != byteLength * 2)
        return false;

    for (size_t i = 0; i < byteLength; ++i) {
        int hi = hexValue(text[i * 2]);
        int lo = hexValue(text[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        output[i] = (uint8_t)((hi << 4) | lo);
    }

    return true;
}

static bool deriveClusterKey(
    const String &clusterId,
    const String &password,
    uint8_t key[32],
    char tag[17]
)
{
    if (!key || !tag || !clusterIdValid(clusterId) || !password.length())
        return false;

    if (psa_crypto_init() != PSA_SUCCESS)
        return false;

    // Beta 38 binds the trust domain to the stable Cluster ID rather than the
    // human-readable name. The name may later be renamed without changing the
    // Cluster identity; password rotation keeps the ID but changes the key/tag.
    String seed;
    seed.reserve(clusterId.length() + password.length() + 40);
    seed = "SensorForgeClusterKeyV2|";
    seed += clusterId;
    seed += '|';
    seed += password;

    size_t outputLength = 0;
    psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256,
        reinterpret_cast<const uint8_t *>(seed.c_str()),
        seed.length(),
        key,
        32,
        &outputLength
    );

    if (status != PSA_SUCCESS || outputLength != 32)
        return false;

    // Public selector only. It is not a membership authenticator; SFD1 uses
    // the stable Cluster ID directly and never exposes this tag to scanners as
    // a password verifier.
    uint8_t tagHash[32] = {};
    seed = "SensorForgeClusterTagV2|";
    seed += clusterId;
    seed += '|';
    seed += password;

    outputLength = 0;
    status = psa_hash_compute(
        PSA_ALG_SHA_256,
        reinterpret_cast<const uint8_t *>(seed.c_str()),
        seed.length(),
        tagHash,
        sizeof(tagHash),
        &outputLength
    );

    if (status != PSA_SUCCESS || outputLength != sizeof(tagHash)) {
        secureWipe(tagHash, sizeof(tagHash));
        secureWipe(key, 32);
        return false;
    }

    bytesToHex(tagHash, 8, tag);
    secureWipe(tagHash, sizeof(tagHash));
    return true;
}

static bool importRuntimeHmacKey(const uint8_t key[32])
{
    if (runtimeHmacKey != 0) {
        psa_destroy_key(runtimeHmacKey);
        runtimeHmacKey = 0;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256);

    psa_status_t status = psa_import_key(
        &attributes,
        key,
        32,
        &runtimeHmacKey
    );

    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS && runtimeHmacKey != 0;
}

static bool computePacketMac(
    const uint8_t *data,
    size_t length,
    uint8_t mac[32]
)
{
    if (!runtimeHmacKey || (!data && length) || !mac)
        return false;

    size_t outputLength = 0;
    psa_status_t status = psa_mac_compute(
        runtimeHmacKey,
        PSA_ALG_HMAC(PSA_ALG_SHA_256),
        data,
        length,
        mac,
        32,
        &outputLength
    );

    return status == PSA_SUCCESS && outputLength == 32;
}

static bool computePacketMacWithRawKey(
    const uint8_t key[32],
    const uint8_t *data,
    size_t length,
    uint8_t mac[32]
)
{
    if (!key || (!data && length) || !mac)
        return false;

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256);

    psa_key_id_t keyId = 0;
    psa_status_t status = psa_import_key(&attributes, key, 32, &keyId);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS || keyId == 0)
        return false;

    size_t outputLength = 0;
    status = psa_mac_compute(
        keyId,
        PSA_ALG_HMAC(PSA_ALG_SHA_256),
        data,
        length,
        mac,
        32,
        &outputLength
    );
    psa_destroy_key(keyId);

    return status == PSA_SUCCESS && outputLength == 32;
}

static bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t length)
{
    if (!a || !b)
        return false;

    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i)
        difference |= (uint8_t)(a[i] ^ b[i]);
    return difference == 0;
}

// ESP-NOW time packets use a domain-separated key rather than the normal
// cluster packet MAC directly. No raw cluster secret leaves this translation.
static bool espNowTimeMac(const uint8_t *data, size_t len, uint8_t mac[32])
{
    if (!runtimeActive || !runtimeHmacKey || !data || !mac ||
        len != EspNowTimePackets::kPayloadSize) return false;
    static const uint8_t context[] = "SensorForge/ESP-NOW/time/HMAC/v1";
    uint8_t subkey[32] = {};
    if (!computePacketMac(context, sizeof(context) - 1, subkey)) return false;
    const bool ok = computePacketMacWithRawKey(subkey, data, len, mac);
    secureWipe(subkey, sizeof(subkey));
    return ok;
}

// Diagnostics-only 80-byte broadcaster frames. A separate derived key and
// non-production frame size prevent the existing 76-byte clock RX path from
// accepting these as time authority messages.
static bool diagnosticBeaconMac(const uint8_t *data, size_t len, uint8_t mac[32])
{
    if (!runtimeActive || !runtimeHmacKey || !data || !mac || len != 48) return false;
    static const uint8_t context[] = "SensorForge/ESP-NOW/diagnostic-beacon/HMAC/v1";
    uint8_t subkey[32] = {};
    if (!computePacketMac(context, sizeof(context) - 1, subkey)) return false;
    const bool ok = computePacketMacWithRawKey(subkey, data, len, mac);
    secureWipe(subkey, sizeof(subkey));
    return ok;
}

// Separate HMAC key for SF-TG grant/ack/revoke protocol. No radio action is
// enabled by this adapter. Never reuse the production time-beacon subkey.
static bool grantProtocolMac(const uint8_t *data, size_t len, uint8_t mac[32])
{
    if (!runtimeActive || !runtimeHmacKey || !data || !mac ||
        len != ClusterTimeGrantProtocol::kBodyBytes) return false;
    static const uint8_t context[] = "SensorForge/ESP-NOW/time-grant/HMAC/v1";
    uint8_t subkey[32] = {};
    if (!computePacketMac(context, sizeof(context) - 1, subkey)) return false;
    const bool ok = computePacketMacWithRawKey(subkey, data, len, mac);
    secureWipe(subkey, sizeof(subkey));
    return ok;
}

static bool usableIp(const IPAddress &ip)
{
    return ip[0] || ip[1] || ip[2] || ip[3];
}

static String jsonEscape(const String &value)
{
    String output;
    output.reserve(value.length() + 12);
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        switch (c) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if ((uint8_t)c >= 0x20U)
                    output += c;
                break;
        }
    }
    return output;
}

static String safePacketText(const String &value, size_t maxLength)
{
    String output;
    output.reserve(value.length());
    for (size_t i = 0; i < value.length() && output.length() < maxLength; ++i) {
        const char c = value[i];
        if (c == '|' || c == '\r' || c == '\n' || (uint8_t)c < 0x20U)
            output += '_';
        else
            output += c;
    }
    return output;
}

static bool packetFieldValid(
    const String &value,
    size_t maxLength,
    bool allowEmpty
)
{
    if ((!allowEmpty && value.length() == 0) || value.length() > maxLength)
        return false;

    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (c == '|' || c == '\r' || c == '\n' || (uint8_t)c < 0x20U)
            return false;
    }

    return true;
}

static bool parseUint32Strict(const char *text, uint32_t &value, int base = 10)
{
    if (!text || !text[0])
        return false;

    errno = 0;
    char *end = nullptr;
    unsigned long parsed = strtoul(text, &end, base);
    if (errno == ERANGE || !end || *end != '\0')
        return false;

    value = (uint32_t)parsed;
    return true;
}

static bool parseInt64Strict(const char *text, int64_t &value)
{
    if (!text || !text[0])
        return false;

    errno = 0;
    char *end = nullptr;
    long long parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0')
        return false;

    value = (int64_t)parsed;
    return true;
}

static bool parseInt32Strict(const char *text, int32_t &value)
{
    if (!text || !text[0])
        return false;

    errno = 0;
    char *end = nullptr;
    long parsed = strtol(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' ||
        parsed < INT32_MIN || parsed > INT32_MAX) {
        return false;
    }

    value = (int32_t)parsed;
    return true;
}

static bool clusterIdCStringValid(const char *id)
{
    if (!id || strlen(id) != 35 || id[0] != 'c' || id[1] != 'l' || id[2] != '-')
        return false;

    for (size_t i = 3; i < 35; ++i) {
        if (hexValue(id[i]) < 0 || (id[i] >= 'A' && id[i] <= 'F'))
            return false;
    }
    return true;
}

static bool integrationIdValid(const char *id)
{
    if (!id || strlen(id) != 35 || id[0] != 's' || id[1] != 'f' || id[2] != '-')
        return false;

    for (size_t i = 3; i < 35; ++i) {
        if (hexValue(id[i]) < 0)
            return false;
    }
    return true;
}

static CoordinatorPolicy coordinatorPolicyFromString(const String &value)
{
    if (value == "preferred")
        return COORDINATOR_POLICY_PREFERRED;
    if (value == "node")
        return COORDINATOR_POLICY_NODE_ONLY;
    return COORDINATOR_POLICY_AUTO;
}

static const char *coordinatorPolicyName(CoordinatorPolicy policy)
{
    switch (policy) {
        case COORDINATOR_POLICY_PREFERRED: return "preferred";
        case COORDINATOR_POLICY_NODE_ONLY: return "node";
        case COORDINATOR_POLICY_AUTO:
        default: return "auto";
    }
}

static bool coordinatorCandidateBetter(
    CoordinatorPolicy candidatePolicy,
    const char *candidateId,
    CoordinatorPolicy currentPolicy,
    const char *currentId
)
{
    if (candidatePolicy == COORDINATOR_POLICY_NODE_ONLY || !candidateId)
        return false;

    if (!currentId || !currentId[0])
        return true;

    if ((uint8_t)candidatePolicy != (uint8_t)currentPolicy)
        return (uint8_t)candidatePolicy > (uint8_t)currentPolicy;

    // Stable tie-breaker. Every node has the same integration_id ordering, so
    // all healthy peers converge on the same candidate without user-assigned
    // numeric priorities.
    return strcmp(candidateId, currentId) < 0;
}

static uint32_t makeCoordinatorEpoch()
{
    uint32_t value = esp_random();
    return value ? value : 1U;
}

static void clearPeers()
{
    for (size_t i = 0; i < CLUSTER_MAX_PEERS; ++i)
        peers[i] = ClusterPeer{};
}

static void clearResources()
{
    for (size_t i = 0; i < CLUSTER_MAX_RESOURCES; ++i)
        resources[i] = ClusterResource{};
}

static void clearDiscoveryNodes()
{
    for (size_t i = 0; i < CLUSTER_MAX_DISCOVERY_NODES; ++i)
        discoveryNodes[i] = ClusterDiscoveryNode{};
}

static ClusterDiscoveryNode *discoverySlotFor(
    const char *clusterId,
    const char *integrationId,
    uint32_t now
)
{
    ClusterDiscoveryNode *unused = nullptr;
    ClusterDiscoveryNode *oldest = nullptr;
    uint32_t oldestAge = 0;

    for (size_t i = 0; i < CLUSTER_MAX_DISCOVERY_NODES; ++i) {
        ClusterDiscoveryNode &node = discoveryNodes[i];
        if (node.used &&
            strcmp(node.clusterId, clusterId) == 0 &&
            strcmp(node.integrationId, integrationId) == 0) {
            return &node;
        }
        if (!node.used && !unused)
            unused = &node;
        if (node.used) {
            const uint32_t age = now - node.lastSeenMs;
            if (!oldest || age > oldestAge) {
                oldest = &node;
                oldestAge = age;
            }
        }
    }

    return unused ? unused : oldest;
}

static void expireDiscoveryNodes(uint32_t now)
{
    for (size_t i = 0; i < CLUSTER_MAX_DISCOVERY_NODES; ++i) {
        ClusterDiscoveryNode &node = discoveryNodes[i];
        if (node.used &&
            (uint32_t)(now - node.lastSeenMs) > CLUSTER_DISCOVERY_NODE_TTL_MS) {
            node = ClusterDiscoveryNode{};
        }
    }
}

static void clearDiscoveryProofs()
{
    for (size_t i = 0; i < CLUSTER_DISCOVERY_PROOF_SLOTS; ++i)
        discoveryProofs[i] = ClusterDiscoveryProof{};
}

static void expireDiscoveryProofs(uint32_t now)
{
    for (size_t i = 0; i < CLUSTER_DISCOVERY_PROOF_SLOTS; ++i) {
        ClusterDiscoveryProof &proof = discoveryProofs[i];
        if (proof.used &&
            (uint32_t)(now - proof.lastSeenMs) > CLUSTER_DISCOVERY_PROOF_TTL_MS) {
            proof = ClusterDiscoveryProof{};
        }
    }
}

static ClusterDiscoveryProof *discoveryProofSlotFor(
    const IPAddress &sourceIp,
    uint32_t now
)
{
    ClusterDiscoveryProof *unused = nullptr;
    ClusterDiscoveryProof *oldest = nullptr;
    uint32_t oldestAge = 0;

    for (size_t i = 0; i < CLUSTER_DISCOVERY_PROOF_SLOTS; ++i) {
        ClusterDiscoveryProof &proof = discoveryProofs[i];
        if (proof.used && proof.sourceIp == sourceIp)
            return &proof;
        if (!proof.used && !unused)
            unused = &proof;
        if (proof.used) {
            const uint32_t age = now - proof.lastSeenMs;
            if (!oldest || age > oldestAge) {
                oldest = &proof;
                oldestAge = age;
            }
        }
    }

    return unused ? unused : oldest;
}

static void rememberDiscoveryPresenceProof(
    const char *packet,
    size_t packetLength,
    const IPAddress &sourceIp,
    uint32_t now
)
{
    if (!packet ||
        packetLength < 5 || packetLength > CLUSTER_PACKET_MAX ||
        memcmp(packet, "SFC1|", 5) != 0) {
        return;
    }

    ClusterDiscoveryProof *proof = discoveryProofSlotFor(sourceIp, now);
    if (!proof)
        return;

    *proof = ClusterDiscoveryProof{};
    proof->used = true;
    proof->sourceIp = sourceIp;
    proof->packetLength = (uint16_t)packetLength;
    proof->lastSeenMs = now;
    memcpy(proof->packet, packet, packetLength);
    proof->packet[packetLength] = '\0';
}

static ClusterPeer *peerSlotFor(const char *integrationId, uint32_t now)
{
    ClusterPeer *unused = nullptr;
    ClusterPeer *oldest = nullptr;
    uint32_t oldestAge = 0;

    for (size_t i = 0; i < CLUSTER_MAX_PEERS; ++i) {
        ClusterPeer &peer = peers[i];
        if (peer.used && strcmp(peer.integrationId, integrationId) == 0)
            return &peer;

        if (!peer.used && !unused)
            unused = &peer;

        if (peer.used) {
            uint32_t age = now - peer.lastSeenMs;
            if (!oldest || age > oldestAge) {
                oldest = &peer;
                oldestAge = age;
            }
        }
    }

    if (unused)
        return unused;

    return oldest;
}

static uint32_t clampSeconds(
    uint32_t value,
    uint32_t minimum,
    uint32_t maximum
)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static uint32_t peerLeaseSeconds(const ClusterPeer &peer)
{
    if (peer.leaseSeconds == 0)
        return CLUSTER_DEFAULT_LEASE_SEC;

    return clampSeconds(
        peer.leaseSeconds,
        20U,
        CLUSTER_RESOURCE_TTL_MAX_SEC
    );
}

static void expirePeers(uint32_t now)
{
    for (size_t i = 0; i < CLUSTER_MAX_PEERS; ++i) {
        ClusterPeer &peer = peers[i];
        if (!peer.used)
            continue;

        uint32_t leaseSeconds =
            peer.leaseSeconds > 0
            ? peerLeaseSeconds(peer)
            : (CLUSTER_LEGACY_PEER_TIMEOUT_MS / 1000UL);

        uint64_t leaseMs = (uint64_t)leaseSeconds * 1000ULL;
        if ((uint64_t)(uint32_t)(now - peer.lastSeenMs) > leaseMs) {
            char expiredIntegrationId[sizeof(peer.integrationId)] = {};
            strncpy(
                expiredIntegrationId,
                peer.integrationId,
                sizeof(expiredIntegrationId) - 1
            );
            peer = ClusterPeer{};

            for (size_t r = 0; r < CLUSTER_MAX_RESOURCES; ++r) {
                if (
                    resources[r].used &&
                    strcmp(
                        resources[r].ownerIntegrationId,
                        expiredIntegrationId
                    ) == 0
                ) {
                    resources[r] = ClusterResource{};
                }
            }
        }
    }
}

static void expireResources(uint32_t now)
{
    for (size_t i = 0; i < CLUSTER_MAX_RESOURCES; ++i) {
        ClusterResource &resource = resources[i];
        if (!resource.used)
            continue;

        uint32_t ttlSeconds = clampSeconds(
            resource.ttlSeconds,
            1U,
            CLUSTER_RESOURCE_TTL_MAX_SEC
        );

        if ((uint64_t)(uint32_t)(now - resource.lastSeenMs) >
            (uint64_t)ttlSeconds * 1000ULL) {
            resource = ClusterResource{};
        }
    }
}

static ClusterPeer *findPeer(const char *integrationId)
{
    if (!integrationId)
        return nullptr;

    for (size_t i = 0; i < CLUSTER_MAX_PEERS; ++i) {
        if (peers[i].used && strcmp(peers[i].integrationId, integrationId) == 0)
            return &peers[i];
    }

    return nullptr;
}

// Standby broadcaster is a management peer, not a recording/sync/SD node.
// The authenticated legacy heartbeat carries at most 15 characters of mode.
// Do not change that signed packet format in this compatibility release.
static bool peerIsStandbyTimeBroadcaster(const ClusterPeer *peer)
{
    if (!peer || !peer->used) return false;
    return strcmp(peer->mode, "time_broadcaste") == 0 ||
           strcmp(peer->mode, "time_broadcaster") == 0;
}

static bool rejectStandbyNodeAction(const ClusterPeer *peer, String &error)
{
    if (!peerIsStandbyTimeBroadcaster(peer)) return false;
    error = "Zeit-Broadcaster (Standby): diese Node-Aktion ist gesperrt.";
    return true;
}

static ClusterPeer *ensurePeerForAuthenticatedPacket(
    const char *integrationId,
    uint32_t remoteBoot,
    const IPAddress &remoteIp,
    uint32_t now
)
{
    if (!integrationIdValid(integrationId))
        return nullptr;

    ClusterPeer *peer = findPeer(integrationId);
    if (!peer) {
        peer = peerSlotFor(integrationId, now);
        if (!peer)
            return nullptr;
        *peer = ClusterPeer{};
        peer->used = true;
        strncpy(peer->integrationId, integrationId, sizeof(peer->integrationId) - 1);
        peer->coordinatorPolicy = COORDINATOR_POLICY_NODE_ONLY;
        peer->leaseSeconds = CLUSTER_DEFAULT_LEASE_SEC;
        peer->wifiRemainingSeconds = -1;
    }

    if (peer->bootNonce != 0 && peer->bootNonce != remoteBoot) {
        *peer = ClusterPeer{};
        peer->used = true;
        strncpy(peer->integrationId, integrationId, sizeof(peer->integrationId) - 1);
        peer->coordinatorPolicy = COORDINATOR_POLICY_NODE_ONLY;
        peer->leaseSeconds = CLUSTER_DEFAULT_LEASE_SEC;
        peer->wifiRemainingSeconds = -1;
    }

    peer->bootNonce = remoteBoot;
    peer->ip = remoteIp;
    return peer;
}

static void recomputeCoordinator(uint32_t now)
{
    const char *bestId = nullptr;
    CoordinatorPolicy bestPolicy = COORDINATOR_POLICY_NODE_ONLY;
    ClusterPeer *bestPeer = nullptr;

    if (cfg_operating_mode != "time_broadcaster" &&
        runtimeCoordinatorPolicy != COORDINATOR_POLICY_NODE_ONLY) {
        bestId = deviceIntegrationId().c_str();
        bestPolicy = runtimeCoordinatorPolicy;
    }

    for (size_t i = 0; i < CLUSTER_MAX_PEERS; ++i) {
        ClusterPeer &peer = peers[i];
        if (!peer.used || peer.coordinatorPolicy == COORDINATOR_POLICY_NODE_ONLY)
            continue;

        const uint32_t leaseSeconds = peerLeaseSeconds(peer);
        // A packet handled later in this loop may have a lastSeenMs newer
        // than the loop-start timestamp. Treat a small negative age as zero,
        // not as a wrapped 49-day-old lease.
        const int32_t signedAgeMs = (int32_t)(now - peer.lastSeenMs);
        const uint32_t ageMs = signedAgeMs < 0 ? 0U : (uint32_t)signedAgeMs;
        if ((uint64_t)ageMs > (uint64_t)leaseSeconds * 1000ULL)
            continue;

        if (coordinatorCandidateBetter(
                peer.coordinatorPolicy,
                peer.integrationId,
                bestPolicy,
                bestId
            )) {
            bestId = peer.integrationId;
            bestPolicy = peer.coordinatorPolicy;
            bestPeer = &peer;
        }
    }

    char newCoordinatorId[36] = {};
    IPAddress newCoordinatorIp;
    uint32_t newCoordinatorEpoch = 0;
    bool newLocalIsCoordinator = false;

    if (bestId && bestId[0]) {
        strncpy(newCoordinatorId, bestId, sizeof(newCoordinatorId) - 1);
        newLocalIsCoordinator =
            strcmp(newCoordinatorId, deviceIntegrationId().c_str()) == 0;

        if (newLocalIsCoordinator) {
            if (!localIsCoordinator || localCoordinatorEpoch == 0)
                localCoordinatorEpoch = makeCoordinatorEpoch();
            newCoordinatorEpoch = localCoordinatorEpoch;
        } else if (bestPeer) {
            newCoordinatorIp = bestPeer->ip;
            if (
                strcmp(bestPeer->advertisedCoordinatorId, bestPeer->integrationId) == 0 &&
                bestPeer->advertisedCoordinatorEpoch != 0
            ) {
                newCoordinatorEpoch = bestPeer->advertisedCoordinatorEpoch;
            }
            // An authenticated coordination message may temporarily carry no
            // Coordinator epoch while the peer is converging. Retain the last
            // ACK-confirmed epoch only for the SAME live Coordinator/boot.
            // Never carry it across a peer reboot or a Coordinator change.
            else if (strcmp(currentCoordinatorId, bestPeer->integrationId) == 0 &&
                     currentCoordinatorEpoch != 0 &&
                     lastCoordinatorAckMs != 0 &&
                     (uint32_t)(now - lastCoordinatorAckMs) <=
                         CLUSTER_COORDINATOR_HEARTBEAT_MS * 3UL &&
                     bestPeer->bootNonce == lastAckCoordinatorBoot) {
                newCoordinatorEpoch = currentCoordinatorEpoch;
            }
        }
    }

    const bool changed =
        strcmp(currentCoordinatorId, newCoordinatorId) != 0 ||
        currentCoordinatorEpoch != newCoordinatorEpoch ||
        localIsCoordinator != newLocalIsCoordinator;

    if (changed) {
        resetClusterTimeEstimate();
        // Snapshot BEFORE changing state: distinguish loss of an advertised
        // epoch from a real candidate/boot transition. No behavior change.
        char *trace = diagElectionTrace[diagElectionTraceNext];
        const uint32_t oldEpoch = currentCoordinatorEpoch;
        const uint32_t peerEpoch = bestPeer ? bestPeer->advertisedCoordinatorEpoch : 0;
        const bool selfAdvertised = bestPeer &&
            strcmp(bestPeer->advertisedCoordinatorId, bestPeer->integrationId) == 0;
        snprintf(trace, sizeof(diagElectionTrace[0]),
                 "t=%lu %s:%lu>%s:%lu peerBoot=%08lx self=%u adv=%lu ackAge=%ld seq=%lu",
                 (unsigned long)now,
                 currentCoordinatorId[0] ? currentCoordinatorId : "none",
                 (unsigned long)oldEpoch,
                 newCoordinatorId[0] ? newCoordinatorId : "none",
                 (unsigned long)newCoordinatorEpoch,
                 (unsigned long)(bestPeer ? bestPeer->bootNonce : 0),
                 (unsigned int)selfAdvertised,
                 (unsigned long)peerEpoch,
                 lastCoordinatorAckMs ? (long)(uint32_t)(now-lastCoordinatorAckMs) : -1L,
                 (unsigned long)(bestPeer ? bestPeer->coordinationSequence : 0));
        diagElectionTraceNext = (diagElectionTraceNext + 1) % CLUSTER_ELECTION_TRACE_COUNT;
        if (diagElectionTraceUsed < CLUSTER_ELECTION_TRACE_COUNT) ++diagElectionTraceUsed;
        ++diagCoordinatorChanges;
        if (currentCoordinatorEpoch != newCoordinatorEpoch)
            ++diagCoordinatorEpochChanges;
        if (lastCoordinatorAckMs != 0)
            ++diagAckContextResets;
        const bool wasLocalCoordinator = localIsCoordinator;
        strncpy(currentCoordinatorId, newCoordinatorId, sizeof(currentCoordinatorId) - 1);
        currentCoordinatorId[sizeof(currentCoordinatorId) - 1] = '\0';
        currentCoordinatorIp = newCoordinatorIp;
        currentCoordinatorEpoch = newCoordinatorEpoch;
        localIsCoordinator = newLocalIsCoordinator;
        coordinationDirty = true;
        // Keep the direct-HB periodic deadline across election updates.
        // An epoch fluctuation must not trigger a heartbeat burst.
        lastCoordinatorAckMs = 0;
        lastAckCoordinatorBoot = 0;
        lastCoordinatorAckSequence = 0;

        if (wasLocalCoordinator && !localIsCoordinator)
            localCoordinatorEpoch = 0;

        consoleWrite(
            "CLUSTER",
            String("Coordinator ") +
            (currentCoordinatorId[0] ? currentCoordinatorId : "<none>") +
            " | local_role=" + (localIsCoordinator ? "coordinator" : "node") +
            " | epoch=" + String((unsigned long)currentCoordinatorEpoch)
        );
    } else if (!newLocalIsCoordinator && bestPeer) {
        currentCoordinatorIp = bestPeer->ip;
    }
}

static ClusterResource *resourceSlotFor(
    const char *ownerIntegrationId,
    const char *resourceId,
    uint32_t now
)
{
    ClusterResource *unused = nullptr;
    ClusterResource *oldest = nullptr;
    uint32_t oldestAge = 0;

    for (size_t i = 0; i < CLUSTER_MAX_RESOURCES; ++i) {
        ClusterResource &resource = resources[i];

        if (
            resource.used &&
            strcmp(resource.ownerIntegrationId, ownerIntegrationId) == 0 &&
            strcmp(resource.resourceId, resourceId) == 0
        ) {
            return &resource;
        }

        if (!resource.used && !unused)
            unused = &resource;

        if (resource.used) {
            const uint32_t age = now - resource.lastSeenMs;
            if (!oldest || age > oldestAge) {
                oldest = &resource;
                oldestAge = age;
            }
        }
    }

    return unused ? unused : oldest;
}

static bool authenticatePacket(
    char *packet,
    size_t packetLength
)
{
    if (
        !packet ||
        packetLength == 0 ||
        packetLength > CLUSTER_PACKET_MAX ||
        !runtimeActive
    ) {
        return false;
    }

    packet[packetLength] = '\0';

    char *separator = strrchr(packet, '|');
    if (!separator || separator == packet)
        return false;

    const char *signatureText = separator + 1;
    *separator = '\0';

    uint8_t receivedMac[32] = {};
    uint8_t expectedMac[32] = {};

    if (!hexToBytes(signatureText, sizeof(receivedMac), receivedMac))
        return false;

    if (!computePacketMac(
            reinterpret_cast<const uint8_t *>(packet),
            strlen(packet),
            expectedMac
        )) {
        return false;
    }

    return constantTimeEqual(
        receivedMac,
        expectedMac,
        sizeof(receivedMac)
    );
}

static bool splitFields(
    char *packet,
    char **fields,
    size_t fieldCapacity,
    size_t &fieldCount
)
{
    fieldCount = 0;
    if (!packet || !fields || fieldCapacity == 0)
        return false;

    char *save = nullptr;
    char *token = strtok_r(packet, "|", &save);
    while (token && fieldCount < fieldCapacity) {
        fields[fieldCount++] = token;
        token = strtok_r(nullptr, "|", &save);
    }

    return token == nullptr;
}

static bool discoveryProofMatchesCredentials(
    const ClusterDiscoveryProof &proof,
    const ClusterDiscoveryNode &node,
    const uint8_t key[32],
    const char expectedTag[17]
)
{
    if (!proof.used || !node.used || !key || !expectedTag ||
        proof.packetLength == 0 || proof.packetLength > CLUSTER_PACKET_MAX) {
        return false;
    }

    char packet[CLUSTER_PACKET_MAX + 1];
    memcpy(packet, proof.packet, proof.packetLength);
    packet[proof.packetLength] = '\0';

    char *separator = strrchr(packet, '|');
    if (!separator || separator == packet)
        return false;

    const char *signatureText = separator + 1;
    if (strlen(signatureText) != 64)
        return false;

    uint8_t receivedMac[32] = {};
    uint8_t expectedMac[32] = {};
    if (!hexToBytes(signatureText, sizeof(receivedMac), receivedMac))
        return false;

    const size_t payloadLength = (size_t)(separator - packet);
    *separator = '\0';
    if (!computePacketMacWithRawKey(
            key,
            reinterpret_cast<const uint8_t *>(packet),
            payloadLength,
            expectedMac
        )) {
        secureWipe(receivedMac, sizeof(receivedMac));
        secureWipe(expectedMac, sizeof(expectedMac));
        return false;
    }

    const bool macMatches = constantTimeEqual(
        receivedMac,
        expectedMac,
        sizeof(receivedMac)
    );
    secureWipe(receivedMac, sizeof(receivedMac));
    secureWipe(expectedMac, sizeof(expectedMac));
    if (!macMatches)
        return false;

    char *fields[12] = {};
    size_t fieldCount = 0;
    if (!splitFields(packet, fields, 12, fieldCount) || fieldCount != 12)
        return false;

    return strcmp(fields[0], CLUSTER_PACKET_MAGIC) == 0 &&
        strcmp(fields[1], expectedTag) == 0 &&
        strcmp(fields[2], node.integrationId) == 0;
}

static bool sendSignedPayload(
    const String &payload,
    const char *errorContext
)
{
    if (!runtimeActive)
        return false;

    uint8_t mac[32] = {};
    if (!computePacketMac(
            reinterpret_cast<const uint8_t *>(payload.c_str()),
            payload.length(),
            mac
        )) {
        lastError = "cluster HMAC computation failed";
        return false;
    }

    char macText[65];
    bytesToHex(mac, sizeof(mac), macText);

    String packet = payload;
    packet += '|';
    packet += macText;

    if (packet.length() > CLUSTER_PACKET_MAX) {
        lastError =
            String(errorContext ? errorContext : "cluster packet") +
            " exceeded packet limit";
        return false;
    }

    if (!clusterUdp.beginMulticastPacket()) {
        lastError =
            String(errorContext ? errorContext : "cluster packet") +
            " multicast send setup failed";
        return false;
    }

    clusterUdp.write(
        reinterpret_cast<const uint8_t *>(packet.c_str()),
        packet.length()
    );

    if (!clusterUdp.endPacket()) {
        lastError =
            String(errorContext ? errorContext : "cluster packet") +
            " multicast send failed";
        return false;
    }

    return true;
}

static bool sendSignedPayloadTo(
    const String &payload,
    const IPAddress &destination,
    uint16_t port,
    const char *errorContext
)
{
    if (!runtimeActive || !usableIp(destination))
        return false;

    uint8_t mac[32] = {};
    if (!computePacketMac(
            reinterpret_cast<const uint8_t *>(payload.c_str()),
            payload.length(),
            mac
        )) {
        lastError = "cluster HMAC computation failed";
        return false;
    }

    char macText[65];
    bytesToHex(mac, sizeof(mac), macText);

    String packet = payload;
    packet += '|';
    packet += macText;

    if (packet.length() > CLUSTER_PACKET_MAX) {
        lastError =
            String(errorContext ? errorContext : "cluster packet") +
            " exceeded packet limit";
        return false;
    }

    if (!coordinatorUdp.beginPacket(destination, port)) {
        lastError =
            String(errorContext ? errorContext : "cluster packet") +
            " unicast send setup failed";
        return false;
    }

    coordinatorUdp.write(
        reinterpret_cast<const uint8_t *>(packet.c_str()),
        packet.length()
    );

    if (!coordinatorUdp.endPacket()) {
        lastError =
            String(errorContext ? errorContext : "cluster packet") +
            " unicast send failed";
        return false;
    }

    return true;
}

static bool discoveryScanWanted(uint32_t now)
{
    return discoveryScanLeaseUntilMs != 0 &&
        (int32_t)(discoveryScanLeaseUntilMs - now) > 0;
}

static void processDiscoveryDatagram(
    char *packet,
    size_t packetLength,
    const IPAddress &remoteIp
)
{
    const uint32_t now = millis();
    if (!discoveryScanWanted(now) || !packet || packetLength == 0 ||
        packetLength > CLUSTER_PACKET_MAX) {
        return;
    }

    packet[packetLength] = '\0';
    char *fields[8] = {};
    size_t fieldCount = 0;
    if (!splitFields(packet, fields, 8, fieldCount) || fieldCount != 8)
        return;

    if (strcmp(fields[0], CLUSTER_DISCOVERY_MAGIC) != 0 ||
        !clusterIdCStringValid(fields[1]) ||
        !integrationIdValid(fields[3]) ||
        strcmp(fields[3], deviceIntegrationId().c_str()) == 0) {
        return;
    }

    uint32_t credentialEpoch = 0;
    uint32_t policyValue = 0;
    uint32_t coordinatorValue = 0;
    String clusterName;
    if (!parseUint32Strict(fields[2], credentialEpoch) ||
        credentialEpoch == 0 ||
        !hexToText(fields[4], clusterName, 63) ||
        !parseUint32Strict(fields[5], policyValue) ||
        policyValue > (uint32_t)COORDINATOR_POLICY_PREFERRED ||
        !parseUint32Strict(fields[6], coordinatorValue) ||
        coordinatorValue > 1 ||
        strlen(fields[7]) > 31) {
        return;
    }

    ClusterDiscoveryNode *node = discoverySlotFor(fields[1], fields[3], now);
    if (!node)
        return;

    *node = ClusterDiscoveryNode{};
    node->used = true;
    strncpy(node->clusterId, fields[1], sizeof(node->clusterId) - 1);
    strncpy(node->integrationId, fields[3], sizeof(node->integrationId) - 1);
    strncpy(node->clusterName, clusterName.c_str(), sizeof(node->clusterName) - 1);
    node->credentialEpoch = credentialEpoch;
    node->coordinatorPolicy = (CoordinatorPolicy)policyValue;
    node->coordinator = coordinatorValue != 0;
    strncpy(node->release, fields[7], sizeof(node->release) - 1);
    node->sourceIp = remoteIp;
    node->lastSeenMs = now;
}

static void processPresence(
    char **fields,
    size_t fieldCount,
    const IPAddress &remoteIp
)
{
    if (fieldCount != 12)
        return;

    if (strcmp(fields[0], CLUSTER_PACKET_MAGIC) != 0)
        return;
    if (strcmp(fields[1], runtimeClusterTag) != 0)
        return;
    if (!integrationIdValid(fields[2]))
        return;
    if (deviceIntegrationId() == fields[2])
        return;

    uint32_t remoteBoot = 0;
    uint32_t remoteSequence = 0;
    uint32_t remoteUptime = 0;
    uint32_t remoteTimeValid = 0;
    uint32_t remoteFlags = 0;
    int64_t remoteEpoch = 0;

    if (!parseUint32Strict(fields[3], remoteBoot, 16) ||
        !parseUint32Strict(fields[4], remoteSequence) ||
        !parseUint32Strict(fields[5], remoteUptime) ||
        !parseInt64Strict(fields[6], remoteEpoch) ||
        !parseUint32Strict(fields[7], remoteTimeValid) ||
        !parseUint32Strict(fields[9], remoteFlags) ||
        remoteTimeValid > 1 ||
        remoteFlags > 0xFFU ||
        strlen(fields[8]) > 15 ||
        strlen(fields[10]) > 31 ||
        strlen(fields[11]) > 63) {
        return;
    }

    const uint32_t now = millis();
    ClusterPeer *peer = peerSlotFor(fields[2], now);
    if (!peer)
        return;

    if (
        peer->used &&
        peer->bootNonce == remoteBoot &&
        remoteSequence <= peer->sequence
    ) {
        return;
    }

    const bool sameBoot =
        peer->used &&
        peer->bootNonce == remoteBoot;

    // A newly authenticated presence with a different boot ID is observable
    // evidence of a boot change, not proof of why the device rebooted.
    if (peer->used && peer->bootNonce && peer->bootNonce != remoteBoot) {
        for (size_t i = 0; i < restartEvidenceCount; ++i) {
            RestartEvidence &e = restartEvidence[i];
            if (!e.observedNewBoot && e.expectedOldBoot == peer->bootNonce &&
                strcmp(e.nodeId, fields[2]) == 0)
                e.observedNewBoot = remoteBoot;
        }
    }


    const uint32_t preservedLease =
        sameBoot && peer->leaseSeconds
        ? peer->leaseSeconds
        : CLUSTER_DEFAULT_LEASE_SEC;

    const int32_t preservedWifiRemaining =
        sameBoot
        ? peer->wifiRemainingSeconds
        : -1;

    const CoordinatorPolicy preservedCoordinatorPolicy =
        sameBoot
        ? peer->coordinatorPolicy
        : COORDINATOR_POLICY_NODE_ONLY;
    const uint32_t preservedCoordinationCapabilities =
        sameBoot ? peer->coordinationCapabilities : 0;
    char preservedAdvertisedCoordinatorId[36] = {};
    if (sameBoot) {
        strncpy(
            preservedAdvertisedCoordinatorId,
            peer->advertisedCoordinatorId,
            sizeof(preservedAdvertisedCoordinatorId) - 1
        );
    }
    const uint32_t preservedAdvertisedCoordinatorEpoch =
        sameBoot ? peer->advertisedCoordinatorEpoch : 0;
    const uint32_t preservedCoordinationSequence =
        sameBoot ? peer->coordinationSequence : 0;
    const uint32_t preservedLastCoordinatorInfoMs =
        sameBoot ? peer->lastCoordinatorInfoMs : 0;
    const uint32_t preservedDirectHeartbeatSequence =
        sameBoot ? peer->directHeartbeatSequence : 0;
    const uint32_t preservedLastDirectHeartbeatMs =
        sameBoot ? peer->lastDirectHeartbeatMs : 0;
    const bool preservedHealthValid = sameBoot && peer->healthValid;
    const uint32_t preservedHealthUpdatedMs = preservedHealthValid ? peer->healthUpdatedMs : 0;
    const uint32_t preservedSdTotalMiB = preservedHealthValid ? peer->sdTotalMiB : 0;
    const uint32_t preservedSdUsedMiB = preservedHealthValid ? peer->sdUsedMiB : 0;
    const int32_t preservedCpuTempDeciC = preservedHealthValid ? peer->cpuTempDeciC : 0;
    const uint32_t preservedFreeHeapBytes = preservedHealthValid ? peer->freeHeapBytes : 0;
    const bool preservedTimeReportValid = sameBoot && peer->timeReportValid;
    const uint32_t preservedTimeReportMs = preservedTimeReportValid ? peer->timeReportMs : 0;
    const uint32_t preservedTimeReportEpoch = preservedTimeReportValid ? peer->timeReportEpoch : 0;
    const uint32_t preservedTimeReportCommandSeq = preservedTimeReportValid ? peer->timeReportCommandSeq : 0;
    const uint32_t preservedSelectedSyncSeq = sameBoot ? peer->selectedSyncSeq : 0;
    const uint32_t preservedSelectedSyncRequestedMs = sameBoot ? peer->selectedSyncRequestedMs : 0;
    const int64_t preservedTimeReportUtcUs = preservedTimeReportValid ? peer->timeReportUtcUs : 0;
    const int64_t preservedTimeReportOffsetUs = preservedTimeReportValid ? peer->timeReportOffsetUs : 0;
    const int64_t preservedTimeReportRttUs = preservedTimeReportValid ? peer->timeReportRttUs : 0;
    const int64_t preservedTimeReportUncertaintyUs = preservedTimeReportValid ? peer->timeReportUncertaintyUs : 0;

    *peer = ClusterPeer{};
    peer->used = true;
    strncpy(peer->integrationId, fields[2], sizeof(peer->integrationId) - 1);
    strncpy(peer->mode, fields[8], sizeof(peer->mode) - 1);
    strncpy(peer->release, fields[10], sizeof(peer->release) - 1);
    strncpy(peer->hostname, fields[11], sizeof(peer->hostname) - 1);
    peer->ip = remoteIp;
    peer->bootNonce = remoteBoot;
    peer->sequence = remoteSequence;
    peer->uptimeSeconds = remoteUptime;
    peer->epochSeconds = remoteEpoch;
    peer->timeValid = remoteTimeValid != 0;
    peer->recording = (remoteFlags & 0x01U) != 0;
    peer->streaming = (remoteFlags & 0x02U) != 0;
    peer->transportMode = (remoteFlags & 0x04U) != 0;
    peer->leaseSeconds = preservedLease;
    peer->wifiRemainingSeconds = preservedWifiRemaining;
    peer->coordinatorPolicy = preservedCoordinatorPolicy;
    peer->coordinationCapabilities = preservedCoordinationCapabilities;
    strncpy(
        peer->advertisedCoordinatorId,
        preservedAdvertisedCoordinatorId,
        sizeof(peer->advertisedCoordinatorId) - 1
    );
    peer->advertisedCoordinatorEpoch = preservedAdvertisedCoordinatorEpoch;
    peer->coordinationSequence = preservedCoordinationSequence;
    peer->lastCoordinatorInfoMs = preservedLastCoordinatorInfoMs;
    peer->directHeartbeatSequence = preservedDirectHeartbeatSequence;
    peer->lastDirectHeartbeatMs = preservedLastDirectHeartbeatMs;
    peer->healthValid = preservedHealthValid;
    peer->healthUpdatedMs = preservedHealthUpdatedMs;
    peer->sdTotalMiB = preservedSdTotalMiB;
    peer->sdUsedMiB = preservedSdUsedMiB;
    peer->cpuTempDeciC = preservedCpuTempDeciC;
    peer->freeHeapBytes = preservedFreeHeapBytes;
    peer->timeReportValid = preservedTimeReportValid;
    peer->timeReportMs = preservedTimeReportMs;
    peer->timeReportEpoch = preservedTimeReportEpoch;
    peer->timeReportCommandSeq = preservedTimeReportCommandSeq;
    peer->selectedSyncSeq = preservedSelectedSyncSeq;
    peer->selectedSyncRequestedMs = preservedSelectedSyncRequestedMs;
    peer->timeReportUtcUs = preservedTimeReportUtcUs;
    peer->timeReportOffsetUs = preservedTimeReportOffsetUs;
    peer->timeReportRttUs = preservedTimeReportRttUs;
    peer->timeReportUncertaintyUs = preservedTimeReportUncertaintyUs;
    peer->lastSeenMs = now;

    if (!runtimeProfileRemembered) {
        runtimeProfileRemembered = true;
        String rememberError;
        if (!clusterProfilesRemember(
                String(runtimeClusterId),
                runtimeClusterName,
                runtimeClusterPassword,
                runtimeCredentialEpoch,
                rememberError
            ) && rememberError.length()) {
            consoleWrite(
                "CLUSTER",
                "Known-cluster credential cache not updated: " + rememberError
            );
        }
    }
}

static void processMeta(
    char **fields,
    size_t fieldCount
)
{
    // SFM1|tag|integration_id|boot|presence_sequence|lease_sec|wifi_remaining_sec
    if (fieldCount != 7)
        return;

    if (strcmp(fields[0], CLUSTER_META_MAGIC) != 0 ||
        strcmp(fields[1], runtimeClusterTag) != 0 ||
        !integrationIdValid(fields[2]) ||
        deviceIntegrationId() == fields[2]) {
        return;
    }

    uint32_t remoteBoot = 0;
    uint32_t remoteSequence = 0;
    uint32_t leaseSeconds = 0;
    int32_t wifiRemainingSeconds = -1;

    if (!parseUint32Strict(fields[3], remoteBoot, 16) ||
        !parseUint32Strict(fields[4], remoteSequence) ||
        !parseUint32Strict(fields[5], leaseSeconds) ||
        !parseInt32Strict(fields[6], wifiRemainingSeconds) ||
        leaseSeconds < 20 ||
        leaseSeconds > CLUSTER_RESOURCE_TTL_MAX_SEC ||
        wifiRemainingSeconds < -1) {
        return;
    }

    ClusterPeer *peer = findPeer(fields[2]);
    if (!peer || peer->bootNonce != remoteBoot)
        return;

    // Metadata is emitted immediately after its matching presence heartbeat.
    // Accept the same sequence number, but never let older metadata overwrite
    // newer information.
    if (remoteSequence < peer->sequence)
        return;

    peer->leaseSeconds = leaseSeconds;
    peer->wifiRemainingSeconds = wifiRemainingSeconds;
    peer->lastSeenMs = millis();
}

// Optional authenticated health sidecar: no remote actions or secrets.
// SFH1|tag|id|boot|presence_seq|total_mib|used_mib|cpu_temp_deci_c|heap_bytes
static void processHealth(char **fields, size_t count)
{
    if (count != 9 || strcmp(fields[0], CLUSTER_HEALTH_MAGIC) ||
        strcmp(fields[1], runtimeClusterTag) || !integrationIdValid(fields[2])) return;
    uint32_t boot=0,seq=0,total=0,used=0,heap=0;
    int32_t temp=0;
    if (!parseUint32Strict(fields[3],boot,16) ||
        !parseUint32Strict(fields[4],seq) ||
        !parseUint32Strict(fields[5],total) ||
        !parseUint32Strict(fields[6],used) ||
        !parseInt32Strict(fields[7],temp) ||
        !parseUint32Strict(fields[8],heap) ||
        (total && used>total) || total>1048576U || temp < -1000 || temp>2000) return;
    ClusterPeer *peer=findPeer(fields[2]);
    if (!peer || peer->bootNonce!=boot || seq != peer->sequence) return;
    peer->healthValid=true;
    peer->healthUpdatedMs=millis();
    peer->sdTotalMiB=total;
    peer->sdUsedMiB=used;
    peer->cpuTempDeciC=temp;
    peer->freeHeapBytes=heap;
}

static void processLeave(
    char **fields,
    size_t fieldCount
)
{
    // SFL1|tag|integration_id|boot|sequence
    if (fieldCount != 5)
        return;

    if (strcmp(fields[0], CLUSTER_LEAVE_MAGIC) != 0 ||
        strcmp(fields[1], runtimeClusterTag) != 0 ||
        !integrationIdValid(fields[2]) ||
        deviceIntegrationId() == fields[2]) {
        return;
    }

    uint32_t remoteBoot = 0;
    uint32_t remoteSequence = 0;
    if (!parseUint32Strict(fields[3], remoteBoot, 16) ||
        !parseUint32Strict(fields[4], remoteSequence)) {
        return;
    }

    ClusterPeer *peer = findPeer(fields[2]);
    if (!peer ||
        peer->bootNonce != remoteBoot ||
        remoteSequence < peer->sequence) {
        return;
    }

    *peer = ClusterPeer{};

    // Resources from a node that explicitly left are no longer reachable.
    for (size_t i = 0; i < CLUSTER_MAX_RESOURCES; ++i) {
        if (
            resources[i].used &&
            strcmp(resources[i].ownerIntegrationId, fields[2]) == 0
        ) {
            resources[i] = ClusterResource{};
        }
    }
}

static void processResource(
    char **fields,
    size_t fieldCount,
    const IPAddress &remoteIp
)
{
    // SFR1|tag|owner|boot|sequence|resource_id|type|timestamp_us|size|ttl|locator
    if (fieldCount != 11)
        return;

    if (strcmp(fields[0], CLUSTER_RESOURCE_MAGIC) != 0 ||
        strcmp(fields[1], runtimeClusterTag) != 0 ||
        !integrationIdValid(fields[2]) ||
        deviceIntegrationId() == fields[2]) {
        return;
    }

    uint32_t remoteBoot = 0;
    uint32_t remoteSequence = 0;
    uint32_t sizeBytes = 0;
    uint32_t ttlSeconds = 0;
    int64_t timestampUs = 0;

    if (!parseUint32Strict(fields[3], remoteBoot, 16) ||
        !parseUint32Strict(fields[4], remoteSequence) ||
        !parseInt64Strict(fields[7], timestampUs) ||
        !parseUint32Strict(fields[8], sizeBytes) ||
        !parseUint32Strict(fields[9], ttlSeconds) ||
        ttlSeconds < 1 ||
        ttlSeconds > CLUSTER_RESOURCE_TTL_MAX_SEC ||
        strlen(fields[5]) == 0 ||
        strlen(fields[5]) > 48 ||
        strlen(fields[6]) == 0 ||
        strlen(fields[6]) > 24 ||
        strlen(fields[10]) > 160) {
        return;
    }

    ClusterPeer *peer = findPeer(fields[2]);
    if (!peer ||
        peer->bootNonce != remoteBoot ||
        remoteSequence < peer->sequence) {
        return;
    }

    const uint32_t now = millis();
    ClusterResource *resource =
        resourceSlotFor(fields[2], fields[5], now);
    if (!resource)
        return;

    *resource = ClusterResource{};
    resource->used = true;
    strncpy(
        resource->ownerIntegrationId,
        fields[2],
        sizeof(resource->ownerIntegrationId) - 1
    );
    strncpy(
        resource->resourceId,
        fields[5],
        sizeof(resource->resourceId) - 1
    );
    strncpy(
        resource->resourceType,
        fields[6],
        sizeof(resource->resourceType) - 1
    );
    strncpy(
        resource->locator,
        fields[10],
        sizeof(resource->locator) - 1
    );
    resource->sourceIp = remoteIp;
    resource->timestampUs = timestampUs;
    resource->sizeBytes = sizeBytes;
    resource->ttlSeconds = ttlSeconds;
    resource->lastSeenMs = now;
}

static void processCoordination(
    char **fields,
    size_t fieldCount,
    const IPAddress &remoteIp
)
{
    // SFCO1|tag|integration_id|boot|sequence|policy|capabilities|coordinator_id|epoch
    if (fieldCount != 9)
        return;

    if (strcmp(fields[0], CLUSTER_COORDINATION_MAGIC) != 0 ||
        strcmp(fields[1], runtimeClusterTag) != 0 ||
        !integrationIdValid(fields[2]) ||
        deviceIntegrationId() == fields[2]) {
        return;
    }

    uint32_t remoteBoot = 0;
    uint32_t remoteSequence = 0;
    uint32_t policyValue = 0;
    uint32_t capabilities = 0;
    uint32_t advertisedEpoch = 0;

    if (!parseUint32Strict(fields[3], remoteBoot, 16) ||
        !parseUint32Strict(fields[4], remoteSequence) ||
        !parseUint32Strict(fields[5], policyValue) ||
        !parseUint32Strict(fields[6], capabilities) ||
        !parseUint32Strict(fields[8], advertisedEpoch) ||
        policyValue > (uint32_t)COORDINATOR_POLICY_PREFERRED ||
        (strcmp(fields[7], "-") != 0 && !integrationIdValid(fields[7]))) {
        return;
    }

    const uint32_t now = millis();
    ClusterPeer *peer = ensurePeerForAuthenticatedPacket(
        fields[2],
        remoteBoot,
        remoteIp,
        now
    );
    if (!peer)
        return;

    // Reject replayed coordination announcements, including equal sequences.
    // Presence and direct heartbeat liveness are tracked independently.
    if (peer->coordinationSequence != 0 && remoteSequence <= peer->coordinationSequence)
        return;

    peer->coordinationSequence = remoteSequence;
    peer->coordinatorPolicy = (CoordinatorPolicy)policyValue;
    peer->coordinationCapabilities = capabilities;
    peer->advertisedCoordinatorId[0] = '\0';
    if (strcmp(fields[7], "-") != 0) {
        strncpy(
            peer->advertisedCoordinatorId,
            fields[7],
            sizeof(peer->advertisedCoordinatorId) - 1
        );
    }
    // A coordinator may briefly announce an incomplete role/epoch during
    // convergence. Do not discard a recently ACK-authenticated epoch for
    // this same coordinator and boot on such an announcement. An explicit
    // nonzero epoch is still processed normally (including real transitions).
    const bool incompleteSelfAnnouncement =
        strcmp(fields[2], fields[7]) != 0 || advertisedEpoch == 0;
    const bool haveRecentConfirmedEpoch =
        strcmp(currentCoordinatorId, peer->integrationId) == 0 &&
        lastCoordinatorAckMs != 0 &&
        (uint32_t)(now - lastCoordinatorAckMs) <=
            CLUSTER_COORDINATOR_HEARTBEAT_MS * 3UL &&
        peer->bootNonce == lastAckCoordinatorBoot &&
        peer->advertisedCoordinatorEpoch == currentCoordinatorEpoch &&
        currentCoordinatorEpoch != 0;
    if (incompleteSelfAnnouncement && haveRecentConfirmedEpoch) {
        ++diagIncompleteCoordinatorAnnouncements;
        // Preserve both the self-identification and epoch proven by ACK.
        strncpy(peer->advertisedCoordinatorId, peer->integrationId,
                sizeof(peer->advertisedCoordinatorId) - 1);
    } else {
        peer->advertisedCoordinatorEpoch = advertisedEpoch;
    }
    peer->lastCoordinatorInfoMs = now;
    peer->lastSeenMs = now;

    // A newly authenticated peer policy may change the election winner.
    // Apply it immediately instead of waiting for another main-loop pass.
    recomputeCoordinator(now);
}

static void sendNodeAck(
    const ClusterPeer &peer,
    uint32_t nodeHeartbeatSequence,
    const IPAddress &remoteIp
)
{
    if (!localIsCoordinator || currentCoordinatorEpoch == 0 || !usableIp(remoteIp))
        return;

    char localBootText[9];
    char remoteBootText[9];
    snprintf(localBootText, sizeof(localBootText), "%08lx", (unsigned long)bootNonce);
    snprintf(remoteBootText, sizeof(remoteBootText), "%08lx", (unsigned long)peer.bootNonce);

    String payload;
    payload.reserve(180);
    payload += CLUSTER_NODE_ACK_MAGIC;
    payload += '|';
    payload += runtimeClusterTag;
    payload += '|';
    payload += deviceIntegrationId();
    payload += '|';
    payload += localBootText;
    payload += '|';
    payload += String((unsigned long)currentCoordinatorEpoch);
    payload += '|';
    payload += peer.integrationId;
    payload += '|';
    payload += remoteBootText;
    payload += '|';
    payload += String((unsigned long)nodeHeartbeatSequence);

    ++diagAckSendAttempted;
    if (!sendSignedPayloadTo(payload, remoteIp, CLUSTER_COORDINATOR_PORT, "cluster node ACK"))
        ++diagAckSendFailed;
}

static void processNodeHeartbeat(
    char **fields,
    size_t fieldCount,
    const IPAddress &remoteIp
)
{
    // SFNH1|tag|node|boot|hb_sequence|coordinator|epoch|policy|lease|wifi_remaining
    if (fieldCount != 10 ||
        strcmp(fields[0], CLUSTER_NODE_HEARTBEAT_MAGIC) != 0 ||
        strcmp(fields[1], runtimeClusterTag) != 0 ||
        !integrationIdValid(fields[2]) ||
        deviceIntegrationId() == fields[2] ||
        !integrationIdValid(fields[5])) {
        return;
    }

    uint32_t remoteBoot = 0;
    uint32_t heartbeatSequence = 0;
    uint32_t coordinatorEpoch = 0;
    uint32_t policyValue = 0;
    uint32_t leaseSeconds = 0;
    int32_t wifiRemainingSeconds = -1;

    if (!parseUint32Strict(fields[3], remoteBoot, 16) ||
        !parseUint32Strict(fields[4], heartbeatSequence) ||
        !parseUint32Strict(fields[6], coordinatorEpoch) ||
        !parseUint32Strict(fields[7], policyValue) ||
        !parseUint32Strict(fields[8], leaseSeconds) ||
        !parseInt32Strict(fields[9], wifiRemainingSeconds) ||
        policyValue > (uint32_t)COORDINATOR_POLICY_PREFERRED ||
        leaseSeconds < 20 ||
        leaseSeconds > CLUSTER_RESOURCE_TTL_MAX_SEC ||
        wifiRemainingSeconds < -1) {
        return;
    }

    if (!localIsCoordinator ||
        strcmp(fields[5], deviceIntegrationId().c_str()) != 0 ||
        coordinatorEpoch == 0 ||
        coordinatorEpoch != currentCoordinatorEpoch) {
        return;
    }

    const uint32_t now = millis();
    ClusterPeer *peer = ensurePeerForAuthenticatedPacket(
        fields[2],
        remoteBoot,
        remoteIp,
        now
    );
    if (!peer)
        return;

    if (peer->directHeartbeatSequence != 0 &&
        heartbeatSequence <= peer->directHeartbeatSequence) {
        return;
    }

    peer->coordinatorPolicy = (CoordinatorPolicy)policyValue;
    peer->leaseSeconds = leaseSeconds;
    peer->wifiRemainingSeconds = wifiRemainingSeconds;
    peer->directHeartbeatSequence = heartbeatSequence;
    peer->lastDirectHeartbeatMs = now;
    peer->lastSeenMs = now;

    sendNodeAck(*peer, heartbeatSequence, remoteIp);
}

static void processNodeAck(
    char **fields,
    size_t fieldCount,
    const IPAddress &remoteIp
)
{
    // SFNA1|tag|coordinator|coord_boot|epoch|node|node_boot|hb_sequence
    ++diagAckAuthenticated;
    if (fieldCount != 8 ||
        strcmp(fields[0], CLUSTER_NODE_ACK_MAGIC) != 0 ||
        strcmp(fields[1], runtimeClusterTag) != 0 ||
        !integrationIdValid(fields[2]) ||
        !integrationIdValid(fields[5])) {
        ++diagAckRejected;
        diagAckLastReject = "format_or_tag";
        return;
    }

    uint32_t coordinatorBoot = 0;
    uint32_t coordinatorEpoch = 0;
    uint32_t nodeBoot = 0;
    uint32_t heartbeatSequence = 0;
    if (!parseUint32Strict(fields[3], coordinatorBoot, 16) ||
        !parseUint32Strict(fields[4], coordinatorEpoch) ||
        !parseUint32Strict(fields[6], nodeBoot, 16) ||
        !parseUint32Strict(fields[7], heartbeatSequence)) {
        ++diagAckRejected;
        diagAckLastReject = "number_format";
        return;
    }

    if (localIsCoordinator ||
        nodeBoot != bootNonce ||
        strcmp(fields[5], deviceIntegrationId().c_str()) != 0 ||
        strcmp(fields[2], currentCoordinatorId) != 0 ||
        coordinatorEpoch == 0 ||
        coordinatorEpoch != currentCoordinatorEpoch) {
        ++diagAckRejected;
        diagAckLastReject = "identity_boot_or_epoch";
        return;
    }

    if (lastCoordinatorAckSequence != 0 &&
        heartbeatSequence <= lastCoordinatorAckSequence) {
        ++diagAckRejected;
        diagAckLastReject = "old_sequence";
        return;
    }

    const uint32_t now = millis();
    ++diagAckAccepted;
    lastCoordinatorAckMs = now;
    lastAckCoordinatorBoot = coordinatorBoot;
    lastCoordinatorAckSequence = heartbeatSequence;
    currentCoordinatorIp = remoteIp;

    ClusterPeer *coordinator = ensurePeerForAuthenticatedPacket(
        fields[2],
        coordinatorBoot,
        remoteIp,
        now
    );
    if (coordinator) {
        coordinator->advertisedCoordinatorEpoch = coordinatorEpoch;
        strncpy(
            coordinator->advertisedCoordinatorId,
            fields[2],
            sizeof(coordinator->advertisedCoordinatorId) - 1
        );
        coordinator->lastCoordinatorInfoMs = now;
        coordinator->lastSeenMs = now;
    }
}

static String int64Text(int64_t value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    return String(buffer);
}

static int64_t localUnixTimeUs()
{
    struct timeval tv = {};
    if (gettimeofday(&tv, nullptr) != 0 || tv.tv_sec < 1577836800LL)
        return 0;
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// All packets pass through existing HMAC verification in processPacket().
// Only the elected coordinator for this epoch may answer requests.
static void processTimeRequest(char **f, size_t n, const IPAddress &ip)
{
    // SFTQ1|tag|node|node_boot|seq|coordinator|epoch|t1_mono_us
    if (n != 8 || !localIsCoordinator || currentCoordinatorEpoch == 0 ||
        strcmp(f[1], runtimeClusterTag) != 0 ||
        strcmp(f[5], deviceIntegrationId().c_str()) != 0 ||
        !integrationIdValid(f[2])) return;
    uint32_t nodeBoot = 0, seq = 0, epoch = 0;
    int64_t t1 = 0;
    if (!parseUint32Strict(f[3], nodeBoot, 16) ||
        !parseUint32Strict(f[4], seq) ||
        !parseUint32Strict(f[6], epoch) ||
        !parseInt64Strict(f[7], t1) || epoch != currentCoordinatorEpoch ||
        t1 <= 0 || seq == 0) return;
    const ClusterPeer *peer = findPeer(f[2]);
    if (!peer || peer->bootNonce != nodeBoot || peer->ip != ip ||
        (uint32_t)(millis() - peer->lastSeenMs) > 60000UL) return;
    const int64_t t2 = localUnixTimeUs();
    if (t2 == 0) return; // Never advertise an untrusted/unset wall clock.
    const int64_t t3 = localUnixTimeUs();
    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lx", (unsigned long)bootNonce);
    String payload = String(CLUSTER_TIME_REPLY_MAGIC) + "|" + runtimeClusterTag + "|" +
        deviceIntegrationId() + "|" + bootText + "|" +
        String((unsigned long)currentCoordinatorEpoch) + "|" + f[2] + "|" +
        f[3] + "|" + String((unsigned long)seq) + "|" +
        int64Text(t1) + "|" + int64Text(t2) + "|" +
        int64Text(t3);
    sendSignedPayloadTo(payload, ip, CLUSTER_COORDINATOR_PORT, "cluster time reply");
}

static void processTimeReply(char **f, size_t n, const IPAddress &ip)
{
    // SFTR1|tag|coord|coord_boot|epoch|node|node_boot|seq|t1|t2_utc|t3_utc
    if (n != 11 || localIsCoordinator || timePendingSeq == 0 ||
        strcmp(f[1], runtimeClusterTag) != 0 ||
        strcmp(f[2], currentCoordinatorId) != 0 ||
        strcmp(f[5], deviceIntegrationId().c_str()) != 0 ||
        ip != currentCoordinatorIp) return;
    uint32_t remoteBoot=0, epoch=0, ownBoot=0, seq=0;
    int64_t t1=0,t2=0,t3=0;
    if (!parseUint32Strict(f[3], remoteBoot, 16) ||
        !parseUint32Strict(f[4], epoch) ||
        !parseUint32Strict(f[6], ownBoot, 16) ||
        !parseUint32Strict(f[7], seq) ||
        !parseInt64Strict(f[8], t1) ||
        !parseInt64Strict(f[9], t2) ||
        !parseInt64Strict(f[10], t3) ||
        ownBoot != bootNonce || epoch != currentCoordinatorEpoch ||
        seq != timePendingSeq || t1 != timePendingMonoUs ||
        remoteBoot == 0 || t3 < t2 || t2 < 1577836800000000LL) return;
    const ClusterPeer *peer = findPeer(f[2]);
    if (!peer || peer->bootNonce != remoteBoot) return;
    const int64_t t4 = esp_timer_get_time();
    const int64_t elapsed = t4 - t1;
    const int64_t serverWork = t3 - t2;
    const int64_t rtt = elapsed - serverWork;
    timePendingSeq = 0;
    if (elapsed <= 0 || rtt < 0 || rtt > CLUSTER_TIME_MAX_RTT_US ||
        serverWork > 100000LL) { ++timeRejectedSamples; return; }
    // Symmetric path estimate; approximate network asymmetry by RTT/2.
    const int64_t localMidpoint = t1 + elapsed / 2;
    const int64_t referenceMidpoint = t2 + serverWork / 2;
    timeAnchorUtcUs = referenceMidpoint;
    timeAnchorMonoUs = localMidpoint;
    timeLastRttUs = rtt;
    timeLastSampleMs = millis();
    timeEstimateValid = true;
    strncpy(timeAuthorityId, currentCoordinatorId, sizeof(timeAuthorityId)-1);
    timeAuthorityEpoch = currentCoordinatorEpoch;
    ++timeAcceptedSamples;
    // One compact signed report per accepted measurement; no periodic telemetry.
    const int64_t wallUs = localUnixTimeUs();
    const int64_t offsetUs = wallUs ? (referenceMidpoint + (t4 - localMidpoint) - wallUs) : -86400000000LL;
    const int64_t uncertaintyUs = rtt / 2;
    char ownBootText[9];
    snprintf(ownBootText, sizeof(ownBootText), "%08lx", (unsigned long)bootNonce);
    const String report = String(CLUSTER_TIME_REPORT_MAGIC) + "|" + runtimeClusterTag + "|" +
        deviceIntegrationId() + "|" + ownBootText + "|" +
        String((unsigned long)currentCoordinatorEpoch) + "|" +
        String((unsigned long)timeSyncLastReceivedSeq) + "|" +
        int64Text(referenceMidpoint) + "|" + int64Text(offsetUs) + "|" +
        int64Text(rtt) + "|" + int64Text(uncertaintyUs);
    sendSignedPayloadTo(report, currentCoordinatorIp, CLUSTER_COORDINATOR_PORT, "cluster time report");
}

static void requestClusterTime(uint32_t now)
{
    if (!runtimeActive || localIsCoordinator || !currentCoordinatorEpoch ||
        !usableIp(currentCoordinatorIp) ||
        (timeLastRequestMs && (uint32_t)(now-timeLastRequestMs) <
            (timeEstimateValid ? CLUSTER_TIME_INTERVAL_MS : CLUSTER_TIME_RETRY_MS)))
        return;
    timeLastRequestMs = now;
    const int64_t t1 = esp_timer_get_time();
    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lx", (unsigned long)bootNonce);
    const uint32_t seq = ++timeRequestSeq;
    String payload = String(CLUSTER_TIME_REQUEST_MAGIC) + "|" + runtimeClusterTag + "|" +
        deviceIntegrationId() + "|" + bootText + "|" +
        String((unsigned long)seq) + "|" + currentCoordinatorId + "|" +
        String((unsigned long)currentCoordinatorEpoch) + "|" + int64Text(t1);
    timePendingSeq = seq;
    timePendingMonoUs = t1;
    if (!sendSignedPayloadTo(payload, currentCoordinatorIp,
                             CLUSTER_COORDINATOR_PORT, "cluster time request"))
        timePendingSeq = 0;
}

// Signed multicast command: only the currently elected coordinator, with the
// current boot nonce and epoch, may request an immediate time sample.
// This is an advisory trigger, NOT a remote system-clock change.
// SFTD1 is HMAC-authenticated in processPacket, and additionally bound to
// a live peer, its current boot nonce and the coordinator's current epoch.
static void processTimeReport(char **f, size_t n, const IPAddress &ip)
{
    if (n != 10 || !runtimeActive || !localIsCoordinator ||
        strcmp(f[1], runtimeClusterTag) != 0 || !integrationIdValid(f[2])) return;
    uint32_t senderBoot=0, epoch=0, commandSeq=0;
    int64_t utc=0, offset=0, rtt=0, uncertainty=0;
    if (!parseUint32Strict(f[3], senderBoot, 16) ||
        !parseUint32Strict(f[4], epoch) ||
        !parseUint32Strict(f[5], commandSeq) ||
        !parseInt64Strict(f[6], utc) || !parseInt64Strict(f[7], offset) ||
        !parseInt64Strict(f[8], rtt) || !parseInt64Strict(f[9], uncertainty) ||
        epoch != currentCoordinatorEpoch || senderBoot == 0 ||
        utc < 1577836800000000LL || rtt < 0 || rtt > CLUSTER_TIME_MAX_RTT_US ||
        uncertainty < 0 || uncertainty > CLUSTER_TIME_MAX_RTT_US ||
        offset < -86400000000LL || offset > 86400000000LL ||
        commandSeq > timeSyncCommandSeq) return;
    ClusterPeer *peer = findPeer(f[2]);
    if (!peer || peer->bootNonce != senderBoot || peer->ip != ip ||
        (uint32_t)(millis() - peer->lastSeenMs) > 60000UL) return;
    // A delayed report from a previous request may not overwrite a newer one.
    if (peer->timeReportValid && peer->timeReportCommandSeq > commandSeq) return;
    peer->timeReportValid = true;
    peer->timeReportMs = millis();
    peer->timeReportEpoch = epoch;
    peer->timeReportCommandSeq = commandSeq;
    peer->timeReportUtcUs = utc;
    peer->timeReportOffsetUs = offset;
    peer->timeReportRttUs = rtt;
    peer->timeReportUncertaintyUs = uncertainty;
}

static void processTimeSyncCommand(char **f, size_t n, const IPAddress &ip)
{
    // SFTS1|tag|coordinator_id|boot_hex|coordinator_epoch|sequence
    if (n != 6 || !runtimeActive || localIsCoordinator ||
        strcmp(f[1], runtimeClusterTag) != 0 ||
        strcmp(f[2], currentCoordinatorId) != 0 ||
        ip != currentCoordinatorIp) return;
    uint32_t senderBoot = 0, epoch = 0, seq = 0;
    if (!parseUint32Strict(f[3], senderBoot, 16) ||
        !parseUint32Strict(f[4], epoch) ||
        !parseUint32Strict(f[5], seq) || senderBoot == 0 || seq == 0 ||
        epoch == 0 || epoch != currentCoordinatorEpoch) return;
    const ClusterPeer *peer = findPeer(f[2]);
    if (!peer || peer->bootNonce != senderBoot || peer->ip != ip ||
        (uint32_t)(millis() - peer->lastSeenMs) > 60000UL) return;
    if (timeSyncLastCoordinatorBoot == senderBoot &&
        seq <= timeSyncLastReceivedSeq) return;
    timeSyncLastCoordinatorBoot = senderBoot;
    timeSyncLastReceivedSeq = seq;
    timeSyncLastCommandMs = millis();
    ++timeSyncCommandsReceived;
    // Bypass the regular 15-minute interval. A failed request continues
    // through the existing bounded 30-second retry path.
    timeLastRequestMs = 0;
}

// A probe has NO executor and cannot restart, power down or delete anything.
// SFJP1|tag|coordinator|coord_boot_hex|epoch|target|target_boot_hex|seq
static void processJobProbe(char **f, size_t n, const IPAddress &ip)
{
    ++probeReceived;
    if (n != 8 || !runtimeActive || localIsCoordinator) {
        rejectProbe("format_or_role"); return;
    }
    if (strcmp(f[1], runtimeClusterTag) != 0) {
        rejectProbe("cluster_tag"); return;
    }
    if (strcmp(f[2], currentCoordinatorId) != 0 ||
        strcmp(f[5], deviceIntegrationId().c_str()) != 0 ||
        ip != currentCoordinatorIp) {
        rejectProbe("identity_or_ip"); return;
    }
    uint32_t cb = 0, epoch = 0, tb = 0, seq = 0;
    if (!parseUint32Strict(f[3], cb, 16) ||
        !parseUint32Strict(f[4], epoch) ||
        !parseUint32Strict(f[6], tb, 16) ||
        !parseUint32Strict(f[7], seq) ||
        cb == 0 || seq == 0 || tb != bootNonce ||
        epoch == 0 || epoch != currentCoordinatorEpoch) {
        rejectProbe("boot_epoch_or_sequence"); return;
    }
    const ClusterPeer *coord = findPeer(f[2]);
    if (!coord || coord->bootNonce != cb || coord->ip != ip ||
        (uint32_t)(millis() - coord->lastSeenMs) > 60000UL) {
        rejectProbe("coordinator_peer_lease"); return;
    }
    if (probeSeenBoot == cb && probeSeenEpoch == epoch &&
        seq < probeSeenSeq) {
        rejectProbe("old_sequence"); return;
    }
    if (probeSeenBoot != cb || probeSeenEpoch != epoch || seq > probeSeenSeq) {
        probeSeenBoot = cb; probeSeenEpoch = epoch; probeSeenSeq = seq;
    }
    char ownBoot[9];
    snprintf(ownBoot, sizeof(ownBoot), "%08lx", (unsigned long)bootNonce);
    const String reply = String(CLUSTER_JOB_ACK_MAGIC) + "|" + runtimeClusterTag +
        "|" + f[2] + "|" + f[3] + "|" + f[4] + "|" +
        deviceIntegrationId() + "|" + ownBoot + "|" + f[7];
    if (sendSignedPayloadTo(reply, ip, CLUSTER_COORDINATOR_PORT, "job probe ACK"))
        ++probeAckSent;
    else
        rejectProbe("ack_send_failed");
}

// SFJA1|tag|coordinator|coord_boot_hex|epoch|node|node_boot_hex|seq
static void processJobProbeAck(char **f, size_t n, const IPAddress &ip)
{
    ++probeAckReceived;
    if (n != 8 || !runtimeActive || !localIsCoordinator ||
        strcmp(f[1], runtimeClusterTag) != 0 ||
        strcmp(f[2], deviceIntegrationId().c_str()) != 0 ||
        !integrationIdValid(f[5])) {
        rejectProbeAck("format_role_or_identity"); return;
    }
    uint32_t cb = 0, epoch = 0, nb = 0, seq = 0;
    if (!parseUint32Strict(f[3], cb, 16) ||
        !parseUint32Strict(f[4], epoch) ||
        !parseUint32Strict(f[6], nb, 16) ||
        !parseUint32Strict(f[7], seq) ||
        cb != bootNonce || epoch != currentCoordinatorEpoch ||
        nb == 0 || seq == 0) {
        rejectProbeAck("boot_epoch_or_sequence"); return;
    }
    const ClusterPeer *node = findPeer(f[5]);
    if (!node || node->bootNonce != nb || node->ip != ip ||
        (uint32_t)(millis() - node->lastSeenMs) > 60000UL) {
        rejectProbeAck("node_peer_lease"); return;
    }
    const uint64_t id = ((uint64_t)cb << 32) | seq;
    const ClusterJobs::Entry *e = ClusterJobs::find(id);
    if (!e || e->kind != ClusterJobs::Kind::Probe ||
        strcmp(e->nodeId, f[5]) != 0 ||
        e->state != ClusterJobs::State::Sent) {
        rejectProbeAck("job_not_pending"); return;
    }
    const uint32_t now = millis();
    if (ClusterJobs::transition(id, epoch, ClusterJobs::State::Accepted, now) &&
        ClusterJobs::transition(id, epoch, ClusterJobs::State::Running, now) &&
        ClusterJobs::transition(id, epoch, ClusterJobs::State::Succeeded, now))
        return;
    rejectProbeAck("job_transition");
}

// Retry ONLY still-pending, non-destructive probe jobs with the same ID.
// Do not extend the original 15-second deadline; do not accept a changed boot.
static void serviceProbeRetries(uint32_t nowMs)
{
    if (!localIsCoordinator || !runtimeActive) return;
    char cb[9];
    snprintf(cb, sizeof(cb), "%08lx", (unsigned long)bootNonce);
    for (size_t i = 0; i < ClusterJobs::kMaxJobs; ++i) {
        ProbeRetry &r = probeRetries[i];
        if (!r.id || r.attempts >= PROBE_MAX_ATTEMPTS ||
            (uint32_t)(nowMs - r.lastSendMs) < PROBE_RETRY_INTERVAL_MS) continue;
        const ClusterJobs::Entry *job = ClusterJobs::find(r.id);
        if (!job || job->kind != ClusterJobs::Kind::Probe ||
            job->state != ClusterJobs::State::Sent ||
            job->coordinatorEpoch != currentCoordinatorEpoch ||
            (int32_t)(job->deadlineMs - nowMs) <= 0) {
            r.id = 0;
            continue;
        }
        const ClusterPeer *peer = findPeer(job->nodeId);
        if (!peer || !peer->used || !usableIp(peer->ip) ||
            peer->bootNonce != r.targetBoot ||
            (uint32_t)(nowMs - peer->lastSeenMs) > 60000UL) {
            r.id = 0;
            continue;
        }
        char nb[9];
        snprintf(nb, sizeof(nb), "%08lx", (unsigned long)r.targetBoot);
        const uint32_t seq = (uint32_t)r.id;
        const String payload = String(CLUSTER_JOB_PROBE_MAGIC) + "|" + runtimeClusterTag +
            "|" + deviceIntegrationId() + "|" + cb + "|" +
            String((unsigned long)currentCoordinatorEpoch) + "|" +
            job->nodeId + "|" + nb + "|" + String((unsigned long)seq);
        ++r.attempts;
        r.lastSendMs = nowMs;
        ++probeRetransmissions;
        sendSignedPayloadTo(payload, peer->ip, CLUSTER_COORDINATOR_PORT,
                            "job probe retransmission");
    }
}

// SFJX1|tag|coordinator|coord_boot|epoch|node|node_boot|seq|kind|reason
// A refusal is authenticated and correlated to the original single-node job.
// Never return a refusal for unauthenticated or replayed traffic.
static void sendJobRefusal(const char *const *f, const IPAddress &ip,
                           uint32_t ownBoot, uint8_t kind, uint16_t reason)
{
    char ownBootText[9];
    snprintf(ownBootText, sizeof(ownBootText), "%08lx", (unsigned long)ownBoot);
    const String packet = String(CLUSTER_JOB_REJECT_MAGIC) + "|" + runtimeClusterTag +
        "|" + f[2] + "|" + f[3] + "|" + f[4] + "|" +
        deviceIntegrationId() + "|" + ownBootText + "|" + f[7] + "|" +
        String((unsigned int)kind) + "|" + String((unsigned int)reason);
    sendSignedPayloadTo(packet, ip, CLUSTER_COORDINATOR_PORT, "job refusal");
}

static void processJobRefusal(char **f, size_t n, const IPAddress &ip)
{
    if (n != 10 || !runtimeActive || !localIsCoordinator ||
        strcmp(f[1], runtimeClusterTag) != 0 ||
        strcmp(f[2], deviceIntegrationId().c_str()) != 0 ||
        !integrationIdValid(f[5])) return;
    uint32_t cb=0,epoch=0,nb=0,seq=0,kind=0,reason=0;
    if (!parseUint32Strict(f[3],cb,16) || !parseUint32Strict(f[4],epoch) ||
        !parseUint32Strict(f[6],nb,16) || !parseUint32Strict(f[7],seq) ||
        !parseUint32Strict(f[8],kind) || !parseUint32Strict(f[9],reason) ||
        cb != bootNonce || epoch != currentCoordinatorEpoch || nb == 0 ||
        seq == 0 || (kind != 1 && kind != 2) || reason == 0 || reason > 100) return;
    const ClusterPeer *node=findPeer(f[5]);
    if (!node || node->bootNonce != nb || node->ip != ip ||
        (uint32_t)(millis()-node->lastSeenMs)>60000UL) return;
    const uint64_t id=((uint64_t)cb<<32)|seq;
    const ClusterJobs::Entry *entry=ClusterJobs::find(id);
    if (!entry || strcmp(entry->nodeId,f[5]) != 0 ||
        (unsigned int)entry->kind != kind ||
        entry->state != ClusterJobs::State::Sent) return;
    if (ClusterJobs::transition(id,epoch,ClusterJobs::State::Failed,millis(),(uint16_t)reason))
        Serial.printf("[CLUSTER JOB] authenticated refusal kind=%lu seq=%lu code=%lu\n",
                      (unsigned long)kind,(unsigned long)seq,(unsigned long)reason);
}

// SFJR1|tag|coordinator|coord_boot|epoch|target|target_boot|seq
// A destructive effect is NEVER triggered by the transport probe or a retry.
static void processJobRestart(char **f, size_t n, const IPAddress &ip)
{
    ++restartReceived;
    Serial.printf("[CLUSTER JOB] restart packet received from %s\n", ip.toString().c_str());
    if (n != 8 || !runtimeActive || localIsCoordinator ||
        strcmp(f[1], runtimeClusterTag) != 0 ||
        strcmp(f[2], currentCoordinatorId) != 0 ||
        strcmp(f[5], deviceIntegrationId().c_str()) != 0 || ip != currentCoordinatorIp) {
        rejectRestart("role_identity_or_ip"); return;
    }
    uint32_t cb=0, epoch=0, targetBoot=0, seq=0;
    if (!parseUint32Strict(f[3], cb, 16) || !parseUint32Strict(f[4], epoch) ||
        !parseUint32Strict(f[6], targetBoot, 16) || !parseUint32Strict(f[7], seq) ||
        cb == 0 || seq == 0 || targetBoot != bootNonce ||
        epoch == 0 || epoch != currentCoordinatorEpoch) {
        rejectRestart("boot_epoch_or_sequence"); return;
    }
    const ClusterPeer *coord = findPeer(f[2]);
    if (!coord || coord->bootNonce != cb || coord->ip != ip ||
        (uint32_t)(millis() - coord->lastSeenMs) > 60000UL) {
        rejectRestart("coordinator_lease"); return;
    }
    // Do not execute a job twice, including when the same signed packet is replayed.
    // Coordinator boot+epoch isolate the sequence domain.
    if (restartSeenBoot == cb && restartSeenEpoch == epoch && seq <= restartSeenSeq) {
        rejectRestart("replay"); return;
    }
    // Even a refused, otherwise authenticated command consumes its sequence.
    restartSeenBoot=cb; restartSeenEpoch=epoch; restartSeenSeq=seq;
    String error;
    if (!webConfigScheduleReboot(3500UL, error)) {
        Serial.printf("[CLUSTER JOB] restart schedule blocked: %s\n", error.c_str());
        rejectRestart("recording_or_schedule_blocked");
        sendJobRefusal((const char *const *)f, ip, bootNonce, 1, 2);
        return; // No ACK: coordinator displays timeout, not success.
    }
    char ownBoot[9];
    snprintf(ownBoot, sizeof(ownBoot), "%08lx", (unsigned long)bootNonce);
    const String reply = String(CLUSTER_JOB_RESTART_ACK_MAGIC) + "|" + runtimeClusterTag +
        "|" + f[2] + "|" + f[3] + "|" + f[4] + "|" +
        deviceIntegrationId() + "|" + ownBoot + "|" + f[7];
    if (!sendSignedPayloadTo(reply, ip, CLUSTER_COORDINATOR_PORT, "job restart accepted"))
        rejectRestart("ack_send_failed");
    else {
        ++restartAccepted;
        Serial.printf("[CLUSTER JOB] restart scheduled; ACK sent; seq=%lu\n", (unsigned long)seq);
    }
}

// ACK proves that reboot was scheduled, NOT that the device rebooted or came back.
static void processJobRestartAck(char **f, size_t n, const IPAddress &ip)
{
    ++restartAckReceived;
    if (n != 8 || !runtimeActive || !localIsCoordinator ||
        strcmp(f[1], runtimeClusterTag) != 0 ||
        strcmp(f[2], deviceIntegrationId().c_str()) != 0 ||
        !integrationIdValid(f[5])) {
        rejectRestartAck("format_role_or_identity"); return;
    }
    uint32_t cb=0, epoch=0, nb=0, seq=0;
    if (!parseUint32Strict(f[3], cb, 16) || !parseUint32Strict(f[4], epoch) ||
        !parseUint32Strict(f[6], nb, 16) || !parseUint32Strict(f[7], seq) ||
        cb != bootNonce || epoch != currentCoordinatorEpoch || nb == 0 || seq == 0) {
        rejectRestartAck("boot_epoch_or_sequence"); return;
    }
    const ClusterPeer *node = findPeer(f[5]);
    if (!node || node->bootNonce != nb || node->ip != ip ||
        (uint32_t)(millis() - node->lastSeenMs) > 60000UL) {
        rejectRestartAck("node_peer_lease"); return;
    }
    const uint64_t id = ((uint64_t)cb << 32) | seq;
    const ClusterJobs::Entry *e = ClusterJobs::find(id);
    if (!e || e->kind != ClusterJobs::Kind::Restart ||
        strcmp(e->nodeId, f[5]) != 0 || e->state != ClusterJobs::State::Sent) {
        rejectRestartAck("job_not_pending"); return;
    }
    if (ClusterJobs::transition(id, epoch, ClusterJobs::State::Accepted, millis())) {
        ++restartAckAccepted;
        Serial.printf("[CLUSTER JOB] restart ACK accepted; seq=%lu\n", (unsigned long)seq);
    } else {
        rejectRestartAck("job_transition");
    }
}

// SFJS1|tag|coordinator|coord_boot|epoch|target|target_boot|seq
// A receiver schedules its own existing protected shutdown, then sends an ACK.
static void processJobShutdown(char **f, size_t n, const IPAddress &ip)
{
    ++shutdownReceived;
    if (n != 8 || !runtimeActive || localIsCoordinator ||
        strcmp(f[1], runtimeClusterTag) != 0 ||
        strcmp(f[2], currentCoordinatorId) != 0 ||
        strcmp(f[5], deviceIntegrationId().c_str()) != 0 || ip != currentCoordinatorIp) {
        rejectShutdown("role_identity_or_ip"); return;
    }
    uint32_t cb=0, epoch=0, targetBoot=0, seq=0;
    if (!parseUint32Strict(f[3], cb, 16) || !parseUint32Strict(f[4], epoch) ||
        !parseUint32Strict(f[6], targetBoot, 16) || !parseUint32Strict(f[7], seq) ||
        cb == 0 || seq == 0 || targetBoot != bootNonce ||
        epoch == 0 || epoch != currentCoordinatorEpoch) {
        rejectShutdown("boot_epoch_or_sequence"); return;
    }
    const ClusterPeer *coord=findPeer(f[2]);
    if (!coord || coord->bootNonce != cb || coord->ip != ip ||
        (uint32_t)(millis() - coord->lastSeenMs) > 60000UL) {
        rejectShutdown("coordinator_lease"); return;
    }
    if (shutdownSeenBoot == cb && shutdownSeenEpoch == epoch && seq <= shutdownSeenSeq) {
        rejectShutdown("replay"); return;
    }
    shutdownSeenBoot=cb; shutdownSeenEpoch=epoch; shutdownSeenSeq=seq;
    String error;
    // 5 seconds gives the signed ACK time to leave before WiFi is stopped.
    if (!webConfigScheduleShutdown(5000UL, error)) {
        Serial.printf("[CLUSTER JOB] shutdown blocked: %s\n", error.c_str());
        rejectShutdown("recording_or_schedule_blocked");
        sendJobRefusal((const char *const *)f, ip, bootNonce, 2, 2); return;
    }
    char ownBoot[9]; snprintf(ownBoot, sizeof(ownBoot), "%08lx", (unsigned long)bootNonce);
    const String reply=String(CLUSTER_JOB_SHUTDOWN_ACK_MAGIC)+"|"+runtimeClusterTag+
        "|"+f[2]+"|"+f[3]+"|"+f[4]+"|"+deviceIntegrationId()+"|"+ownBoot+"|"+f[7];
    if (!sendSignedPayloadTo(reply, ip, CLUSTER_COORDINATOR_PORT, "job shutdown accepted"))
        rejectShutdown("ack_send_failed");
    else { ++shutdownAccepted; Serial.printf("[CLUSTER JOB] shutdown scheduled; ACK sent; seq=%lu\n", (unsigned long)seq); }
}

static void processJobShutdownAck(char **f, size_t n, const IPAddress &ip)
{
    ++shutdownAckReceived;
    if (n != 8 || !runtimeActive || !localIsCoordinator ||
        strcmp(f[1], runtimeClusterTag) != 0 ||
        strcmp(f[2], deviceIntegrationId().c_str()) != 0 || !integrationIdValid(f[5])) {
        rejectShutdownAck("format_role_or_identity"); return;
    }
    uint32_t cb=0, epoch=0, nb=0, seq=0;
    if (!parseUint32Strict(f[3], cb, 16) || !parseUint32Strict(f[4], epoch) ||
        !parseUint32Strict(f[6], nb, 16) || !parseUint32Strict(f[7], seq) ||
        cb != bootNonce || epoch != currentCoordinatorEpoch || nb == 0 || seq == 0) {
        rejectShutdownAck("boot_epoch_or_sequence"); return;
    }
    const ClusterPeer *node=findPeer(f[5]);
    if (!node || node->bootNonce != nb || node->ip != ip ||
        (uint32_t)(millis() - node->lastSeenMs) > 60000UL) {
        rejectShutdownAck("node_peer_lease"); return;
    }
    const uint64_t id=((uint64_t)cb<<32)|seq;
    const ClusterJobs::Entry *e=ClusterJobs::find(id);
    if (!e || e->kind != ClusterJobs::Kind::Shutdown ||
        strcmp(e->nodeId, f[5]) != 0 || e->state != ClusterJobs::State::Sent) {
        rejectShutdownAck("job_not_pending"); return;
    }
    if (ClusterJobs::transition(id, epoch, ClusterJobs::State::Accepted, millis())) ++shutdownAckAccepted;
    else rejectShutdownAck("job_transition");
}

// Signed scheduled capture: command SFJC1 and status SFJCR1.
// Camera work occurs in the firmware main loop, never in the UDP callback.
static void sendCaptureResult(uint32_t cb,uint32_t epoch,uint32_t seq,uint16_t status,uint16_t error,uint32_t bytes,const char *uuid="",int64_t dispatch=0) {
    if(!runtimeActive || localIsCoordinator || epoch!=currentCoordinatorEpoch || !usableIp(currentCoordinatorIp))return;
    char coordBoot[9],ownBoot[9];
    snprintf(coordBoot,sizeof(coordBoot),"%08lx",(unsigned long)cb);
    snprintf(ownBoot,sizeof(ownBoot),"%08lx",(unsigned long)bootNonce);
    char dispatchText[30];snprintf(dispatchText,sizeof(dispatchText),"%lld",(long long)dispatch);
    const String packet=String(CLUSTER_JOB_CAPTURE_RESULT_MAGIC)+"|"+runtimeClusterTag+"|"+currentCoordinatorId+
        "|"+coordBoot+"|"+String((unsigned long)epoch)+"|"+deviceIntegrationId()+"|"+ownBoot+"|"+
        String((unsigned long)seq)+"|"+String((unsigned int)status)+"|"+
        String((unsigned int)error)+"|"+String((unsigned long)bytes)+"|"+String(uuid[0]?uuid:"-")+"|"+String(dispatchText);
    sendSignedPayloadTo(packet,currentCoordinatorIp,CLUSTER_COORDINATOR_PORT,"capture status");
}
// Only terminal outcomes are retried. Retransmission is status-only and
// cannot invoke the camera. The coordinator deduplicates by job ID/state.
static void queueCaptureTerminalResult(uint32_t cb,uint32_t epoch,uint32_t seq,
                                      uint16_t status,uint16_t error,uint32_t bytes,
                                      const char *uuid="",int64_t dispatch=0){
    captureResultRetry=CaptureResultRetry{};
    CaptureResultRetry &r=captureResultRetry;
    r.active=true;r.cb=cb;r.epoch=epoch;r.seq=seq;r.status=status;
    r.error=error;r.bytes=bytes;r.dispatch=dispatch;
    if(uuid){strncpy(r.uuid,uuid,sizeof(r.uuid)-1);}
    sendCaptureResult(cb,epoch,seq,status,error,bytes,r.uuid,dispatch);
    r.attempts=1;r.lastSendMs=millis();
}
static void serviceCaptureResultRetry(uint32_t now){
    CaptureResultRetry &r=captureResultRetry;
    if(!r.active)return;
    if(!runtimeActive||localIsCoordinator||r.epoch!=currentCoordinatorEpoch||
       !usableIp(currentCoordinatorIp)){
        r=CaptureResultRetry{};return;
    }
    if((uint32_t)(now-r.lastSendMs)<2000UL)return;
    if(r.attempts>=10){++captureResultRetryExhausted;r=CaptureResultRetry{};return;}
    sendCaptureResult(r.cb,r.epoch,r.seq,r.status,r.error,r.bytes,r.uuid,r.dispatch);
    ++r.attempts;++captureResultRetrySent;r.lastSendMs=now;
}
static void sendCaptureReceipt(uint32_t cb,uint32_t epoch,uint32_t seq,
                               uint32_t nodeBoot,uint16_t status,const IPAddress &ip){
    char cbText[9],nbText[9];
    snprintf(cbText,sizeof(cbText),"%08lx",(unsigned long)cb);
    snprintf(nbText,sizeof(nbText),"%08lx",(unsigned long)nodeBoot);
    const String packet=String(CLUSTER_JOB_CAPTURE_RECEIPT_MAGIC)+"|"+runtimeClusterTag+"|"+
        deviceIntegrationId()+"|"+cbText+"|"+String((unsigned long)epoch)+"|"+
        String(nbText)+"|"+String((unsigned long)seq)+"|"+String((unsigned)status);
    sendSignedPayloadTo(packet,ip,CLUSTER_COORDINATOR_PORT,"capture result receipt");
}
static void processCaptureReceipt(char **f,size_t n,const IPAddress &ip){
    if(n!=8||!runtimeActive||localIsCoordinator||!captureResultRetry.active||
       strcmp(f[1],runtimeClusterTag)!=0||strcmp(f[2],currentCoordinatorId)!=0||
       ip!=currentCoordinatorIp)return;
    uint32_t cb=0,epoch=0,nb=0,seq=0,status=0;
    if(!parseUint32Strict(f[3],cb,16)||!parseUint32Strict(f[4],epoch)||
       !parseUint32Strict(f[5],nb,16)||!parseUint32Strict(f[6],seq)||
       !parseUint32Strict(f[7],status)||nb!=bootNonce||
       cb!=captureResultRetry.cb||epoch!=captureResultRetry.epoch||
       seq!=captureResultRetry.seq||status!=captureResultRetry.status||
       epoch!=currentCoordinatorEpoch)return;
    const ClusterPeer *coord=findPeer(f[2]);
    if(!coord||coord->bootNonce!=cb||coord->ip!=ip||
       (uint32_t)(millis()-coord->lastSeenMs)>60000UL)return;
    ++captureResultReceiptReceived;
    captureResultRetry=CaptureResultRetry{};
}
// SFHT1|tag|coordinator|coord_boot_hex|epoch|target|target_boot_hex|seq|channel|utc_us
// Signed transport preparation only. Deliberately does not arm GPIOs until the
// coordinator-side measurement session and safe peripheral handoff exist.
static void processHwTimeRequest(char **f,size_t n,const IPAddress &ip) {
    if(n!=10 || !runtimeActive || localIsCoordinator ||
       strcmp(f[1],runtimeClusterTag)!=0 ||
       strcmp(f[2],currentCoordinatorId)!=0 ||
       strcmp(f[5],deviceIntegrationId().c_str())!=0 ||
       ip!=currentCoordinatorIp)return;
    uint32_t cb=0,epoch=0,nb=0,seq=0,channel=0;
    int64_t utc=0,nowUtc=0;
    if(!parseUint32Strict(f[3],cb,16)||!parseUint32Strict(f[4],epoch)||
       !parseUint32Strict(f[6],nb,16)||!parseUint32Strict(f[7],seq)||
       !parseUint32Strict(f[8],channel)||!parseInt64Strict(f[9],utc)||
       !cb||!epoch||!seq||nb!=bootNonce||epoch!=currentCoordinatorEpoch||
       channel>255)return;
    const ClusterPeer *coord=findPeer(f[2]);
    if(!coord||coord->bootNonce!=cb||coord->ip!=ip||
       (uint32_t)(millis()-coord->lastSeenMs)>60000UL)return;
    ClusterHwTimeProtocol::Request req={cb,epoch,seq,(uint8_t)channel,utc};
    if(!clusterTimeNowUs(nowUtc)||
       !ClusterHwTimeProtocol::validate(req,nowUtc,
           (hwTimeSeen.boot==cb && hwTimeSeen.epoch==epoch) ? hwTimeSeen.sequence : 0,
           droneModeEnabled(), CLUSTER_HW_TIME_OUT>=0))return;
    // Consume authenticated sequences before any future GPIO/resource operation.
    // Retransmissions must never produce a second impulse.
    if(!hwTimeSeen.accept(req))return;
    // Only XIAO GPIO2 currently has a dedicated output. Freenove GPIO48
    // shares the RGB LED and must NOT be driven before an explicit handoff.
    // The arm function freezes the UTC->monotonic mapping at acceptance.
    bool armed = false;
#if defined(BOARD_XIAO) && CLUSTER_HW_TIME_OUT == 2
    if (!clusterHwTimePulseActive()) armed = clusterHwTimePulseArm(utc);
#endif
    // ACK status 1 means the timer was actually armed, NOT that an edge was
    // observed. Status 2 explicitly means no output was armed.
    char nodeBoot[9];
    snprintf(nodeBoot,sizeof(nodeBoot),"%08lx",(unsigned long)bootNonce);
    const String ack=String(CLUSTER_JOB_HW_TIME_ACK_MAGIC)+"|"+runtimeClusterTag+"|"+
        currentCoordinatorId+"|"+f[3]+"|"+f[4]+"|"+deviceIntegrationId()+"|"+
        nodeBoot+"|"+f[7]+"|"+f[8]+"|"+(armed ? "1" : "2");
    sendSignedPayloadTo(ack,ip,CLUSTER_COORDINATOR_PORT,"hardware time arm ACK");
}
// SFHTA1|tag|coordinator|coord_boot|epoch|node|node_boot|seq|channel|status
// status 1 means timer armed; status 2 means output unavailable.
static void processHwTimeAck(char **f,size_t n,const IPAddress &ip) {
    if(n!=10||!runtimeActive||!localIsCoordinator||
       strcmp(f[1],runtimeClusterTag)!=0||
       strcmp(f[2],deviceIntegrationId().c_str())!=0||
       !integrationIdValid(f[5]))return;
    uint32_t cb=0,epoch=0,nb=0,seq=0,channel=0,status=0;
    if(!parseUint32Strict(f[3],cb,16)||!parseUint32Strict(f[4],epoch)||
       !parseUint32Strict(f[6],nb,16)||!parseUint32Strict(f[7],seq)||
       !parseUint32Strict(f[8],channel)||!parseUint32Strict(f[9],status)||
       cb!=bootNonce||epoch!=currentCoordinatorEpoch||!nb||!seq||
       channel<1||channel>3||(status!=1&&status!=2))return;
    const ClusterPeer *node=findPeer(f[5]);
    if(!node||node->bootNonce!=nb||node->ip!=ip||
       (uint32_t)(millis()-node->lastSeenMs)>60000UL)return;
    if (hwSession.active && epoch == hwSession.epoch) {
        const uint8_t idx = (uint8_t)(channel - 1);
        if (hwSession.nodes[idx] == f[5] && hwSession.seq[idx] == seq &&
            hwSession.armStatus[idx] == 0) {
            hwSession.armStatus[idx] = (uint8_t)status;
        }
    }
    // Informational only until a coordinator measurement session is wired.
    Serial.printf("[CLUSTER HW TIME] node=%s seq=%lu channel=%lu timer=%s\n",
                  f[5],(unsigned long)seq,(unsigned long)channel,
                  status==1 ? "ARMED" : "NOT ARMED");
}
static void processJobCapture(char **f,size_t n,const IPAddress &ip) {
    bool testPulse=false;
    if(!ClusterCaptureOptions::parse(n,f,testPulse) || !runtimeActive || localIsCoordinator || strcmp(f[1],runtimeClusterTag)!=0 ||
       strcmp(f[2],currentCoordinatorId)!=0 || strcmp(f[5],deviceIntegrationId().c_str())!=0 ||
       ip!=currentCoordinatorIp)return;
    uint32_t cb=0,epoch=0,nb=0,seq=0;
    if(!parseUint32Strict(f[3],cb,16)||!parseUint32Strict(f[4],epoch)||
       !parseUint32Strict(f[6],nb,16)||!parseUint32Strict(f[7],seq)||
       !cb||!epoch||!seq||nb!=bootNonce||epoch!=currentCoordinatorEpoch)return;
    const ClusterPeer *coord=findPeer(f[2]);
    if(!coord||coord->bootNonce!=cb||coord->ip!=ip||(uint32_t)(millis()-coord->lastSeenMs)>60000UL)return;
    // Always consume valid sequence before deciding readiness: no replay trigger.
    if(captureSeenBoot==cb&&captureSeenEpoch==epoch&&seq<=captureSeenSeq)return;
    captureSeenBoot=cb;captureSeenEpoch=epoch;captureSeenSeq=seq;
    char *end=nullptr;errno=0;
    int64_t utc=(int64_t)strtoll(f[8],&end,10);
    int64_t nowUtc=0;
    if(errno||!end||*end||utc<=0||!clusterTimeNowUs(nowUtc)||utc-nowUtc<1500000LL||
       utc-nowUtc>1800000000LL||pendingCapture.active) {
        sendCaptureResult(cb,epoch,seq,3,1,0);return;
    }
    // Capture is more important than an in-progress precision series.
    // Pause BEFORE accepting the shot; retain the last calibrated time model.
    ClusterEspNowRadio::pauseForCapture(
        static_cast<uint32_t>((utc - nowUtc) / 1000LL) + 5000UL);
    // Freeze the mapping immediately, before sending the acceptance ACK.
    // Future cluster/NTP adjustments only influence *future* jobs.
    const int64_t acceptedMonoUs=esp_timer_get_time();
    const int64_t remainingUs=utc-nowUtc;
    captureResultRetry=CaptureResultRetry{};
    clusterCaptureBeginJob(((uint64_t)cb<<32)|seq);
    // Beta131-FIX (Eighth cycle, Problem 2): Store time source generation
    // CRITICAL: Capture must detect source changes even within same epoch
    pendingCapture={true,cb,epoch,seq,utc,nowUtc,acceptedMonoUs,
                    acceptedMonoUs+remainingUs,
                    ClusterEspNowRadio::timeSourceGeneration(),testPulse};
    captureTestPulseState=testPulse?"requested":"off";
    sendCaptureResult(cb,epoch,seq,1,0,0);
}
static void processJobCaptureResult(char **f,size_t n,const IPAddress &ip) {
    if(n!=13||!runtimeActive||!localIsCoordinator||strcmp(f[1],runtimeClusterTag)!=0||
       strcmp(f[2],deviceIntegrationId().c_str())!=0||!integrationIdValid(f[5]))return;
    uint32_t cb=0,epoch=0,nb=0,seq=0,status=0,error=0,bytes=0;
    if(!parseUint32Strict(f[3],cb,16)||!parseUint32Strict(f[4],epoch)||
       !parseUint32Strict(f[6],nb,16)||!parseUint32Strict(f[7],seq)||
       !parseUint32Strict(f[8],status)||!parseUint32Strict(f[9],error)||
       !parseUint32Strict(f[10],bytes)||cb!=bootNonce||epoch!=currentCoordinatorEpoch||
       !nb||!seq||status<1||status>3||error>65535)return;
    char *dispatchEnd=nullptr;errno=0;
    int64_t dispatch=(int64_t)strtoll(f[12],&dispatchEnd,10);
    if(errno||!dispatchEnd||*dispatchEnd)return;
    const ClusterPeer *peer=findPeer(f[5]);
    if(!peer||peer->bootNonce!=nb||peer->ip!=ip||(uint32_t)(millis()-peer->lastSeenMs)>60000UL)return;
    uint64_t id=((uint64_t)cb<<32)|seq;
    const ClusterJobs::Entry *e=ClusterJobs::find(id);
    if(!e||e->kind!=ClusterJobs::Kind::Capture||strcmp(e->nodeId,f[5])!=0)return;
    // Only a still-open capture may accept its one immutable media result.
    // Check the job deadline using one timestamp shared with transitions.
    const uint32_t resultNowMs=millis();
    if(status==2){
        if((e->state!=ClusterJobs::State::Accepted && e->state!=ClusterJobs::State::Sent) ||
           static_cast<int32_t>(resultNowMs-e->deadlineMs)>=0 ||
           !ClusterJobs::setCaptureMedia(id,epoch,f[11],dispatch,bytes))return;
    }
    if(status==1&&e->state==ClusterJobs::State::Sent){
        if(ClusterJobs::transition(id,epoch,ClusterJobs::State::Accepted,resultNowMs))++captureAccepted;
    } else if(status==2&&(e->state==ClusterJobs::State::Accepted||e->state==ClusterJobs::State::Sent)){
        if(e->state==ClusterJobs::State::Sent)ClusterJobs::transition(id,epoch,ClusterJobs::State::Accepted,resultNowMs);
        ClusterJobs::transition(id,epoch,ClusterJobs::State::Running,resultNowMs);
        if(ClusterJobs::transition(id,epoch,ClusterJobs::State::Succeeded,resultNowMs))++captureCompleted;
        captureLastSize=bytes;
    } else if(status==3&&(e->state==ClusterJobs::State::Sent||e->state==ClusterJobs::State::Accepted)){
        if(ClusterJobs::transition(id,epoch,ClusterJobs::State::Failed,resultNowMs,(uint16_t)error))++captureFailed;
    }
    // ACK both first delivery and exact duplicates. Terminal state, immutable
    // media and result-code checks prevent forged/late status from changing a job.
    const ClusterJobs::Entry *after=ClusterJobs::find(id);
    if(after && ((status==2&&after->state==ClusterJobs::State::Succeeded&&
                  after->mediaBytes==bytes&&after->dispatchUtcUs==dispatch&&
                  strcmp(after->mediaUuid,f[11])==0)||
                 (status==3&&after->state==ClusterJobs::State::Failed&&
                  after->resultCode==(uint16_t)error)))
        sendCaptureReceipt(cb,epoch,seq,nb,(uint16_t)status,ip);
}
// Returns GPIO2 after the shot or on any abort. Without a prepared pin only a
// still pending request is closed, so a final reason (not_prepared, refusal)
// is kept and the status never shows "requested" for an ended job.
static void releaseCaptureTestPulse(const char *state){
    if(clusterCapturePulsePrepared())clusterCapturePulseRelease();
    else if(strcmp(captureTestPulseState,"requested"))return;
    captureTestPulseState=state;
}
// Called by clusterLoop during regular firmware main loop. Future high precision
// backends must replace the dispatch mechanism, not the shared UTC contract.
static void serviceScheduledCapture(){
    if(!pendingCapture.active)return;

    // Beta131-FIX (Eighth cycle, Problem 2): Abort capture on time source change
    // CRITICAL: Planned captures must NOT execute if coordinator/epoch changed
    // Epoch check detects cross-epoch switches, generation check detects same-epoch switches
    const uint32_t currentGeneration = ClusterEspNowRadio::timeSourceGeneration();
    if(pendingCapture.epoch != currentCoordinatorEpoch) {
        Serial.printf("[CLUSTER CAPTURE] ABORTED - epoch changed (%u -> %u)\n",
            pendingCapture.epoch, currentCoordinatorEpoch);
        PendingCapture p=pendingCapture;
        pendingCapture.active=false;
        clusterCaptureArmedFlag=false;
        releaseCaptureTestPulse("aborted");
        queueCaptureTerminalResult(p.cb,p.epoch,p.seq,3,2,0);  // Aborted due to source change
        return;
    }
    // Beta131-FIX (Eighth cycle, Problem 2): Detect source changes within same epoch
    // CRITICAL: Coordinator ↔ Broadcaster switch invalidates time model even if epoch unchanged
    if(pendingCapture.sourceGeneration != currentGeneration) {
        Serial.printf("[CLUSTER CAPTURE] ABORTED - source generation changed (%u -> %u) within epoch %u\n",
            pendingCapture.sourceGeneration, currentGeneration, currentCoordinatorEpoch);
        PendingCapture p=pendingCapture;
        pendingCapture.active=false;
        clusterCaptureArmedFlag=false;
        releaseCaptureTestPulse("aborted");
        queueCaptureTerminalResult(p.cb,p.epoch,p.seq,3,2,0);  // Aborted due to source change
        return;
    }

    // Deliberately do not call clusterTimeNowUs() here. A capture accepted
    // at UTC T owns a fixed monotonic deadline even if the coordinator's
    // NTP clock jumps, the node receives a new sync sample, or WiFi drops.
    const int64_t nowMonoUs=esp_timer_get_time();
    const int64_t leftUs=pendingCapture.deadlineMonoUs-nowMonoUs;
    if(leftUs>0){
        if(leftUs<=2000000LL && !clusterCaptureArmedFlag){
            if(!clusterCapturePrepareCamera()){
                PendingCapture p=pendingCapture;pendingCapture.active=false;
                clusterCaptureArmedFlag=false;
                releaseCaptureTestPulse("camera_failed");
                queueCaptureTerminalResult(p.cb,p.epoch,p.seq,3,1,0);return;
            }
            clusterCaptureArmedFlag=true;
            // Configure the test-pulse output now, outside the time-critical
            // dispatch. A refused pin never blocks the shot itself.
            if(pendingCapture.testPulse){
                const char *reason="";
                captureTestPulseState=clusterCapturePulsePrepare(reason)?"prepared":reason;
            }
        }
        return;
    }
    PendingCapture p=pendingCapture;pendingCapture.active=false;
    uint32_t bytes=0;int64_t dispatch=0;
    uint16_t rc=clusterCapturePhoto(((uint64_t)p.cb<<32)|p.seq,p.utc,dispatch,bytes,
                                    p.anchorUtcUs,p.anchorMonoUs,
                                    p.testPulse&&clusterCapturePulsePrepared());
    if(p.testPulse&&!clusterCapturePulsePrepared()&&!strcmp(captureTestPulseState,"requested"))
        captureTestPulseState="not_prepared";  // arming step was never reached
    releaseCaptureTestPulse(clusterCapturePulseEmitted()?"emitted":"not_emitted");
    captureLastDispatchOffsetUs=dispatch?dispatch-p.utc:0;
    captureLastSize=bytes;captureLastError=rc;
    clusterCaptureArmedFlag=false;
    char mediaUuid[37]={};int64_t actualDispatch=0;
    if(!rc&&!clusterCaptureLatestMedia(((uint64_t)p.cb<<32)|p.seq,mediaUuid,actualDispatch))rc=5;
    queueCaptureTerminalResult(p.cb,p.epoch,p.seq,rc?3:2,rc,bytes,mediaUuid,actualDispatch);
    Serial.printf("[CLUSTER CAPTURE] seq=%lu rc=%u jpeg=%lu dispatch_offset_us=%lld (frozen clock; NOT exposure)\n",
        (unsigned long)p.seq,(unsigned)rc,(unsigned long)bytes,(long long)captureLastDispatchOffsetUs);
}

// SFJW1|tag|coordinator|coord_boot|epoch|target|target_boot|seq
// Every accepted destructive command is consumed before queuing. No retries.
static void sendHeartbeat();
static void sendClusterWipeResult(uint32_t cb, uint32_t epoch, uint32_t seq,
                                  uint32_t /*unused*/, uint16_t result)
{
    if (!runtimeActive || currentCoordinatorEpoch != epoch ||
        !usableIp(currentCoordinatorIp)) return;
    char cbText[9], ownBoot[9];
    snprintf(cbText,sizeof(cbText),"%08lx",(unsigned long)cb);
    snprintf(ownBoot,sizeof(ownBoot),"%08lx",(unsigned long)bootNonce);
    const String message=String(CLUSTER_JOB_WIPE_RESULT_MAGIC)+"|"+runtimeClusterTag+
        "|"+currentCoordinatorId+"|"+cbText+"|"+String((unsigned long)epoch)+
        "|"+deviceIntegrationId()+"|"+ownBoot+"|"+String((unsigned long)seq)+
        "|"+String((unsigned)result);
    if (sendSignedPayloadTo(message,currentCoordinatorIp,CLUSTER_COORDINATOR_PORT,"wipe completed"))
        ++wipeResultSent;
    // SD maintenance finished (success or error): invalidate stale accounting
    // and immediately send a fresh authenticated presence + health sidecar.
    // A failed filesystem read will report unknown, never the old capacity.
    healthForceSdRefresh=true;
    sendHeartbeat();
}
static void processJobWipe(char **f,size_t n,const IPAddress &ip)
{
    ++wipeReceived;
    if (n!=8 || !runtimeActive || localIsCoordinator ||
        strcmp(f[1],runtimeClusterTag)!=0 ||
        strcmp(f[2],currentCoordinatorId)!=0 ||
        strcmp(f[5],deviceIntegrationId().c_str())!=0 || ip!=currentCoordinatorIp) return;
    uint32_t cb=0,epoch=0,nb=0,seq=0;
    if (!parseUint32Strict(f[3],cb,16) || !parseUint32Strict(f[4],epoch) ||
        !parseUint32Strict(f[6],nb,16) || !parseUint32Strict(f[7],seq) ||
        !cb || !epoch || !seq || nb!=bootNonce || epoch!=currentCoordinatorEpoch) return;
    const ClusterPeer *coord=findPeer(f[2]);
    if (!coord || coord->bootNonce!=cb || coord->ip!=ip ||
        (uint32_t)(millis()-coord->lastSeenMs)>60000UL) return;
    if (wipeSeenBoot==cb && wipeSeenEpoch==epoch && seq<=wipeSeenSeq) return;
    // Consume even when not ready. Repeated signed commands MUST NOT rewipe.
    wipeSeenBoot=cb;wipeSeenEpoch=epoch;wipeSeenSeq=seq;
    if (!webSdQueueClusterWipe(cb,epoch,seq,sendClusterWipeResult)) {
        // One authenticated terminal refusal: unavailable/busy.
        sendClusterWipeResult(cb,epoch,seq,0,100);
        return;
    }
    ++wipeQueued;
    Serial.printf("[CLUSTER SD] wipe queued seq=%lu\n",(unsigned long)seq);
}
static void processJobWipeResult(char **f,size_t n,const IPAddress &ip)
{
    ++wipeResultReceived;
    if (n!=9 || !runtimeActive || !localIsCoordinator ||
        strcmp(f[1],runtimeClusterTag)!=0 ||
        strcmp(f[2],deviceIntegrationId().c_str())!=0 || !integrationIdValid(f[5])) return;
    uint32_t cb=0,epoch=0,nb=0,seq=0,result=0;
    if (!parseUint32Strict(f[3],cb,16) || !parseUint32Strict(f[4],epoch) ||
        !parseUint32Strict(f[6],nb,16) || !parseUint32Strict(f[7],seq) ||
        !parseUint32Strict(f[8],result) || result>65535UL ||
        cb!=bootNonce || epoch!=currentCoordinatorEpoch || !nb || !seq) return;
    const ClusterPeer *peer=findPeer(f[5]);
    if (!peer || peer->bootNonce!=nb || peer->ip!=ip ||
        (uint32_t)(millis()-peer->lastSeenMs)>60000UL) return;
    const uint64_t id=((uint64_t)cb<<32)|seq;
    const ClusterJobs::Entry *e=ClusterJobs::find(id);
    if (!e || e->kind!=ClusterJobs::Kind::ClearRecordings ||
        strcmp(e->nodeId,f[5])!=0 || e->state!=ClusterJobs::State::Sent) return;
    if (result==0) {
        if (ClusterJobs::transition(id,epoch,ClusterJobs::State::Accepted,millis())) {
            ClusterJobs::transition(id,epoch,ClusterJobs::State::Running,millis());
            ClusterJobs::transition(id,epoch,ClusterJobs::State::Succeeded,millis());
        }
    } else {
        ClusterJobs::transition(id,epoch,ClusterJobs::State::Failed,millis(),(uint16_t)result);
    }
}

// Persistent Drone readiness: authenticated unicast, replay-protected, no retry.
// Payload SFJD1|tag|coordinator|coord_boot|epoch|node|node_boot|seq|enable
// Reply   SFJDA1|tag|coordinator|coord_boot|epoch|node|node_boot|seq|enabled|result
static void processJobDrone(char **f,size_t n,const IPAddress &ip){
    if(n!=9||!runtimeActive||localIsCoordinator||
       strcmp(f[1],runtimeClusterTag)!=0||strcmp(f[2],currentCoordinatorId)!=0||
       strcmp(f[5],deviceIntegrationId().c_str())!=0||ip!=currentCoordinatorIp)return;
    uint32_t cb=0,epoch=0,nb=0,seq=0,enable=0;
    if(!parseUint32Strict(f[3],cb,16)||!parseUint32Strict(f[4],epoch)||
       !parseUint32Strict(f[6],nb,16)||!parseUint32Strict(f[7],seq)||
       !parseUint32Strict(f[8],enable)||enable>1||!cb||!epoch||!seq||
       nb!=bootNonce||epoch!=currentCoordinatorEpoch)return;
    const ClusterPeer *coord=findPeer(f[2]);
    if(!coord||coord->bootNonce!=cb||coord->ip!=ip||
       (uint32_t)(millis()-coord->lastSeenMs)>60000UL)return;
    if(droneSeenBoot==cb&&droneSeenEpoch==epoch&&seq<=droneSeenSeq)return;
    droneSeenBoot=cb;droneSeenEpoch=epoch;droneSeenSeq=seq;
    const bool success=droneModeSet(enable!=0);
    char own[9];snprintf(own,sizeof(own),"%08lx",(unsigned long)bootNonce);
    String msg=String(CLUSTER_JOB_DRONE_ACK_MAGIC)+"|"+runtimeClusterTag+"|"+
        currentCoordinatorId+"|"+f[3]+"|"+f[4]+"|"+deviceIntegrationId()+"|"+
        own+"|"+f[7]+"|"+(droneModeEnabled()?"1":"0")+"|"+(success?"0":"1");
    sendSignedPayloadTo(msg,ip,CLUSTER_COORDINATOR_PORT,"drone mode result");
    Serial.printf("[CLUSTER DRONE] seq=%lu requested=%u result=%u\n",(unsigned long)seq,(unsigned)enable,success?0U:1U);
}
static void processJobDroneAck(char **f,size_t n,const IPAddress &ip){
    if(n!=10||!runtimeActive||!localIsCoordinator||
       strcmp(f[1],runtimeClusterTag)!=0||strcmp(f[2],deviceIntegrationId().c_str())!=0||
       !integrationIdValid(f[5]))return;
    uint32_t cb=0,epoch=0,nb=0,seq=0,enabled=0,result=0;
    if(!parseUint32Strict(f[3],cb,16)||!parseUint32Strict(f[4],epoch)||
       !parseUint32Strict(f[6],nb,16)||!parseUint32Strict(f[7],seq)||
       !parseUint32Strict(f[8],enabled)||!parseUint32Strict(f[9],result)||
       enabled>1||result>1||cb!=bootNonce||epoch!=currentCoordinatorEpoch||!nb||!seq)return;
    const ClusterPeer *node=findPeer(f[5]);
    if(!node||node->bootNonce!=nb||node->ip!=ip||
       (uint32_t)(millis()-node->lastSeenMs)>60000UL)return;
    const uint64_t id=((uint64_t)cb<<32)|seq;
    const ClusterJobs::Entry *e=ClusterJobs::find(id);
    if(!e||e->kind!=ClusterJobs::Kind::Drone||
       strcmp(e->nodeId,f[5])!=0||e->state!=ClusterJobs::State::Sent)return;
    if(result==0){
        ClusterJobs::transition(id,epoch,ClusterJobs::State::Accepted,millis());
        ClusterJobs::transition(id,epoch,ClusterJobs::State::Running,millis());
        ClusterJobs::transition(id,epoch,ClusterJobs::State::Succeeded,millis(),(uint16_t)enabled);
    }else ClusterJobs::transition(id,epoch,ClusterJobs::State::Failed,millis(),(uint16_t)result);
}

static void processPacket(
    char *packet,
    size_t packetLength,
    const IPAddress &remoteIp
)
{
    if (!authenticatePacket(packet, packetLength))
        return;

    char *fields[16] = {};
    size_t fieldCount = 0;
    if (!splitFields(
            packet,
            fields,
            sizeof(fields) / sizeof(fields[0]),
            fieldCount
        ) ||
        fieldCount == 0) {
        return;
    }

    if (strcmp(fields[0], CLUSTER_PACKET_MAGIC) == 0) {
        processPresence(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_META_MAGIC) == 0) {
        processMeta(fields, fieldCount);
    } else if (strcmp(fields[0], CLUSTER_HEALTH_MAGIC) == 0) {
        processHealth(fields, fieldCount);
    } else if (strcmp(fields[0], CLUSTER_LEAVE_MAGIC) == 0) {
        processLeave(fields, fieldCount);
    } else if (strcmp(fields[0], CLUSTER_RESOURCE_MAGIC) == 0) {
        processResource(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_COORDINATION_MAGIC) == 0) {
        processCoordination(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_NODE_HEARTBEAT_MAGIC) == 0) {
        processNodeHeartbeat(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_NODE_ACK_MAGIC) == 0) {
        processNodeAck(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_TIME_REQUEST_MAGIC) == 0) {
        processTimeRequest(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_TIME_SYNC_COMMAND_MAGIC) == 0) {
        processTimeSyncCommand(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_TIME_REPLY_MAGIC) == 0) {
        processTimeReply(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_PROBE_MAGIC) == 0) {
        processJobProbe(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_ACK_MAGIC) == 0) {
        processJobProbeAck(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_RESTART_MAGIC) == 0) {
        processJobRestart(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_RESTART_ACK_MAGIC) == 0) {
        processJobRestartAck(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_SHUTDOWN_MAGIC) == 0) {
        processJobShutdown(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_SHUTDOWN_ACK_MAGIC) == 0) {
        processJobShutdownAck(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_HW_TIME_MAGIC) == 0) {
        processHwTimeRequest(fields,fieldCount,remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_HW_TIME_ACK_MAGIC) == 0) {
        processHwTimeAck(fields,fieldCount,remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_CAPTURE_MAGIC) == 0) {
        processJobCapture(fields,fieldCount,remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_CAPTURE_RESULT_MAGIC) == 0) {
        processJobCaptureResult(fields,fieldCount,remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_CAPTURE_RECEIPT_MAGIC) == 0) {
        processCaptureReceipt(fields,fieldCount,remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_DRONE_MAGIC) == 0) {
        processJobDrone(fields,fieldCount,remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_DRONE_ACK_MAGIC) == 0) {
        processJobDroneAck(fields,fieldCount,remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_WIPE_MAGIC) == 0) {
        processJobWipe(fields,fieldCount,remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_WIPE_RESULT_MAGIC) == 0) {
        processJobWipeResult(fields,fieldCount,remoteIp);
    } else if (strcmp(fields[0], CLUSTER_JOB_REJECT_MAGIC) == 0) {
        processJobRefusal(fields, fieldCount, remoteIp);
    } else if (strcmp(fields[0], CLUSTER_TIME_REPORT_MAGIC) == 0) {
        processTimeReport(fields, fieldCount, remoteIp);
    }
}

static void sendDiscoveryAnnouncement()
{
    if (!runtimeActive)
        return;

    const String clusterId = String(runtimeClusterId);
    if (!clusterIdValid(clusterId) || runtimeCredentialEpoch == 0)
        return;

    String payload;
    payload.reserve(260);
    payload += CLUSTER_DISCOVERY_MAGIC;
    payload += '|';
    payload += clusterId;
    payload += '|';
    payload += String((unsigned long)runtimeCredentialEpoch);
    payload += '|';
    payload += deviceIntegrationId();
    payload += '|';
    payload += textToHex(runtimeClusterName);
    payload += '|';
    payload += String((unsigned long)runtimeCoordinatorPolicy);
    payload += '|';
    payload += localIsCoordinator ? '1' : '0';
    payload += '|';
    payload += safePacketText(String(SENSORFORGE_RELEASE_TAG), 31);

    if (payload.length() > CLUSTER_PACKET_MAX)
        return;

    if (!clusterUdp.beginMulticastPacket())
        return;
    clusterUdp.write(
        reinterpret_cast<const uint8_t *>(payload.c_str()),
        payload.length()
    );
    clusterUdp.endPacket();
}

static void sendHeartbeat()
{
    if (!runtimeActive)
        return;

    const bool recording = recorderIsOpen();
    const bool streaming =
        streamerRtspClientConnected() ||
        streamerHttpClientConnected();

    uint32_t flags = 0;
    if (recording)
        flags |= 0x01U;
    if (streaming)
        flags |= 0x02U;
    if (cfg_transport_mode)
        flags |= 0x04U;

    const bool validTime = timeIsValid();
    const int64_t epoch = validTime ? (int64_t)time(nullptr) : 0;
    const String hostname = safePacketText(cfg_hostname, 63);
    const String mode = safePacketText(cfg_operating_mode, 15);
    const String release = safePacketText(String(SENSORFORGE_RELEASE_TAG), 31);

    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lx", (unsigned long)bootNonce);

    const uint32_t heartbeatSequence = ++sequenceNumber;

    // Keep the original SFC1 presence packet wire-compatible with Beta 34/35.
    // New lease metadata is sent as a second tiny authenticated packet.
    String payload;
    payload.reserve(240);
    payload += CLUSTER_PACKET_MAGIC;
    payload += '|';
    payload += runtimeClusterTag;
    payload += '|';
    payload += deviceIntegrationId();
    payload += '|';
    payload += bootText;
    payload += '|';
    payload += String(heartbeatSequence);
    payload += '|';
    payload += String((unsigned long)(millis() / 1000UL));
    payload += '|';
    payload += String((long long)epoch);
    payload += '|';
    payload += validTime ? '1' : '0';
    payload += '|';
    payload += mode;
    payload += '|';
    payload += String(flags);
    payload += '|';
    payload += release;
    payload += '|';
    payload += hostname;

    if (!sendSignedPayload(payload, "cluster heartbeat"))
        return;

    String meta;
    meta.reserve(150);
    meta += CLUSTER_META_MAGIC;
    meta += '|';
    meta += runtimeClusterTag;
    meta += '|';
    meta += deviceIntegrationId();
    meta += '|';
    meta += bootText;
    meta += '|';
    meta += String(heartbeatSequence);
    meta += '|';
    meta += String(CLUSTER_DEFAULT_LEASE_SEC);
    meta += '|';
    meta += String(localWifiRemainingSeconds);

    if (!sendSignedPayload(meta, "cluster metadata"))
        return;

    // Public, unauthenticated discovery metadata is emitted only by active
    // cluster members. Passive scanners listen for it but never transmit.
    // Cache SD accounting: filesystem operations are not performed every 10 s.
    static uint32_t lastHealthReadMs=0;
    static uint32_t totalMiB=0, usedMiB=0;
    const uint32_t healthNow=millis();
    if (healthForceSdRefresh || lastHealthReadMs == 0 || (uint32_t)(healthNow-lastHealthReadMs) >= 60000UL) {
        healthForceSdRefresh=false;
        lastHealthReadMs=healthNow;
        totalMiB=0; usedMiB=0;
        if (sdReady && !recorderIsOpen()) {
            const uint64_t total=STORAGE.totalBytes();
            const uint64_t used=STORAGE.usedBytes();
            if (total>0 && used<=total) {
                totalMiB=(uint32_t)(total/(1024ULL*1024ULL));
                usedMiB=(uint32_t)(used/(1024ULL*1024ULL));
            }
        }
    }
    const float cpuC=thermalCpuTemperatureC();
    const int32_t tempDeci=isfinite(cpuC) ? (int32_t)roundf(cpuC*10.0f) : -1000;
    String health=String(CLUSTER_HEALTH_MAGIC)+"|"+runtimeClusterTag+"|"+
        deviceIntegrationId()+"|"+bootText+"|"+String(heartbeatSequence)+"|"+
        String(totalMiB)+"|"+String(usedMiB)+"|"+String(tempDeci)+"|"+
        String((unsigned long)ESP.getFreeHeap());
    healthLocalTotalMiB=totalMiB;
    healthLocalUsedMiB=usedMiB;
    sendSignedPayload(health, "cluster health");
    sendDiscoveryAnnouncement();

    if (
        lastError.startsWith("cluster heartbeat") ||
        lastError.startsWith("cluster metadata") ||
        lastError.startsWith("cluster HMAC")
    ) {
        lastError = "";
    }
}

static bool sendCoordination()
{
    if (!runtimeActive)
        return false;

    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lx", (unsigned long)bootNonce);

    String payload;
    payload.reserve(200);
    payload += CLUSTER_COORDINATION_MAGIC;
    payload += '|';
    payload += runtimeClusterTag;
    payload += '|';
    payload += deviceIntegrationId();
    payload += '|';
    payload += bootText;
    payload += '|';
    payload += String((unsigned long)++sequenceNumber);
    payload += '|';
    payload += String((unsigned long)runtimeCoordinatorPolicy);
    payload += '|';
    payload += String((unsigned long)CLUSTER_LOCAL_CAPABILITIES); // board capability bitset (display only)
    payload += '|';
    payload += currentCoordinatorId[0] ? currentCoordinatorId : "-";
    payload += '|';
    payload += String((unsigned long)currentCoordinatorEpoch);

    const bool ok = sendSignedPayload(payload, "cluster coordination");
    if (ok)
        coordinationDirty = false;
    return ok;
}

static void sendCoordinatorHeartbeat()
{
    if (!runtimeActive || localIsCoordinator ||
        !currentCoordinatorId[0] || currentCoordinatorEpoch == 0 ||
        !usableIp(currentCoordinatorIp)) {
        return;
    }

    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lx", (unsigned long)bootNonce);

    const uint32_t heartbeatSequence = ++directHeartbeatSequence;

    String payload;
    payload.reserve(220);
    payload += CLUSTER_NODE_HEARTBEAT_MAGIC;
    payload += '|';
    payload += runtimeClusterTag;
    payload += '|';
    payload += deviceIntegrationId();
    payload += '|';
    payload += bootText;
    payload += '|';
    payload += String((unsigned long)heartbeatSequence);
    payload += '|';
    payload += currentCoordinatorId;
    payload += '|';
    payload += String((unsigned long)currentCoordinatorEpoch);
    payload += '|';
    payload += String((unsigned long)runtimeCoordinatorPolicy);
    payload += '|';
    payload += String((unsigned long)CLUSTER_COORDINATOR_LEASE_SEC);
    payload += '|';
    payload += String((long)localWifiRemainingSeconds);

    if (sendSignedPayloadTo(
            payload,
            currentCoordinatorIp,
            CLUSTER_COORDINATOR_PORT,
            "cluster coordinator heartbeat"
        )) {
        ++diagHbSent;
        if (lastError.startsWith("cluster coordinator heartbeat"))
            lastError = "";
    } else {
        ++diagHbSendFailed;
    }
}

static void sendLeave()
{
    if (!runtimeActive || bootNonce == 0 || !runtimeHmacKey)
        return;

    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lx", (unsigned long)bootNonce);

    String payload;
    payload.reserve(100);
    payload += CLUSTER_LEAVE_MAGIC;
    payload += '|';
    payload += runtimeClusterTag;
    payload += '|';
    payload += deviceIntegrationId();
    payload += '|';
    payload += bootText;
    payload += '|';
    payload += String(++sequenceNumber);

    // Best effort only: shutdown must never be blocked by a failed goodbye.
    sendSignedPayload(payload, "cluster leave");
}

static bool configTag(char tag[17])
{
    if (!cfg_cluster_enabled || !cfg_cluster_name.length() || !cfg_cluster_password.length())
        return false;

    uint8_t key[32] = {};
    bool ok = deriveClusterKey(
        clusterEffectiveId(),
        cfg_cluster_password,
        key,
        tag
    );
    secureWipe(key, sizeof(key));
    return ok;
}

} // namespace

bool clusterIdValid(const String &clusterId)
{
    return clusterIdCStringValid(clusterId.c_str());
}

String clusterEffectiveId()
{
    String configured = cfg_cluster_id;
    configured.trim();
    configured.toLowerCase();
    if (clusterIdValid(configured))
        return configured;
    return legacyClusterIdFromName(cfg_cluster_name);
}

String clusterGenerateId()
{
    uint8_t bytes[16] = {};
    esp_fill_random(bytes, sizeof(bytes));
    char text[3 + 32 + 1];
    text[0] = 'c';
    text[1] = 'l';
    text[2] = '-';
    bytesToHex(bytes, sizeof(bytes), text + 3);
    secureWipe(bytes, sizeof(bytes));
    return String(text);
}

void clusterDiscoveryTouch()
{
    const uint32_t now = millis();
    discoveryScanLeaseUntilMs = now + CLUSTER_DISCOVERY_SCAN_LEASE_MS;

    // Discovery is strictly subordinate to the existing WiFi lifecycle.
    // Never call WiFi.begin/mode/AP start from here.
    if (runtimeActive || WiFi.getMode() == WIFI_MODE_NULL)
        return;

    if (discoverySocketActive)
        return;

    clusterUdp.stop();
    if (clusterUdp.beginMulticast(CLUSTER_MULTICAST_GROUP, CLUSTER_PORT))
        discoverySocketActive = true;
}

bool clusterDiscoveryScannerActive()
{
    const uint32_t now = millis();
    return discoveryScanWanted(now) && (runtimeActive || discoverySocketActive);
}

size_t clusterAuthenticatedPeerCount()
{
    size_t count = 0;
    const uint32_t now = millis();
    for (size_t i = 0; i < CLUSTER_MAX_PEERS; ++i) {
        if (!peers[i].used)
            continue;
        const uint64_t ageMs = (uint64_t)(uint32_t)(now - peers[i].lastSeenMs);
        if (ageMs <= (uint64_t)peerLeaseSeconds(peers[i]) * 1000ULL)
            ++count;
    }
    return count;
}

ClusterCredentialCheckResult clusterVerifyDiscoveredCredentials(
    const String &clusterId,
    uint32_t credentialEpoch,
    const String &password,
    String &error,
    uint32_t &verifiedEpoch
)
{
    error = "";
    verifiedEpoch = 0;

    String normalizedId = clusterId;
    normalizedId.trim();
    normalizedId.toLowerCase();
    if (!clusterIdValid(normalizedId) || credentialEpoch == 0 || !password.length()) {
        error = "Ungültige Cluster-Zugangsdaten";
        return CLUSTER_CREDENTIAL_INTERNAL_ERROR;
    }

    const uint32_t now = millis();
    expireDiscoveryNodes(now);
    expireDiscoveryProofs(now);

    bool clusterVisible = false;
    uint32_t discoveredEpoch = 0;
    bool conflictingEpochVisible = false;
    for (size_t i = 0; i < CLUSTER_MAX_DISCOVERY_NODES; ++i) {
        const ClusterDiscoveryNode &node = discoveryNodes[i];
        if (!node.used ||
            (uint32_t)(now - node.lastSeenMs) > CLUSTER_DISCOVERY_NODE_TTL_MS ||
            normalizedId != node.clusterId) {
            continue;
        }
        clusterVisible = true;
        if (discoveredEpoch == 0)
            discoveredEpoch = node.credentialEpoch;
        else if (node.credentialEpoch != discoveredEpoch)
            conflictingEpochVisible = true;
    }

    if (!clusterVisible)
        return CLUSTER_CREDENTIAL_NOT_DISCOVERED;

    // The browser/local credential epoch is not authoritative for a recovery
    // join. Reject only contradictory epochs advertised by remote nodes.
    if (conflictingEpochVisible) {
        error = "Für diese Cluster-ID sind gleichzeitig unterschiedliche Credential-Epochen sichtbar. Join wurde aus Sicherheitsgründen abgelehnt.";
        return CLUSTER_CREDENTIAL_EPOCH_MISMATCH;
    }

    uint8_t key[32] = {};
    char tag[17] = {};
    if (!deriveClusterKey(normalizedId, password, key, tag)) {
        secureWipe(key, sizeof(key));
        error = "Cluster-Passwort konnte nicht geprüft werden";
        return CLUSTER_CREDENTIAL_INTERNAL_ERROR;
    }

    bool proofAvailable = false;
    for (size_t i = 0; i < CLUSTER_MAX_DISCOVERY_NODES; ++i) {
        const ClusterDiscoveryNode &node = discoveryNodes[i];
        if (!node.used ||
            (uint32_t)(now - node.lastSeenMs) > CLUSTER_DISCOVERY_NODE_TTL_MS ||
            normalizedId != node.clusterId ||
            node.credentialEpoch != discoveredEpoch) {
            continue;
        }

        for (size_t p = 0; p < CLUSTER_DISCOVERY_PROOF_SLOTS; ++p) {
            const ClusterDiscoveryProof &proof = discoveryProofs[p];
            if (!proof.used || !(proof.sourceIp == node.sourceIp) ||
                (uint32_t)(now - proof.lastSeenMs) > CLUSTER_DISCOVERY_PROOF_TTL_MS) {
                continue;
            }

            // SFC1 and SFD1 are emitted back-to-back in one heartbeat cycle.
            // Pair only packets close in time so a stale Presence from a node
            // that just changed Cluster credentials cannot be mistaken for a
            // current wrong-password proof.
            const uint32_t pairSkewMs =
                proof.lastSeenMs >= node.lastSeenMs
                ? proof.lastSeenMs - node.lastSeenMs
                : node.lastSeenMs - proof.lastSeenMs;
            if (pairSkewMs > CLUSTER_DISCOVERY_PROOF_PAIR_WINDOW_MS)
                continue;

            proofAvailable = true;
            if (discoveryProofMatchesCredentials(proof, node, key, tag)) {
                secureWipe(key, sizeof(key));
                verifiedEpoch = discoveredEpoch;
                return CLUSTER_CREDENTIAL_VERIFIED;
            }
        }
    }

    secureWipe(key, sizeof(key));

    if (proofAvailable) {
        error = "Cluster-Passwort ist falsch. Einstellungen wurden nicht gespeichert.";
        return CLUSTER_CREDENTIAL_INVALID_PASSWORD;
    }

    error = "Cluster erkannt, aber noch kein signiertes Presence-Paket zur Passwortprüfung empfangen. Seite einige Sekunden geöffnet lassen und erneut speichern.";
    return CLUSTER_CREDENTIAL_PROOF_PENDING;
}

void clusterNetworkStart()
{
    clusterNetworkStop();

    runtimeConfiguredEnabled = cfg_cluster_enabled != 0;
    runtimeCoordinatorPolicy =
        coordinatorPolicyFromString(cfg_cluster_coordinator_policy);
    if (!runtimeConfiguredEnabled) {
        lastError = "";
        return;
    }

    if (!cfg_cluster_name.length() || !cfg_cluster_password.length()) {
        lastError = "cluster enabled without complete credentials";
        return;
    }

    const String effectiveId = clusterEffectiveId();
    if (!clusterIdValid(effectiveId)) {
        lastError = "cluster identity invalid";
        return;
    }

    uint8_t key[32] = {};
    if (!deriveClusterKey(
            effectiveId,
            cfg_cluster_password,
            key,
            runtimeClusterTag
        )) {
        secureWipe(key, sizeof(key));
        lastError = "cluster key derivation failed";
        return;
    }

    if (!importRuntimeHmacKey(key)) {
        secureWipe(key, sizeof(key));
        lastError = "cluster HMAC key import failed";
        return;
    }
    secureWipe(key, sizeof(key));

    if (cfg_cluster_credential_epoch == 0) {
        psa_destroy_key(runtimeHmacKey);
        runtimeHmacKey = 0;
        runtimeClusterTag[0] = '\0';
        lastError = "cluster identity invalid";
        return;
    }
    strncpy(runtimeClusterId, effectiveId.c_str(), sizeof(runtimeClusterId) - 1);
    runtimeCredentialEpoch = cfg_cluster_credential_epoch;
    runtimeClusterName = cfg_cluster_name;
    runtimeClusterPassword = cfg_cluster_password;

    clusterUdp.stop();
    coordinatorUdp.stop();
    if (!clusterUdp.beginMulticast(CLUSTER_MULTICAST_GROUP, CLUSTER_PORT)) {
        psa_destroy_key(runtimeHmacKey);
        runtimeHmacKey = 0;
        runtimeClusterTag[0] = '\0';
        lastError = "cluster multicast bind failed";
        return;
    }

    if (!coordinatorUdp.begin(CLUSTER_COORDINATOR_PORT)) {
        clusterUdp.stop();
        psa_destroy_key(runtimeHmacKey);
        runtimeHmacKey = 0;
        runtimeClusterTag[0] = '\0';
        lastError = "cluster coordinator unicast bind failed";
        return;
    }

    clearPeers();
    clearResources();
    runtimeProfileRemembered = false;
    discoverySocketActive = false;
    localWifiRemainingSeconds = -1;
    bootNonce = esp_random();
    if (bootNonce == 0)
        bootNonce = 1;
    diagHbSent = diagHbSendFailed = 0;
    diagAckSendAttempted = diagAckSendFailed = 0;
    diagAckAuthenticated = diagAckAccepted = diagAckRejected = 0;
    diagAckLastReject = "none";
    memset(diagElectionTrace, 0, sizeof(diagElectionTrace));
    diagElectionTraceNext = diagElectionTraceUsed = 0;
    sequenceNumber = 0;
    nextHeartbeatMs = 0;
    nextCoordinatorHeartbeatMs = 0;
    directHeartbeatSequence = 0;
    lastCoordinatorAckMs = 0;
    lastAckCoordinatorBoot = 0;
    lastCoordinatorAckSequence = 0;
    localIsCoordinator = false;
    localCoordinatorEpoch = 0;
    currentCoordinatorId[0] = '\0';
    currentCoordinatorIp = IPAddress();
    currentCoordinatorEpoch = 0;
    coordinationDirty = false;
    runtimeActive = true;
    mdnsServiceAdvertised = false;
    lastError = "";

    recomputeCoordinator(millis());

    consoleWrite(
        "CLUSTER",
        "Runtime ON | node=" + deviceIntegrationId() +
        " | cluster_id=" + String(runtimeClusterId) +
        " | credential_epoch=" + String((unsigned long)runtimeCredentialEpoch) +
        " | policy=" + String(coordinatorPolicyName(runtimeCoordinatorPolicy)) +
        " | group=" + CLUSTER_MULTICAST_GROUP.toString() +
        ":" + String(CLUSTER_PORT)
    );
}

void clusterNetworkStop()
{
    ClusterEspNowRadio::stop();
    hwTimeSessionFinish("network_stopped", false);
    // Like the session above: every WiFi-off path (window end, thermal,
    // streamer recovery) returns the meter pins before the caller may move
    // GPIO21 to RTC presence sampling. No-op when the meter is not running.
    ClusterHwTimeMeter::stop("stopped_network_stop");
    // A deliberate cluster lifecycle stop cancels the pending local dispatch.
    // Keep already produced PSRAM media until a new accepted capture job.
    releaseCaptureTestPulse("aborted");
    pendingCapture = PendingCapture{};
    captureResultRetry = CaptureResultRetry{};
    clusterCaptureArmedFlag = false;
    coordinatorCaptureGuardUntilMs=0;
    captureSeenBoot = captureSeenEpoch = captureSeenSeq = 0;
    // Invalidate volatile job bookkeeping on every runtime/credential lifecycle reset.
    ClusterJobs::reset();
    probeSeq = probeSeenBoot = probeSeenEpoch = probeSeenSeq = 0;
    restartSeenBoot = restartSeenEpoch = restartSeenSeq = 0;
    restartSent = restartReceived = restartAccepted = restartRejected = 0;
    restartAckReceived = restartAckAccepted = restartAckRejected = 0;
    restartAckLastReject = "none";
    restartLastReject = "none";
    shutdownSeenBoot = shutdownSeenEpoch = shutdownSeenSeq = 0;
    wipeSeenBoot=wipeSeenEpoch=wipeSeenSeq=0;
    shutdownSent = shutdownReceived = shutdownAccepted = shutdownRejected = 0;
    shutdownAckReceived = shutdownAckAccepted = shutdownAckRejected = 0;
    shutdownLastReject = shutdownAckLastReject = "none";
    restartEvidenceCount = 0;
    memset(restartEvidence, 0, sizeof(restartEvidence));
    probeJobsStarted = 0;
    probeReceived = probeAckSent = probeAckReceived = 0;
    probeRejected = probeAckRejected = 0;
    probeLastReject = probeAckLastReject = "none";
    if (runtimeActive) {
        // Best-effort signed goodbye before the owning WiFi lifecycle tears the
        // radio down. Unexpected power loss still falls back to the peer lease.
        sendLeave();
        consoleWrite("CLUSTER", "Runtime OFF");
    }

    clusterUdp.stop();
    coordinatorUdp.stop();
    clusterHwTimePulseCancel();
    runtimeActive = false;
    discoverySocketActive = false;
    discoveryScanLeaseUntilMs = 0;
    runtimeProfileRemembered = false;
    mdnsServiceAdvertised = false;
    nextHeartbeatMs = 0;
    nextCoordinatorHeartbeatMs = 0;
    directHeartbeatSequence = 0;
    lastCoordinatorAckMs = 0;
    lastAckCoordinatorBoot = 0;
    lastCoordinatorAckSequence = 0;
    localWifiRemainingSeconds = -1;
    sequenceNumber = 0;
    localIsCoordinator = false;
    localCoordinatorEpoch = 0;
    currentCoordinatorId[0] = '\0';
    currentCoordinatorIp = IPAddress();
    currentCoordinatorEpoch = 0;
    coordinationDirty = false;
    bootNonce = 0;
    runtimeClusterTag[0] = '\0';
    runtimeClusterId[0] = '\0';
    runtimeCredentialEpoch = 1;
    runtimeClusterName = "";
    runtimeClusterPassword = "";
    clearPeers();
    clearResources();
    clearDiscoveryNodes();
    clearDiscoveryProofs();

    if (runtimeHmacKey != 0) {
        psa_destroy_key(runtimeHmacKey);
        runtimeHmacKey = 0;
    }
    resetClusterTimeEstimate();
    timeSyncCommandSeq = 0;
    timeSyncLastCommandMs = 0;
    timeSyncCommandsReceived = 0;
    timeSyncCommandsSent = 0;
}

void clusterMdnsAdvertise()
{
    if (!runtimeActive || mdnsServiceAdvertised)
        return;

    if (!MDNS.addService(CLUSTER_SERVICE, CLUSTER_PROTO, CLUSTER_PORT)) {
        lastError = "cluster mDNS service registration failed";
        return;
    }

    // The Arduino-ESP32 const/String addServiceTxt overloads intentionally
    // return void; the underlying library logs individual TXT failures. Keep
    // the cluster UDP runtime independent from optional TXT metadata.
    MDNS.addServiceTxt(CLUSTER_SERVICE, CLUSTER_PROTO, "v", "4");
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "features",
        "lease,leave,resource,coordinator,passive-discovery"
    );
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "iid",
        deviceIntegrationId()
    );
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "cid",
        String(runtimeClusterId)
    );
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "cred_epoch",
        String((unsigned long)runtimeCredentialEpoch)
    );
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "name",
        runtimeClusterName
    );
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "tag",
        runtimeClusterTag
    );
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "mode",
        cfg_operating_mode
    );
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "release",
        SENSORFORGE_RELEASE_TAG
    );
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "coord_policy",
        coordinatorPolicyName(runtimeCoordinatorPolicy)
    );
    MDNS.addServiceTxt(
        CLUSTER_SERVICE,
        CLUSTER_PROTO,
        "coord_port",
        String(CLUSTER_COORDINATOR_PORT)
    );

    mdnsServiceAdvertised = true;

    consoleWrite(
        "CLUSTER",
        "mDNS service _" + String(CLUSTER_SERVICE) + "._" +
        String(CLUSTER_PROTO) + " | tag=" + runtimeClusterTag
    );
}

void clusterLoop()
{
    // Beta138: every device with an ACTIVE cluster runtime (coordinator, time
    // broadcaster - also without lease - and normal nodes) runs without WiFi modem
    // sleep (WIFI_PS_NONE); outside active cluster operation the Arduino default is
    // restored. Never touched while a capture deadline is protected.
    if (!clusterCaptureProtectionActive()) {
        WifiPowerPolicy::service(runtimeActive);
    }
    // Diagnostic ESP-NOW time transport only; never changes cluster clock.
    // Capture deadline takes precedence over any radio work.
    if (!clusterCaptureProtectionActive()) {
        if (cfg_operating_mode == "time_broadcaster") {
            // Beta131-CORRECTED (Third cycle): Broadcaster receives BOTH IDs (Problem 1 & 2 FIX)
            // 1. Own device ID - for diagnostic beacons and Grant identity verification
            // 2. Coordinator ID - for time reference validation in precisionUtcNow()
            ClusterEspNowRadio::serviceDiagnosticBroadcaster(runtimeActive && !localIsCoordinator,
                                                              currentCoordinatorEpoch,
                                                              deviceIntegrationId().c_str(),  // Broadcaster's OWN ID
                                                              currentCoordinatorId,  // Coordinator's ID
                                                              bootNonce);
        } else {
            ClusterEspNowRadio::service(runtimeActive && (localIsCoordinator || droneModeEnabled()),
                                       localIsCoordinator, currentCoordinatorEpoch,
                                       localIsCoordinator ? deviceIntegrationId().c_str() : currentCoordinatorId,
                                       bootNonce);
        }
    }
    hwTimeSessionService();
    // Passive GPIO meter: role independent, also without cluster runtime.
    ClusterHwTimeMeter::service();
    // Never keep a measurement output scheduled after Drone Mode has ended.
    if (clusterHwTimePulseActive() && (!runtimeActive || localIsCoordinator ||
        !droneModeEnabled())) clusterHwTimePulseCancel();
    // A previously accepted capture has an immutable monotonic deadline.
    // Service it BEFORE potentially expensive UDP authentication, peer expiry,
    // heartbeat and clock-maintenance work. Packet handling must never be
    // permitted to push an armed photo past its local deadline.
    if (runtimeActive && pendingCapture.active) {
        serviceScheduledCapture();
        if (pendingCapture.active && clusterCaptureArmedFlag &&
            pendingCapture.deadlineMonoUs - esp_timer_get_time() <= 50000LL) {
            // Reserve the last 50 ms for the local camera dispatcher.
            // No busy-wait: loopTask yields as usual in sensorforge.ino.
            return;
        }
    }
    const uint32_t now = millis();

    if (discoveryScanLeaseUntilMs != 0 &&
        !discoveryScanWanted(now)) {
        discoveryScanLeaseUntilMs = 0;
        if (!runtimeActive && discoverySocketActive) {
            clusterUdp.stop();
            discoverySocketActive = false;
        }
    }

    if (discoveryScanLeaseUntilMs != 0) {
        expireDiscoveryNodes(now);
        expireDiscoveryProofs(now);
    }

    if (!runtimeActive && !discoverySocketActive)
        return;

    // Process any already queued authenticated Presence/ACK traffic BEFORE
    // expiring a peer. Otherwise a delayed loop can discard a live Coordinator
    // just before consuming its waiting ACK, causing avoidable role churn.
    int processed = 0;
    while (processed < 4) {
        int packetSize = clusterUdp.parsePacket();
        if (packetSize <= 0)
            break;

        if ((size_t)packetSize > CLUSTER_PACKET_MAX) {
            clusterUdp.clear();
            ++processed;
            continue;
        }

        char packet[CLUSTER_PACKET_MAX + 1];
        int readLength = clusterUdp.read(packet, (size_t)packetSize);
        if (readLength > 0) {
            const IPAddress remoteIp = clusterUdp.remoteIP();

            // While the Cluster page is actively scanning, retain one recent
            // signed Presence packet per sender as a RAM-only credential proof.
            // Passive scanners still never transmit or join a cluster here.
            if (discoveryScanWanted(now) && readLength >= 5 &&
                memcmp(packet, "SFC1|", 5) == 0) {
                rememberDiscoveryPresenceProof(
                    packet,
                    (size_t)readLength,
                    remoteIp,
                    now
                );
            }

            if (readLength >= 5 &&
                memcmp(packet, "SFD1|", 5) == 0) {
                processDiscoveryDatagram(
                    packet,
                    (size_t)readLength,
                    remoteIp
                );
            } else if (runtimeActive) {
                processPacket(
                    packet,
                    (size_t)readLength,
                    remoteIp
                );
            }
        }
        ++processed;
    }

    // Passive scanner mode stops here. It never opens the coordinator socket,
    // never authenticates/join clusters and never transmits cluster traffic.
    if (!runtimeActive)
        return;

    int directProcessed = 0;
    while (directProcessed < 4) {
        int packetSize = coordinatorUdp.parsePacket();
        if (packetSize <= 0)
            break;

        if ((size_t)packetSize > CLUSTER_PACKET_MAX) {
            coordinatorUdp.clear();
            ++directProcessed;
            continue;
        }

        char packet[CLUSTER_PACKET_MAX + 1];
        int readLength = coordinatorUdp.read(packet, (size_t)packetSize);
        if (readLength > 0) {
            processPacket(
                packet,
                (size_t)readLength,
                coordinatorUdp.remoteIP()
            );
        }
        ++directProcessed;
    }

    // Packet handlers have now refreshed authenticated liveness. Expire only
    // after draining the bounded receive queues, using a fresh timestamp.
    // This retains the 60-second lease on actual silence and does not let
    // discovery-only packets or unauthenticated traffic renew membership.
    const uint32_t electionNow = millis();
    expirePeers(electionNow);
    expireResources(electionNow);
    recomputeCoordinator(electionNow);
    ClusterJobs::expire(electionNow);
    serviceProbeRetries(electionNow);

    if (nextHeartbeatMs == 0 || (int32_t)(now - nextHeartbeatMs) >= 0) {
        sendHeartbeat();
        sendCoordination();
        nextHeartbeatMs = now + CLUSTER_HEARTBEAT_MS;
    } else if (coordinationDirty) {
        sendCoordination();
    }

    if (!localIsCoordinator &&
        currentCoordinatorId[0] &&
        currentCoordinatorEpoch != 0 &&
        usableIp(currentCoordinatorIp) &&
        (nextCoordinatorHeartbeatMs == 0 ||
         (int32_t)(now - nextCoordinatorHeartbeatMs) >= 0)) {
        sendCoordinatorHeartbeat();
        nextCoordinatorHeartbeatMs = now + CLUSTER_COORDINATOR_HEARTBEAT_MS;
    }
    requestClusterTime(millis());
    // Newly accepted jobs are handled on the next loop iteration. Do not
    // spend additional UDP/clock work before dispatching an armed capture.
    serviceCaptureResultRetry(millis());

}

bool clusterLocalIsCoordinator()
{
    return runtimeActive && localIsCoordinator &&
           currentCoordinatorEpoch != 0 &&
           strcmp(currentCoordinatorId, deviceIntegrationId().c_str()) == 0;
}

bool clusterRequestTimeSync(String &error)
{
    if (!clusterLocalIsCoordinator()) {
        error = "Dieses Gerät ist nicht der aktive Coordinator.";
        return false;
    }
    if (!localUnixTimeUs()) {
        error = "Coordinator hat noch keine gültige Systemzeit.";
        return false;
    }
    // Limit interactive command rate. This affects only manual sync commands.
    const uint32_t now = millis();
    if (timeSyncLastCommandMs && (uint32_t)(now - timeSyncLastCommandMs) < 10000UL) {
        error = "Zeit-Sync wurde gerade erst angefordert.";
        return false;
    }
    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lx", (unsigned long)bootNonce);
    const uint32_t seq = ++timeSyncCommandSeq;
    const String payload = String(CLUSTER_TIME_SYNC_COMMAND_MAGIC) + "|" +
        runtimeClusterTag + "|" + deviceIntegrationId() + "|" + bootText +
        "|" + String((unsigned long)currentCoordinatorEpoch) + "|" +
        String((unsigned long)seq);
    if (!sendSignedPayload(payload, "manual cluster time sync")) {
        error = String(lastError);
        return false;
    }
    timeSyncLastCommandMs = now;
    ++timeSyncCommandsSent;
    return true;
}

// Manual selected-node time sync: same signed SFTS1 format as multicast,
// delivered by authenticated unicast. Other nodes do not receive the command.
bool clusterRequestNodeTimeSync(const String &nodeId, String &error)
{
    if (!clusterLocalIsCoordinator()) {
        error = "Nur der aktive Coordinator darf einen Zeitabgleich anfordern."; return false;
    }
    if (!localUnixTimeUs()) {
        error = "Coordinator hat noch keine gültige Systemzeit."; return false;
    }
    const ClusterPeer *peer = findPeer(nodeId.c_str());
    if (!peer || !peer->used || !usableIp(peer->ip) || peer->bootNonce == 0 ||
        (uint32_t)(millis() - peer->lastSeenMs) > 60000UL) {
        error = "Node ist nicht authentifiziert erreichbar."; return false;
    }
    if (rejectStandbyNodeAction(peer, error)) return false;
    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lx", (unsigned long)bootNonce);
    if (++timeSyncCommandSeq == 0) ++timeSyncCommandSeq;
    const String payload = String(CLUSTER_TIME_SYNC_COMMAND_MAGIC) + "|" +
        runtimeClusterTag + "|" + deviceIntegrationId() + "|" + bootText + "|" +
        String((unsigned long)currentCoordinatorEpoch) + "|" +
        String((unsigned long)timeSyncCommandSeq);
    if (!sendSignedPayloadTo(payload, peer->ip, CLUSTER_COORDINATOR_PORT,
                             "selected node time sync")) {
        error = String(lastError); return false;
    }
    ClusterPeer *updatedPeer = findPeer(nodeId.c_str());
    if (updatedPeer) {
        updatedPeer->selectedSyncSeq = timeSyncCommandSeq;
        updatedPeer->selectedSyncRequestedMs = millis();
    }
    ++timeSyncCommandsSent;
    return true;
}

// Administrator-triggerable transport test; no destructive operations.
bool clusterSendJobProbe(const String &nodeId, String &error)
{
    if (!clusterLocalIsCoordinator()) {
        error = "Nur der aktive Coordinator darf einen Probeauftrag senden.";
        return false;
    }
    const ClusterPeer *peer = findPeer(nodeId.c_str());
    if (!peer || !peer->used || !usableIp(peer->ip) ||
        peer->bootNonce == 0 ||
        (uint32_t)(millis() - peer->lastSeenMs) > 60000UL) {
        error = "Node ist nicht authentifiziert erreichbar.";
        return false;
    }
    if (++probeSeq == 0) ++probeSeq;
    const uint64_t id = ((uint64_t)bootNonce << 32) | probeSeq;
    const uint32_t now = millis();
    if (!ClusterJobs::enqueue(id, currentCoordinatorEpoch, peer->integrationId,
                              ClusterJobs::Kind::Probe, now, 15000UL)) {
        error = "Job-Liste voll oder ungültiger Probeauftrag.";
        return false;
    }
    ++probeJobsStarted;
    char cb[9], nb[9];
    snprintf(cb, sizeof(cb), "%08lx", (unsigned long)bootNonce);
    snprintf(nb, sizeof(nb), "%08lx", (unsigned long)peer->bootNonce);
    const String payload = String(CLUSTER_JOB_PROBE_MAGIC) + "|" + runtimeClusterTag +
        "|" + deviceIntegrationId() + "|" + cb + "|" +
        String((unsigned long)currentCoordinatorEpoch) + "|" +
        nodeId + "|" + nb + "|" + String((unsigned long)probeSeq);
    if (!sendSignedPayloadTo(payload, peer->ip, CLUSTER_COORDINATOR_PORT,
                             "job transport probe")) {
        ClusterJobs::transition(id, currentCoordinatorEpoch,
                                ClusterJobs::State::Failed, millis(), 1);
        error = "Probeversand fehlgeschlagen.";
        return false;
    }
    ClusterJobs::transition(id, currentCoordinatorEpoch,
                            ClusterJobs::State::Sent, millis());
    // Slots are bounded by the same job table; if full, the probe still works
    // as a single-shot request rather than allocating extra memory.
    for (size_t i = 0; i < ClusterJobs::kMaxJobs; ++i) {
        if (probeRetries[i].id == 0 ||
            !ClusterJobs::find(probeRetries[i].id) ||
            ClusterJobs::find(probeRetries[i].id)->state != ClusterJobs::State::Sent) {
            probeRetries[i] = { id, peer->bootNonce, millis(), 1 };
            break;
        }
    }
    return true;
}

// Explicit coordinator-side single-node restart. Never broadcasts or retries.
bool clusterRequestNodeRestart(const String &nodeId, String &error)
{
    if (!clusterLocalIsCoordinator()) {
        error = "Nur der aktive Coordinator darf Neustarts anfordern."; return false;
    }
    const ClusterPeer *peer = findPeer(nodeId.c_str());
    if (!peer || !peer->used || !usableIp(peer->ip) || peer->bootNonce == 0 ||
        (uint32_t)(millis() - peer->lastSeenMs) > 60000UL) {
        error = "Node ist nicht authentifiziert erreichbar."; return false;
    }
    if (rejectStandbyNodeAction(peer, error)) return false;
    if (++probeSeq == 0) ++probeSeq;
    const uint64_t id = ((uint64_t)bootNonce << 32) | probeSeq;
    if (!ClusterJobs::enqueue(id, currentCoordinatorEpoch, peer->integrationId,
                              ClusterJobs::Kind::Restart, millis(), 15000UL)) {
        error = "Job-Liste voll."; return false;
    }
    char cb[9], nb[9];
    snprintf(cb, sizeof(cb), "%08lx", (unsigned long)bootNonce);
    snprintf(nb, sizeof(nb), "%08lx", (unsigned long)peer->bootNonce);
    const String payload = String(CLUSTER_JOB_RESTART_MAGIC) + "|" + runtimeClusterTag +
        "|" + deviceIntegrationId() + "|" + cb + "|" +
        String((unsigned long)currentCoordinatorEpoch) + "|" +
        nodeId + "|" + nb + "|" + String((unsigned long)probeSeq);
    if (!sendSignedPayloadTo(payload, peer->ip, CLUSTER_COORDINATOR_PORT, "node restart")) {
        ClusterJobs::transition(id, currentCoordinatorEpoch,
                                ClusterJobs::State::Failed, millis(), 1);
        error = "Neustartauftrag konnte nicht gesendet werden."; return false;
    }
    // Keep original boot ID even when ACK is lost and the job times out.
    if (restartEvidenceCount < ClusterJobs::kMaxJobs) {
        RestartEvidence &e = restartEvidence[restartEvidenceCount++];
        e = RestartEvidence{};
        e.jobId = id;
        strncpy(e.nodeId, nodeId.c_str(), sizeof(e.nodeId) - 1);
        e.expectedOldBoot = peer->bootNonce;
    }
    ++restartSent;
    ClusterJobs::transition(id, currentCoordinatorEpoch, ClusterJobs::State::Sent, millis());
    return true;
}

// Explicit coordinator-only single-node shutdown. Never retries or broadcasts.
bool clusterRequestNodeShutdown(const String &nodeId, String &error)
{
    if (!clusterLocalIsCoordinator()) {
        error="Nur der aktive Coordinator darf Shutdown anfordern."; return false;
    }
    const ClusterPeer *peer=findPeer(nodeId.c_str());
    if (!peer || !peer->used || !usableIp(peer->ip) || peer->bootNonce == 0 ||
        (uint32_t)(millis()-peer->lastSeenMs)>60000UL) {
        error="Node ist nicht authentifiziert erreichbar."; return false;
    }
    if (rejectStandbyNodeAction(peer, error)) return false;
    if (++probeSeq == 0) ++probeSeq;
    const uint64_t id=((uint64_t)bootNonce<<32)|probeSeq;
    if (!ClusterJobs::enqueue(id, currentCoordinatorEpoch, peer->integrationId,
                              ClusterJobs::Kind::Shutdown, millis(), 15000UL)) {
        error="Job-Liste voll."; return false;
    }
    char cb[9], nb[9];
    snprintf(cb,sizeof(cb),"%08lx",(unsigned long)bootNonce);
    snprintf(nb,sizeof(nb),"%08lx",(unsigned long)peer->bootNonce);
    const String payload=String(CLUSTER_JOB_SHUTDOWN_MAGIC)+"|"+runtimeClusterTag+
        "|"+deviceIntegrationId()+"|"+cb+"|"+
        String((unsigned long)currentCoordinatorEpoch)+"|"+
        nodeId+"|"+nb+"|"+String((unsigned long)probeSeq);
    if (!sendSignedPayloadTo(payload,peer->ip,CLUSTER_COORDINATOR_PORT,"node shutdown")) {
        ClusterJobs::transition(id,currentCoordinatorEpoch,ClusterJobs::State::Failed,millis(),1);
        error="Shutdownauftrag konnte nicht gesendet werden."; return false;
    }
    ++shutdownSent;
    ClusterJobs::transition(id,currentCoordinatorEpoch,ClusterJobs::State::Sent,millis());
    return true;
}

// Signed hardware-clock diagnostic request. Transport only: deliberately not
// exposed in WebConfig until ACK, GPIO ownership, and capture are integrated.
bool clusterRequestNodeHwTime(const String &nodeId, uint8_t channel,
                              int64_t utcUs, String &error, uint32_t *sequenceOut) {
    if (!clusterLocalIsCoordinator() || !droneModeEnabled()) {
        error = "Hardware-Zeitmessung nur am Coordinator im Drone Mode.";
        return false;
    }
    // Do not send any requests while the local measurement pins are disabled.
    if (CLUSTER_HW_TIME_IN_1 < 0 || CLUSTER_HW_TIME_IN_2 < 0 ||
        CLUSTER_HW_TIME_IN_3 < 0) {
        error = "Hardware-Messeingaenge am Coordinator nicht konfiguriert.";
        return false;
    }
    int64_t nowUtc=0;
    if (channel < 1 || channel > ClusterHwTimeProtocol::kChannels ||
        !clusterTimeNowUs(nowUtc) || nowUtc <= 0 ||
        utcUs-nowUtc < ClusterHwTimeProtocol::kMinLeadUs ||
        utcUs-nowUtc > ClusterHwTimeProtocol::kMaxLeadUs) {
        error = "Ungueltiger Messkanal oder geplanter Messzeitpunkt.";
        return false;
    }
    const ClusterPeer *peer=findPeer(nodeId.c_str());
    if (!peer || !peer->used || !usableIp(peer->ip) || !peer->bootNonce ||
        (uint32_t)(millis()-peer->lastSeenMs)>60000UL) {
        error = "Mess-Node nicht erreichbar.";
        return false;
    }
    if (rejectStandbyNodeAction(peer, error)) return false;
    if (++probeSeq==0) ++probeSeq;
    char cb[9],nb[9],when[30];
    snprintf(cb,sizeof(cb),"%08lx",(unsigned long)bootNonce);
    snprintf(nb,sizeof(nb),"%08lx",(unsigned long)peer->bootNonce);
    snprintf(when,sizeof(when),"%lld",(long long)utcUs);
    const String cmd=String(CLUSTER_JOB_HW_TIME_MAGIC)+"|"+runtimeClusterTag+"|"+
        deviceIntegrationId()+"|"+cb+"|"+String((unsigned long)currentCoordinatorEpoch)+
        "|"+nodeId+"|"+nb+"|"+String((unsigned long)probeSeq)+"|"+
        String((unsigned)channel)+"|"+when;
    if (!sendSignedPayloadTo(cmd,peer->ip,CLUSTER_COORDINATOR_PORT,
                             "hardware clock diagnostic")) {
        error = "Hardware-Messauftrag nicht gesendet.";
        return false;
    }
    if (sequenceOut) *sequenceOut = probeSeq;
    return true;
}

// Safe coordinator-side one-shot session; no automatic repeated triggering.
// Deliberately callable only after explicit GPIO pin map qualification.
bool clusterHwTimeStart(const String nodeIds[3], String &error) {
    if (!nodeIds) { error = "Node-Liste fehlt."; return false; }
    if (hwSession.active) { error = "Messung bereits aktiv."; return false; }
    if (!clusterLocalIsCoordinator() || !droneModeEnabled()) {
        error = "Coordinator muss im Drone Mode sein."; return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (nodeIds[i].isEmpty()) { error = "Drei Nodes erforderlich."; return false; }
        for (int j = 0; j < i; ++j) if (nodeIds[i] == nodeIds[j]) {
            error = "Nodes muessen unterschiedlich sein."; return false;
        }
        const ClusterPeer *peer = findPeer(nodeIds[i].c_str());
        if (!peer || !peer->used || !usableIp(peer->ip) || !peer->bootNonce ||
            (uint32_t)(millis() - peer->lastSeenMs) > 60000UL) {
            error = "Mess-Node nicht erreichbar: " + nodeIds[i]; return false;
        }
        if (rejectStandbyNodeAction(peer, error)) return false;
    }
    int64_t nowUtc = 0;
    if (!clusterTimeNowUs(nowUtc)) { error = "Cluster-Uhr nicht bereit."; return false; }
    if (!ClusterHwTimeGpioLease::acquire(error)) return false;
    // Bind UTC and mono clock as closely as possible. No RTC adjustment.
    const int64_t anchorMono = esp_timer_get_time();
    int64_t anchorUtc = 0;
    if (!clusterTimeNowUs(anchorUtc)) {
        ClusterHwTimeGpioLease::release();
        error = "Cluster-Uhr verloren."; return false;
    }
    const int64_t utc = anchorUtc + 5000000LL; // 5 seconds lead
    hwSession = HwTimeSession{};
    hwSession.active = true;
    hwSession.targetUtcUs = utc;
    hwSession.referenceMonoUs = anchorMono + 5000000LL;
    hwSession.epoch = currentCoordinatorEpoch;
    hwSession.startedMs = millis();
    for (int i = 0; i < 3; ++i) hwSession.nodes[i] = nodeIds[i];
    // Never send a partial set by accident if the pins could not be acquired.
    // Once sent, failed peers cannot be cancelled remotely; keep sampling and
    // mark missing edges, rather than claiming an atomic multi-node command.
    bool sentAll = true;
    for (int i = 0; i < 3; ++i) {
        uint32_t seq = 0;
        String sendError;
        if (!clusterRequestNodeHwTime(nodeIds[i], (uint8_t)(i + 1), utc,
                                     sendError, &seq)) {
            sentAll = false;
            error = "Kanal " + String(i + 1) + ": " + sendError;
            break;
        }
        hwSession.seq[i] = seq;
    }
    if (!sentAll) {
        // No new commands issued for remaining nodes. Previously sent nodes
        // can still pulse; keep inputs reserved through the deadline.
        hwSessionLastResult = "partially_sent";
        return false;
    }
    hwSessionLastResult = "waiting_for_edges";
    return true;
}

// CSV output uses the coordinator's monotonic timestamp as an observation axis.
// It is not an absolute UTC timestamp or a calibrated hardware-capture clock.
String clusterHwTimeCsv() {
    String csv = "sample,reference_mono_us,node1_ms,node2_ms,node3_ms,node1_minus_2_ms,node1_minus_3_ms,node2_minus_3_ms\n";
    ClusterHwTimeSeries::Row row = {};
    for (size_t i = 0; i < hwHistory.size(); ++i) {
        if (!hwHistory.get(i, row)) continue;
        csv += String(row.sequence) + "," + String((long long)row.referenceUs);
        for (int ch = 0; ch < 3; ++ch) {
            csv += ",";
            if (row.offsets.valid[ch]) csv += String(row.offsets.offsetMs[ch], 4);
        }
        for (int pair = 0; pair < 3; ++pair) {
            csv += ",";
            if (row.offsets.pairValid[pair]) csv += String(row.offsets.pairOffsetMs[pair], 4);
        }
        csv += "\n";
    }
    return csv;
}

String clusterHwTimeStatusJson() {
    String json = "{\"active\":";
    json += hwSession.active ? "true" : "false";
    json += ",\"state\":\"" + hwSessionLastResult + "\"";
    json += ",\"rows\":" + String((unsigned)hwHistory.size());
    json += ",\"arm\":[";
    for (int i = 0; i < 3; ++i) {
        if (i) json += ',';
        json += String(hwSession.armStatus[i]);
    }
    json += "],\"drift_ppm\":[";
    for (int i = 0; i < 3; ++i) {
        if (i) json += ',';
        double ppm = 0;
        json += hwHistory.drift(i, ppm) ? String(ppm, 2) : String("null");
    }
    json += "],\"samples\":[";
    // Small bounded history for the browser; full history available as CSV.
    const size_t total = hwHistory.size();
    const size_t first = total > 96 ? total - 96 : 0;
    ClusterHwTimeSeries::Row row = {};
    for (size_t i = first; i < total; ++i) {
        if (!hwHistory.get(i, row)) continue;
        if (i != first) json += ',';
        json += "{\"n\":" + String(row.sequence) + ",\"ms\":[";
        for (int ch = 0; ch < 3; ++ch) {
            if (ch) json += ',';
            json += row.offsets.valid[ch] ? String(row.offsets.offsetMs[ch], 4) : String("null");
        }
        json += "]}";
    }
    json += "]}";
    return json;
}

bool clusterRequestNodeCapture(const String &nodeId,int64_t utcUs,String &error,bool testPulse){
    if(!clusterLocalIsCoordinator()){error="Nur am Coordinator.";return false;}
    int64_t nowUtc=0;
    if(!clusterTimeNowUs(nowUtc)||utcUs-nowUtc<2000000LL||utcUs-nowUtc>1800000000LL){
        error="Capture-Zeit muss 2 Sekunden bis 30 Minuten in der Zukunft liegen.";return false;
    }
    const ClusterPeer *peer=findPeer(nodeId.c_str());
    if(!peer||!peer->used||!usableIp(peer->ip)||!peer->bootNonce||
       (uint32_t)(millis()-peer->lastSeenMs)>60000UL){error="Node nicht erreichbar.";return false;}
    if (rejectStandbyNodeAction(peer, error)) return false;
    if(++probeSeq==0)++probeSeq;
    uint64_t id=((uint64_t)bootNonce<<32)|probeSeq;
    if(!ClusterJobs::enqueue(id,currentCoordinatorEpoch,peer->integrationId,
        ClusterJobs::Kind::Capture,millis(),1830000UL)){error="Job-Liste voll.";return false;}
    char cb[9],nb[9],when[30];
    snprintf(cb,sizeof(cb),"%08lx",(unsigned long)bootNonce);
    snprintf(nb,sizeof(nb),"%08lx",(unsigned long)peer->bootNonce);
    snprintf(when,sizeof(when),"%lld",(long long)utcUs);
    // Local admission guard protects SD/OTA as well as ESP-NOW from dispatch onward.
    const uint32_t guardMs=static_cast<uint32_t>((utcUs-nowUtc)/1000LL)+5000UL;
    const uint32_t guardUntil=millis()+guardMs;
    if (!coordinatorCaptureGuardUntilMs ||
        static_cast<int32_t>(guardUntil-coordinatorCaptureGuardUntilMs)>0)
        coordinatorCaptureGuardUntilMs=guardUntil;
    // Suppress coordinator broadcasts/probe replies immediately on dispatch.
    // Later requests may extend the hold, never shorten it.
    ClusterEspNowRadio::pauseForCapture(
        static_cast<uint32_t>((utcUs - nowUtc) / 1000LL) + 5000UL);
    const String cmd=String(CLUSTER_JOB_CAPTURE_MAGIC)+"|"+runtimeClusterTag+"|"+
        deviceIntegrationId()+"|"+cb+"|"+String((unsigned long)currentCoordinatorEpoch)+
        "|"+nodeId+"|"+nb+"|"+String((unsigned long)probeSeq)+"|"+when+
        // Optional 10th field only on explicit request; default packet unchanged.
        (testPulse?String("|")+ClusterCaptureOptions::kTestPulseToken:String());
    ClusterJobs::transition(id,currentCoordinatorEpoch,ClusterJobs::State::Sent,millis());
    if(!sendSignedPayloadTo(cmd,peer->ip,CLUSTER_COORDINATOR_PORT,"scheduled photo")){
        ClusterJobs::transition(id,currentCoordinatorEpoch,ClusterJobs::State::Failed,millis(),1);
        error="Capture-Auftrag nicht gesendet.";return false;
    }    ++captureSent;return true;
}

bool clusterRequestNodeDrone(const String &nodeId,bool enabled,String &error){
    if(!clusterLocalIsCoordinator()) {error="Nur am Coordinator.";return false;}
    const ClusterPeer *peer=findPeer(nodeId.c_str());
    if(!peer||!peer->used||!usableIp(peer->ip)||!peer->bootNonce||
       (uint32_t)(millis()-peer->lastSeenMs)>60000UL){error="Node nicht erreichbar/authentifiziert.";return false;}
    if (rejectStandbyNodeAction(peer, error)) return false;
    if(++probeSeq==0)++probeSeq;
    const uint64_t id=((uint64_t)bootNonce<<32)|probeSeq;
    if(!ClusterJobs::enqueue(id,currentCoordinatorEpoch,peer->integrationId,
        ClusterJobs::Kind::Drone,millis(),20000UL)){error="Job-Tabelle voll.";return false;}
    char cb[9],nb[9];snprintf(cb,sizeof(cb),"%08lx",(unsigned long)bootNonce);
    snprintf(nb,sizeof(nb),"%08lx",(unsigned long)peer->bootNonce);
    const String msg=String(CLUSTER_JOB_DRONE_MAGIC)+"|"+runtimeClusterTag+"|"+
        deviceIntegrationId()+"|"+cb+"|"+String((unsigned long)currentCoordinatorEpoch)+
        "|"+nodeId+"|"+nb+"|"+String((unsigned long)probeSeq)+"|"+(enabled?"1":"0");
    ClusterJobs::transition(id,currentCoordinatorEpoch,ClusterJobs::State::Sent,millis());
    if(!sendSignedPayloadTo(msg,peer->ip,CLUSTER_COORDINATOR_PORT,"drone mode")){
        ClusterJobs::transition(id,currentCoordinatorEpoch,ClusterJobs::State::Failed,millis(),1);
        error="Drone-Auftrag nicht gesendet.";return false;
    }    return true;
}

bool clusterRequestNodeWipe(const String &nodeId,String &error)
{
    if (!clusterLocalIsCoordinator()) {
        error="Nur der Coordinator darf SD-Wipe starten.";return false;
    }
    const ClusterPeer *peer=findPeer(nodeId.c_str());
    if (!peer || !peer->used || !usableIp(peer->ip) || !peer->bootNonce ||
        (uint32_t)(millis()-peer->lastSeenMs)>60000UL) {
        error="Node nicht authentifiziert erreichbar.";return false;
    }
    if (rejectStandbyNodeAction(peer, error)) return false;
    if (++probeSeq==0) ++probeSeq;
    const uint64_t id=((uint64_t)bootNonce<<32)|probeSeq;
    if (!ClusterJobs::enqueue(id,currentCoordinatorEpoch,peer->integrationId,
                              ClusterJobs::Kind::ClearRecordings,millis(),120000UL)) {
        error="Job-Liste voll.";return false;
    }
    char cb[9],nb[9];
    snprintf(cb,sizeof(cb),"%08lx",(unsigned long)bootNonce);
    snprintf(nb,sizeof(nb),"%08lx",(unsigned long)peer->bootNonce);
    const String message=String(CLUSTER_JOB_WIPE_MAGIC)+"|"+runtimeClusterTag+
        "|"+deviceIntegrationId()+"|"+cb+"|"+
        String((unsigned long)currentCoordinatorEpoch)+"|"+nodeId+"|"+nb+"|"+
        String((unsigned long)probeSeq);
    ClusterJobs::transition(id,currentCoordinatorEpoch,ClusterJobs::State::Sent,millis());
    if (!sendSignedPayloadTo(message,peer->ip,CLUSTER_COORDINATOR_PORT,"node SD wipe")) {
        ClusterJobs::transition(id,currentCoordinatorEpoch,ClusterJobs::State::Failed,millis(),1);
        error="SD-Wipe-Auftrag konnte nicht gesendet werden.";return false;
    }
    return true;
}

bool clusterRuntimeActive()
{
    return runtimeActive;
}

bool clusterRestartRequired()
{
    if ((cfg_cluster_enabled != 0) != runtimeConfiguredEnabled)
        return true;

    if (!runtimeConfiguredEnabled)
        return false;

    if (coordinatorPolicyFromString(cfg_cluster_coordinator_policy) !=
        runtimeCoordinatorPolicy) {
        return true;
    }

    if (clusterEffectiveId() != String(runtimeClusterId) ||
        cfg_cluster_credential_epoch != runtimeCredentialEpoch ||
        cfg_cluster_name != runtimeClusterName) {
        return true;
    }

    char currentTag[17] = {};
    if (!configTag(currentTag))
        return true;

    return strcmp(currentTag, runtimeClusterTag) != 0;
}

const char *clusterLastError()
{
    return lastError.c_str();
}

void clusterSetWifiRemainingSeconds(int32_t remainingSeconds)
{
    if (remainingSeconds < -1)
        remainingSeconds = -1;

    localWifiRemainingSeconds = remainingSeconds;
}

bool clusterAnnounceResource(
    const ClusterResourceAnnouncement &announcement,
    String &error
)
{
    error = "";

    if (!runtimeActive) {
        error = "cluster runtime is not active";
        return false;
    }

    if (!packetFieldValid(announcement.resourceId, 48, false)) {
        error = "invalid cluster resource id";
        return false;
    }

    if (!packetFieldValid(announcement.resourceType, 24, false)) {
        error = "invalid cluster resource type";
        return false;
    }

    if (!packetFieldValid(announcement.locator, 160, false)) {
        error = "invalid cluster resource locator";
        return false;
    }

    if (
        announcement.ttlSeconds < 1 ||
        announcement.ttlSeconds > CLUSTER_RESOURCE_TTL_MAX_SEC
    ) {
        error = "cluster resource ttl out of range";
        return false;
    }

    char bootText[9];
    snprintf(bootText, sizeof(bootText), "%08lx", (unsigned long)bootNonce);

    const uint32_t resourceSequence = ++sequenceNumber;

    String payload;
    payload.reserve(360);
    payload += CLUSTER_RESOURCE_MAGIC;
    payload += '|';
    payload += runtimeClusterTag;
    payload += '|';
    payload += deviceIntegrationId();
    payload += '|';
    payload += bootText;
    payload += '|';
    payload += String(resourceSequence);
    payload += '|';
    payload += announcement.resourceId;
    payload += '|';
    payload += announcement.resourceType;
    payload += '|';
    payload += String((long long)announcement.timestampUs);
    payload += '|';
    payload += String((unsigned long)announcement.sizeBytes);
    payload += '|';
    payload += String((unsigned long)announcement.ttlSeconds);
    payload += '|';
    payload += announcement.locator;

    if (!sendSignedPayload(payload, "cluster resource")) {
        error = lastError.length()
            ? lastError
            : String("cluster resource multicast failed");
        return false;
    }

    ClusterResource *resource = resourceSlotFor(
        deviceIntegrationId().c_str(),
        announcement.resourceId.c_str(),
        millis()
    );
    if (resource) {
        *resource = ClusterResource{};
        resource->used = true;
        strncpy(
            resource->ownerIntegrationId,
            deviceIntegrationId().c_str(),
            sizeof(resource->ownerIntegrationId) - 1
        );
        strncpy(
            resource->resourceId,
            announcement.resourceId.c_str(),
            sizeof(resource->resourceId) - 1
        );
        strncpy(
            resource->resourceType,
            announcement.resourceType.c_str(),
            sizeof(resource->resourceType) - 1
        );
        strncpy(
            resource->locator,
            announcement.locator.c_str(),
            sizeof(resource->locator) - 1
        );

        wifi_mode_t mode = WiFi.getMode();
        if (
            (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) &&
            WiFi.status() == WL_CONNECTED
        ) {
            resource->sourceIp = WiFi.localIP();
        } else if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
            resource->sourceIp = WiFi.softAPIP();
        }

        resource->timestampUs = announcement.timestampUs;
        resource->sizeBytes = announcement.sizeBytes;
        resource->ttlSeconds = announcement.ttlSeconds;
        resource->lastSeenMs = millis();
    }

    return true;
}

String clusterStatusJson()
{
    const uint32_t nowMs = millis();
    // Read-only snapshot: election and ACK ownership belong to clusterLoop().
    const bool localTimeValid = timeIsValid();
    const int64_t localEpoch = localTimeValid ? (int64_t)time(nullptr) : 0;
    const bool localRecording = recorderIsOpen();
    const bool localStreaming =
        streamerRtspClientConnected() ||
        streamerHttpClientConnected();

    String json;
    json.reserve(8200);
    json = "{\"ok\":true";
    json += ",\"configured_enabled\":";
    json += cfg_cluster_enabled ? "true" : "false";
    json += ",\"runtime_active\":";
    json += runtimeActive ? "true" : "false";
    json += ",\"restart_required\":";
    json += clusterRestartRequired() ? "true" : "false";
    json += ",\"cluster_id\":\"" + jsonEscape(clusterEffectiveId()) + "\"";
    json += ",\"runtime_cluster_id\":\"" + jsonEscape(String(runtimeClusterId)) + "\"";
    json += ",\"cluster_name\":\"" + jsonEscape(cfg_cluster_name) + "\"";
    json += ",\"credential_epoch\":" + String((unsigned long)cfg_cluster_credential_epoch);
    json += ",\"runtime_credential_epoch\":" + String((unsigned long)runtimeCredentialEpoch);
    json += ",\"discovery_scanner_active\":";
    json += clusterDiscoveryScannerActive() ? "true" : "false";
    json += ",\"coordinator_policy\":\"" +
        jsonEscape(cfg_cluster_coordinator_policy) + "\"";
    json += ",\"runtime_coordinator_policy\":\"" +
        String(coordinatorPolicyName(runtimeCoordinatorPolicy)) + "\"";
    json += ",\"coordinator_id\":\"" +
        jsonEscape(String(currentCoordinatorId)) + "\"";
    json += ",\"coordinator_epoch\":" +
        String((unsigned long)currentCoordinatorEpoch);
    json += ",\"drone_mode_local\":" + String(droneModeEnabled()?"true":"false");
    json += ",\"local_role\":\"" +
        String(localIsCoordinator ? "coordinator" : "node") + "\"";
    json += ",\"coordinator_heartbeat_sec\":" +
        String((unsigned long)(CLUSTER_COORDINATOR_HEARTBEAT_MS / 1000UL));
    json += ",\"coordinator_lease_sec\":" +
        String((unsigned long)CLUSTER_COORDINATOR_LEASE_SEC);
    json += ",\"coordinator_port\":" +
        String((unsigned long)CLUSTER_COORDINATOR_PORT);
    int64_t clusterUtc = 0;
    const bool clusterClockValid = clusterTimeNowUs(clusterUtc);
    json += ",\"cluster_time\":{";
    json += "\"valid\":";
    json += clusterClockValid ? "true" : "false";
    json += ",\"source\":\"";
    uint32_t sourceAge=0; double sourceRtt=0; int64_t sourceUtc=0;
    const bool espSource=!localIsCoordinator && droneModeEnabled() &&
        ClusterEspNowRadio::precisionUtcNow(currentCoordinatorEpoch,currentCoordinatorId,
                                              sourceUtc,sourceAge,sourceRtt);
    json += localIsCoordinator ? "local_coordinator" : (espSource ? "espnow_precision" : "wifi_coordinator");
    json += "\"";
    json += ",\"utc_us\":" + int64Text(clusterClockValid ? clusterUtc : 0);
    // Offset is relative to the device's own wall clock, not independent
    // proof of absolute correctness. The uncertainty is heuristic, NOT a bound.
    const int64_t localWallUs = localUnixTimeUs();
    json += ",\"local_offset_available\":";
    json += (clusterClockValid && localWallUs != 0) ? "true" : "false";
    json += ",\"local_offset_us\":" + int64Text(
        (clusterClockValid && localWallUs != 0) ? (clusterUtc - localWallUs) : 0);
    const uint32_t sampleAgeMs = timeEstimateValid ? (uint32_t)(nowMs - timeLastSampleMs) : 0;
    // The historical RTT/2 diagnostic refers only to Wi-Fi synchronization.
    // Do not advertise it as the ESP-NOW clock's uncertainty.
    const int64_t estimatedUncertaintyUs = espSource ? 0 : (timeEstimateValid ?
        (timeLastRttUs / 2 + (int64_t)sampleAgeMs * 50LL / 1000LL) : 0);
    json += ",\"uncertainty_estimate_us\":" + int64Text(estimatedUncertaintyUs);
    json += ",\"sync_interval_sec\":900";
    json += ",\"manual_sync_sequence\":" + String((unsigned long)timeSyncCommandSeq);
    json += ",\"manual_sync_sent\":" + String((unsigned long)timeSyncCommandsSent);
    json += ",\"manual_sync_received\":" + String((unsigned long)timeSyncCommandsReceived);
    json += ",\"last_rtt_us\":" + int64Text(timeLastRttUs);
    json += ",\"sample_age_ms\":" + String(timeEstimateValid ? (unsigned long)(nowMs-timeLastSampleMs) : 0UL);
    json += ",\"accepted\":" + String((unsigned long)timeAcceptedSamples);
    json += ",\"rejected\":" + String((unsigned long)timeRejectedSamples);
    json += "}";
    // Local radio diagnostics only. Node-specific ESP-NOW estimates are NOT
    // present in coordinator discovery reports and must not be fabricated.
    const ClusterEspNowRadio::Diagnostics esn = ClusterEspNowRadio::diagnostics();
    json += ",\"espnow_time_local\":{";
    json += "\"active\":"; json += esn.active ? "true" : "false";
    json += ",\"channel\":" + String((unsigned int)esn.channel);
    json += ",\"sent\":" + String((unsigned long)esn.sent);
    json += ",\"received\":" + String((unsigned long)esn.received);
    json += ",\"authenticated\":" + String((unsigned long)esn.authenticated);
    json += ",\"rejected\":" + String((unsigned long)esn.rejected);
    json += ",\"replay\":" + String((unsigned long)esn.replay);
    json += ",\"dropped\":" + String((unsigned long)esn.dropped);
    json += ",\"probes_sent\":" + String((unsigned long)esn.probesSent);
    json += ",\"probes_answered\":" + String((unsigned long)esn.probesAnswered);
    json += ",\"exchanges_complete\":" + String((unsigned long)esn.exchangesComplete);
    json += ",\"exchange_timeouts\":" + String((unsigned long)esn.exchangeTimeouts);
    json += ",\"rx_queue_delay_last_us\":" + String((unsigned long)esn.rxQueueDelayLastUs);
    json += ",\"rx_queue_delay_max_us\":" + String((unsigned long)esn.rxQueueDelayMaxUs);
    json += ",\"reply_final_dispatch_gap_last_us\":" + String((unsigned long)esn.replyFinalDispatchGapLastUs);
    json += ",\"reply_final_dispatch_gap_max_us\":" + String((unsigned long)esn.replyFinalDispatchGapMaxUs);
    json += ",\"probe_tx_pre_send_last_us\":" + String((unsigned long)esn.probeTxPreSendLastUs);
    json += ",\"probe_tx_pre_send_max_us\":" + String((unsigned long)esn.probeTxPreSendMaxUs);
    json += ",\"probe_tx_pre_send_samples\":" + String((unsigned long)esn.probeTxPreSendSamples);
    json += ",\"probe_tx_pre_send_mean_us\":" + String(esn.probeTxPreSendSamples ? double(esn.probeTxPreSendTotalUs)/esn.probeTxPreSendSamples : 0.0, 2);
    json += ",\"reply_tx_pre_send_last_us\":" + String((unsigned long)esn.replyTxPreSendLastUs);
    json += ",\"reply_tx_pre_send_max_us\":" + String((unsigned long)esn.replyTxPreSendMaxUs);
    json += ",\"reply_tx_pre_send_samples\":" + String((unsigned long)esn.replyTxPreSendSamples);
    json += ",\"reply_tx_pre_send_mean_us\":" + String(esn.replyTxPreSendSamples ? double(esn.replyTxPreSendTotalUs)/esn.replyTxPreSendSamples : 0.0, 2);
    json += ",\"tx_callback_delay_last_us\":" + String((unsigned long)esn.txCallbackLatencyLastUs);
    json += ",\"tx_callback_delay_max_us\":" + String((unsigned long)esn.txCallbackLatencyMaxUs);
    json += ",\"tx_callback_samples\":" + String((unsigned long)esn.txCallbackSamples);
    json += ",\"tx_callback_failed\":" + String((unsigned long)esn.txCallbackFailed);
    json += ",\"tx_pending_overflow\":" + String((unsigned long)esn.txPendingOverflow);
    json += ",\"tx_callback_unmatched\":" + String((unsigned long)esn.txCallbackUnmatched);
    json += ",\"last_exchange_valid\":"; json += esn.lastExchangeValid ? "true" : "false";
    json += ",\"last_offset_us\":" + String(esn.lastOffsetUs, 3);
    json += ",\"last_rtt_us\":" + String(esn.lastRttUs, 3);
    json += ",\"last_series_estimate_available\":"; json += esn.lastSeriesEstimateAvailable ? "true" : "false";
    json += ",\"last_series_estimate_age_ms\":" + String((unsigned long)esn.lastSeriesEstimateAgeMs);
    json += ",\"last_series_selected\":" + String((unsigned int)esn.lastSeriesSelected);
    json += ",\"last_series_spread_us\":" + String(esn.lastSeriesSpreadUs, 3);
    json += ",\"last_series_rtt_min_us\":" + String(esn.lastSeriesRttMinUs, 3);
    json += ",\"last_series_rtt_max_us\":" + String(esn.lastSeriesRttMaxUs, 3);
    json += ",\"last_series_offset_p10_relative_us\":" + String(esn.lastSeriesOffsetP10Us, 3);
    json += ",\"last_series_offset_p90_relative_us\":" + String(esn.lastSeriesOffsetP90Us, 3);
    json += ",\"drift_valid\":"; json += esn.driftValid ? "true" : "false";
    json += ",\"stored_drift_available\":"; json += esn.storedDriftAvailable ? "true" : "false";
    json += ",\"drift_ppm\":" + String(esn.driftPpm, 4);
    json += ",\"drift_uncertainty_ppm\":" + String(esn.driftUncertaintyPpm, 4);
    json += ",\"drift_samples\":" + String((unsigned long)esn.driftSamples);
    json += ",\"drift_persisted\":" + String((unsigned long)esn.driftPersisted);
    json += ",\"last_accepted_age_ms\":" + String((unsigned long)esn.lastAcceptedAgeMs);
    json += ",\"precision_recently_accepted\":"; json += esn.precisionRecentlyAccepted ? "true" : "false";
    json += ",\"action_pause_remaining_ms\":" + String((unsigned long)esn.capturePauseRemainingMs);
    json += ",\"precision_clock_active\":";json += espSource ? "true" : "false";
    json += ",\"precision_sample_age_ms\":" + String(espSource ? (unsigned long)sourceAge : 0UL);
    json += ",\"precision_rtt_us\":" + String(espSource ? sourceRtt : 0.0, 3);
    // Beta131: Broadcaster handover diagnostics (minimal addition for verification)
    json += ",\"broadcaster_lease_active\":"; json += esn.broadcasterLeaseActive ? "true" : "false";
    json += ",\"broadcaster_lease_remaining_ms\":" + String((unsigned long)esn.broadcasterLeaseRemainingMs);
    json += ",\"broadcaster_production_beacons_sent\":" + String((unsigned long)esn.broadcasterProductionBeaconsSent);
    json += ",\"coordinator_suppressing_beacons\":"; json += esn.coordinatorSuppressingBeacons ? "true" : "false";
    json += ",\"coordinator_beacons_suppressed\":" + String((unsigned long)esn.coordinatorBeaconsSuppressed);
    // Beta136: coordinator_suppressing_beacons = actual suppression; lease state separately.
    json += ",\"coordinator_broadcaster_lease_active\":"; json += esn.coordinatorBroadcasterLeaseActive ? "true" : "false";
    json += ",\"coordinator_broadcaster_beacon_age_ms\":" + String((unsigned long)esn.coordinatorBroadcasterBeaconAgeMs);
    json += ",\"coordinator_beacons_sent\":" + String((unsigned long)esn.coordinatorBeaconsSent);
    json += ",\"coordinator_early_resumes\":" + String((unsigned long)esn.coordinatorEarlyResumes);
    // Beta136: series quality since radio start (reason: 0 none, 1 invalid, 2 spread, 3 rtt, 4 deadline)
    json += ",\"series_accepted\":" + String((unsigned long)esn.seriesAccepted);
    json += ",\"series_rejected\":" + String((unsigned long)esn.seriesRejected);
    json += ",\"series_reject_invalid\":" + String((unsigned long)esn.seriesRejectInvalid);
    json += ",\"series_reject_spread\":" + String((unsigned long)esn.seriesRejectSpread);
    json += ",\"series_reject_rtt\":" + String((unsigned long)esn.seriesRejectRtt);
    json += ",\"series_reject_deadline\":" + String((unsigned long)esn.seriesRejectDeadline);
    json += ",\"series_consecutive_failures\":" + String((unsigned int)esn.seriesConsecutiveFailures);
    json += ",\"series_last_failure_reason\":" + String((unsigned int)esn.seriesLastFailureReason);
    json += ",\"series_last_rejected_age_ms\":" + String((unsigned long)esn.seriesLastRejectedAgeMs);
    json += ",\"series_last_rejected_spread_us\":" + String((unsigned long)esn.seriesLastRejectedSpreadUs);
    json += ",\"series_last_completed_age_ms\":" + String((unsigned long)esn.seriesLastCompletedAgeMs);
    json += ",\"series_next_in_ms\":" + String((unsigned long)esn.seriesNextInMs);
    // Beta136: probe RX (t2) -> reply pre-send (t3) on this device, answered probes only.
    json += ",\"probe_reply_delay_samples\":" + String((unsigned long)esn.probeReplyDelaySamples);
    json += ",\"probe_reply_delay_last_us\":" + String((unsigned long)esn.probeReplyDelayLastUs);
    json += ",\"probe_reply_delay_max_us\":" + String((unsigned long)esn.probeReplyDelayMaxUs);
    json += ",\"probe_reply_delay_mean_us\":" + String(esn.probeReplyDelaySamples ?
        (double)esn.probeReplyDelayTotalUs / (double)esn.probeReplyDelaySamples : 0.0, 1);
    json += ",\"probe_reply_delay_hist\":[";
    for (uint8_t i = 0; i < ClusterEspNowRadio::Diagnostics::kProbeReplyDelayBuckets; ++i) {
        if (i) json += ",";
        json += String((unsigned long)esn.probeReplyDelayHist[i]);
    }
    json += "],\"tx_reply_latency_samples\":" + String((unsigned long)esn.txReplyLatencySamples);
    json += ",\"tx_reply_latency_last_us\":" + String((unsigned long)esn.txReplyLatencyLastUs);
    json += ",\"tx_reply_latency_max_us\":" + String((unsigned long)esn.txReplyLatencyMaxUs);
    json += ",\"tx_reply_latency_hist\":[";
    for (uint8_t i = 0; i < ClusterEspNowRadio::Diagnostics::kProbeReplyDelayBuckets; ++i) {
        if (i) json += ",";
        json += String((unsigned long)esn.txReplyLatencyHist[i]);
    }
    json += "],\"probe_reply_delay_hist_upper_us\":[";
    for (uint8_t i = 0; i + 1 < ClusterEspNowRadio::Diagnostics::kProbeReplyDelayBuckets; ++i) {
        if (i) json += ",";
        json += String((unsigned long)ClusterEspNowRadio::kProbeReplyDelayBucketUpperUs[i]);
    }
    json += "]";
    char activeSourceText[17]; snprintf(activeSourceText,sizeof(activeSourceText),"%016llx",(unsigned long long)esn.activeTimeSource);
    json += ",\"active_time_source\":\"" + String(activeSourceText) + "\"";
    json += ",\"source_changes\":" + String((unsigned long)esn.sourceChanges);
    // Beta137: role-based WiFi power save (actual value read via esp_wifi_get_ps()).
    {
        const WifiPowerPolicy::Status wps = WifiPowerPolicy::status();
        json += ",\"wifi_ps_mode\":\""; json += WifiPowerPolicy::modeName(wps.actual); json += "\"";
        json += ",\"wifi_ps_expected\":\""; json += WifiPowerPolicy::modeName(wps.expected); json += "\"";
        json += ",\"wifi_ps_cluster_active\":"; json += wps.clusterActive ? "true" : "false";
        json += ",\"wifi_ps_applies\":" + String((unsigned long)wps.applies);
        json += ",\"wifi_ps_apply_failures\":" + String((unsigned long)wps.applyFailures);
        json += ",\"wifi_ps_read_failures\":" + String((unsigned long)wps.readFailures);
        json += ",\"wifi_ps_last_error\":" + String((long)wps.lastError);
    }
    // Beta131-DIAGNOSIS (Seventeenth cycle): Production beacon blocking diagnostics
    json += ",\"broadcaster_time_base_invalid\":" + String((unsigned long)esn.broadcasterTimeBaseInvalid);
    json += ",\"broadcaster_precision_failed\":" + String((unsigned long)esn.broadcasterPrecisionFailed);
    json += ",\"precision_guard_was_coordinator\":" + String((unsigned long)esn.precisionGuardWasCoordinator);
    json += ",\"precision_guard_epoch_mismatch\":" + String((unsigned long)esn.precisionGuardEpochMismatch);
    json += ",\"precision_guard_tag_mismatch\":" + String((unsigned long)esn.precisionGuardTagMismatch);
    json += ",\"precision_guard_not_valid\":" + String((unsigned long)esn.precisionGuardNotValid);
    json += ",\"precision_guard_other\":" + String((unsigned long)esn.precisionGuardOther);
    // Beta131-DIAGNOSIS (Nineteenth cycle): Separate "other" guards for precise diagnosis
    json += ",\"precision_guard_ref_lifetime\":" + String((unsigned long)esn.precisionGuardRefLifetime);
    json += ",\"precision_guard_estimate_age\":" + String((unsigned long)esn.precisionGuardEstimateAge);
    json += ",\"precision_guard_drift\":" + String((unsigned long)esn.precisionGuardDrift);
    json += ",\"precision_guard_mono_reconstruct\":" + String((unsigned long)esn.precisionGuardMonoReconstruct);
    json += ",\"precision_guard_utc_range\":" + String((unsigned long)esn.precisionGuardUtcRange);
    json += '}';
    // ESP-NOW result frames are verified by the radio HMAC and replay gate.
    // Bind to the node boot nonce at presentation time; do not infer identity from MAC.
    // Beta128: fixed-size, observation-only Grant/Ack counters. No new buffers,
    // authority, capture-clock changes or productive beacon control.
    const ClusterEspNowRadio::GrantDiagnosticStats grantStats =
        ClusterEspNowRadio::grantDiagnosticStats();
    json += ",\"espnow_grant_diagnostic\":{";
    // Beta131-FIX: Show actual handover capability status
    const bool handoverCapable = (cfg_operating_mode == "time_broadcaster") || localIsCoordinator;
    json += "\"handover_enabled\":";
    json += handoverCapable ? "true" : "false";
    json += ",\"received\":" + String((unsigned long)grantStats.received);
    json += ",\"authenticated\":" + String((unsigned long)grantStats.authenticated);
    json += ",\"rejected\":" + String((unsigned long)grantStats.rejected);
    json += ",\"replay\":" + String((unsigned long)grantStats.replay);
    json += ",\"dropped\":" + String((unsigned long)grantStats.dropped);
    json += ",\"grants_sent\":" + String((unsigned long)grantStats.grantsSent);
    json += ",\"acks_sent\":" + String((unsigned long)grantStats.acksSent);
    json += ",\"acks_verified\":" + String((unsigned long)grantStats.acksVerified);
    json += ",\"timeouts\":" + String((unsigned long)grantStats.timeouts);
    json += ",\"send_failed\":" + String((unsigned long)grantStats.sendFailed);
    json += '}';
    json += ",\"espnow_node_reports\":[";
    ClusterEspNowRadio::NodeSyncReport reports[12];
    const size_t reportCount=ClusterEspNowRadio::nodeSyncReports(reports,12);
    for(size_t i=0;i<reportCount;++i){
        if(i)json+=',';
        const auto &r=reports[i];
        char bootText[9];snprintf(bootText,sizeof(bootText),"%08lx",(unsigned long)r.bootNonce);
        json += "{\"boot_id\":\""+String(bootText)+"\"";
        json += ",\"received_age_ms\":"+String((unsigned long)r.receivedAgeMs);
        json += ",\"sample_age_ms\":"+String((unsigned long)r.sampleAgeMs);
        json += ",\"offset_us\":"+String((long)r.offsetUs);
        json += ",\"rtt_us\":"+String((unsigned long)r.rttUs);
        json += ",\"accepted_samples\":"+String((unsigned int)r.acceptedSamples);
        json += ",\"precision_accepted\":";json+=r.precisionAccepted?"true":"false";
        json += ",\"probes_sent\":"+String((unsigned long)r.probesSent);
        json += ",\"exchanges_complete\":"+String((unsigned long)r.exchangesComplete);
        json += ",\"failure_code\":"+String((unsigned int)r.failureCode);
        json += ",\"failed_series\":"+String((unsigned int)r.failedSeries);
        json += ",\"has_series_statistics\":";json+=r.hasSeriesStatistics?"true":"false";
        json += ",\"series_successful\":"+String((unsigned long)r.successfulSeries);
        json += ",\"series_rejected\":"+String((unsigned long)r.rejectedSeries);
        json += ",\"last_series_result\":"+String((unsigned int)r.lastSeriesResult);
        json += ",\"last_series_age_ms\":"+String((unsigned long)r.lastSeriesAgeMs);
        json += ",\"has_last_rejected_series\":";json+=r.hasLastRejectedSeries?"true":"false";
        json += ",\"last_rejected_reason\":"+String((unsigned int)r.lastRejectedReason);
        json += ",\"last_rejected_age_ms\":"+String((unsigned long)r.lastRejectedAgeMs);
        json += ",\"last_rejected_metrics_valid\":";json+=r.lastRejectedMetricsValid?"true":"false";
        json += ",\"last_rejected_spread_us\":"+String((unsigned long)r.lastRejectedSpreadUs);
        json += ",\"last_rejected_rtt_us\":"+String((unsigned long)r.lastRejectedRttUs);
        json += ",\"has_failure_metrics\":";json+=r.hasFailureMetrics?"true":"false";
        json += ",\"failed_spread_us\":"+String((unsigned long)r.failedSpreadUs);
        json += ",\"failed_rtt_us\":"+String((unsigned long)r.failedRttUs);
        json += ",\"has_failure_offsets\":";json+=r.hasFailureOffsets?"true":"false";
        json += ",\"failed_min_rtt_offset_us\":"+String((long)r.failedMinRttOffsetUs);
        json += ",\"failed_selected_offset_us\":"+String((long)r.failedSelectedOffsetUs);
        json += ",\"failed_accepted\":"+String((unsigned int)r.failedAccepted);
        json += ",\"failed_selected\":"+String((unsigned int)r.failedSelected);
        json += ",\"has_time_source\":";json+=r.hasTimeSource?"true":"false";
        json += ",\"using_espnow_time\":";json+=r.usingEspNowTime?"true":"false";
        json += ",\"has_drift_report\":";json+=r.hasDriftReport?"true":"false";
        json += ",\"drift_valid\":";json+=r.driftValid?"true":"false";
        json += ",\"drift_ppm\":"+String(r.driftMilliPpm/1000.0,3);
        json += ",\"spread_us\":"+String((unsigned int)r.spreadUs);
        json += '}';
    }
    json += ']';
    json += ",\"ack_diagnostics\":{";
    json += "\"hb_sent\":" + String((unsigned long)diagHbSent);
    json += ",\"hb_send_failed\":" + String((unsigned long)diagHbSendFailed);
    json += ",\"ack_send_attempted\":" + String((unsigned long)diagAckSendAttempted);
    json += ",\"ack_send_failed\":" + String((unsigned long)diagAckSendFailed);
    json += ",\"ack_authenticated\":" + String((unsigned long)diagAckAuthenticated);
    json += ",\"ack_accepted\":" + String((unsigned long)diagAckAccepted);
    json += ",\"ack_rejected\":" + String((unsigned long)diagAckRejected);
    json += ",\"coordinator_changes\":" + String((unsigned long)diagCoordinatorChanges);
    json += ",\"coordinator_epoch_changes\":" + String((unsigned long)diagCoordinatorEpochChanges);
    json += ",\"ack_context_resets\":" + String((unsigned long)diagAckContextResets);
    json += ",\"incomplete_coord_announcements\":" + String((unsigned long)diagIncompleteCoordinatorAnnouncements);
    json += ",\"last_reject\":\"" + String(diagAckLastReject) + "\"}";
    json += ",\"election_trace\":[";
    for (uint8_t i = 0; i < diagElectionTraceUsed; ++i) {
        if (i) json += ',';
        const uint8_t slot = (diagElectionTraceNext + CLUSTER_ELECTION_TRACE_COUNT -
                              diagElectionTraceUsed + i) % CLUSTER_ELECTION_TRACE_COUNT;
        json += "\"" + jsonEscape(String(diagElectionTrace[slot])) + "\"";
    }
    json += ']';
    json += ",\"coordinator_ack_age_ms\":";
    if (!localIsCoordinator && lastCoordinatorAckMs != 0)
        json += String((unsigned long)(nowMs - lastCoordinatorAckMs));
    else
        json += "-1";
    json += ",\"heartbeat_sec\":" +
        String((unsigned long)(CLUSTER_HEARTBEAT_MS / 1000UL));
    json += ",\"default_lease_sec\":" +
        String((unsigned long)CLUSTER_DEFAULT_LEASE_SEC);
    json += ",\"password_configured\":";
    json += cfg_cluster_password.length() ? "true" : "false";
    json += ",\"error\":\"" + jsonEscape(lastError) + "\"";
    json += ",\"nodes\":[";

    json += "{\"local\":true";
    json += ",\"online\":";
    json += runtimeActive ? "true" : "false";
    json += ",\"integration_id\":\"" + jsonEscape(deviceIntegrationId()) + "\"";
    json += ",\"hostname\":\"" + jsonEscape(cfg_hostname) + "\"";
    json += ",\"ip\":\"";
    wifi_mode_t mode = WiFi.getMode();
    if (
        (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) &&
        WiFi.status() == WL_CONNECTED
    ) {
        json += WiFi.localIP().toString();
    } else if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        json += WiFi.softAPIP().toString();
    } else {
        json += "0.0.0.0";
    }
    json += "\"";
    json += ",\"mode\":\"" + jsonEscape(cfg_operating_mode) + "\"";
    json += ",\"drone_mode\":" + String(droneModeEnabled()?"true":"false");
    json += ",\"release\":\"" + jsonEscape(String(SENSORFORGE_RELEASE_TAG)) + "\"";
    json += ",\"uptime_seconds\":" + String((unsigned long)(nowMs / 1000UL));
    char localBootText[9];
    snprintf(localBootText, sizeof(localBootText), "%08lx", (unsigned long)bootNonce);
    json += ",\"boot_id\":\"" + String(localBootText) + "\"";
    json += ",\"time_valid\":";
    json += localTimeValid ? "true" : "false";
    json += ",\"epoch\":" + String((long long)localEpoch);
    json += ",\"health_valid\":true";
    json += ",\"sd_total_mib\":" + String((unsigned long)healthLocalTotalMiB);
    json += ",\"sd_used_mib\":" + String((unsigned long)healthLocalUsedMiB);
    json += ",\"cpu_temp_deci_c\":" + String((long)(isfinite(thermalCpuTemperatureC()) ? roundf(thermalCpuTemperatureC()*10.0f) : -1000));
    json += ",\"free_heap_bytes\":" + String((unsigned long)ESP.getFreeHeap());
    json += ",\"recording\":";
    json += localRecording ? "true" : "false";
    json += ",\"streaming\":";
    json += localStreaming ? "true" : "false";
    json += ",\"transport\":";
    json += cfg_transport_mode ? "true" : "false";
    json += ",\"coordinator_policy\":\"" +
        String(coordinatorPolicyName(runtimeCoordinatorPolicy)) + "\"";
    json += ",\"cluster_role\":\"" +
        String(localIsCoordinator ? "coordinator" : "node") + "\"";
    json += ",\"precision_sync\":\"" + String(precisionSyncName(CLUSTER_LOCAL_CAPABILITIES)) + "\"";
    json += ",\"direct_heartbeat_age_ms\":-1";
    json += ",\"coordinator_ack_age_ms\":";
    if (!localIsCoordinator && lastCoordinatorAckMs != 0)
        json += String((unsigned long)(nowMs - lastCoordinatorAckMs));
    else
        json += "-1";
    json += ",\"lease_sec\":" +
        String((unsigned long)CLUSTER_DEFAULT_LEASE_SEC);
    json += ",\"lease_remaining_sec\":" +
        String((unsigned long)CLUSTER_DEFAULT_LEASE_SEC);
    json += ",\"wifi_remaining_sec\":" +
        String((long)localWifiRemainingSeconds);
    json += ",\"age_ms\":0}";

    for (size_t i = 0; i < CLUSTER_MAX_PEERS; ++i) {
        const ClusterPeer &peer = peers[i];
        if (!peer.used)
            continue;

        const uint32_t ageMs = nowMs - peer.lastSeenMs;
        const uint32_t leaseSeconds = peerLeaseSeconds(peer);
        const uint64_t leaseMs = (uint64_t)leaseSeconds * 1000ULL;
        if ((uint64_t)ageMs > leaseMs)
            continue;

        const uint32_t leaseRemainingSeconds =
            ageMs >= leaseMs
            ? 0
            : (uint32_t)((leaseMs - ageMs + 999ULL) / 1000ULL);

        json += ",{";
        json += "\"local\":false";
        json += ",\"online\":true";
        json += ",\"integration_id\":\"" + jsonEscape(String(peer.integrationId)) + "\"";
        json += ",\"hostname\":\"" + jsonEscape(String(peer.hostname)) + "\"";
        json += ",\"ip\":\"" + peer.ip.toString() + "\"";
        json += ",\"mode\":\"" + jsonEscape(String(peer.mode)) + "\"";
        json += ",\"release\":\"" + jsonEscape(String(peer.release)) + "\"";
        json += ",\"uptime_seconds\":" + String((unsigned long)peer.uptimeSeconds);
        char peerBootText[9];
        snprintf(peerBootText, sizeof(peerBootText), "%08lx", (unsigned long)peer.bootNonce);
        json += ",\"boot_id\":\"" + String(peerBootText) + "\"";
        json += ",\"time_valid\":";
        json += peer.timeValid ? "true" : "false";
        json += ",\"epoch\":" + String((long long)peer.epochSeconds);
        const bool freshHealth=peer.healthValid && (uint32_t)(nowMs-peer.healthUpdatedMs)<90000UL;
        json += ",\"health_valid\":";
        json += freshHealth ? "true" : "false";
        json += ",\"sd_total_mib\":" + String((unsigned long)(freshHealth?peer.sdTotalMiB:0));
        json += ",\"sd_used_mib\":" + String((unsigned long)(freshHealth?peer.sdUsedMiB:0));
        json += ",\"cpu_temp_deci_c\":" + String((long)(freshHealth?peer.cpuTempDeciC:-1000));
        json += ",\"free_heap_bytes\":" + String((unsigned long)(freshHealth?peer.freeHeapBytes:0));
        json += ",\"recording\":";
        json += peer.recording ? "true" : "false";
        json += ",\"streaming\":";
        json += peer.streaming ? "true" : "false";
        json += ",\"transport\":";
        json += peer.transportMode ? "true" : "false";
        json += ",\"coordinator_policy\":\"" +
            String(coordinatorPolicyName(peer.coordinatorPolicy)) + "\"";
        json += ",\"precision_sync\":\"" + String(precisionSyncName(peer.coordinationCapabilities)) + "\"";
        json += ",\"cluster_role\":\"" +
            String(
                currentCoordinatorId[0] &&
                strcmp(peer.integrationId, currentCoordinatorId) == 0
                ? "coordinator"
                : "node"
            ) + "\"";
        json += ",\"direct_heartbeat_age_ms\":";
        if (peer.lastDirectHeartbeatMs != 0)
            json += String((unsigned long)(nowMs - peer.lastDirectHeartbeatMs));
        else
            json += "-1";
        json += ",\"coordinator_ack_age_ms\":-1";
        json += ",\"selected_sync_seq\":" + String((unsigned long)peer.selectedSyncSeq);
        json += ",\"selected_sync_age_ms\":" + String((unsigned long)(peer.selectedSyncSeq ? nowMs-peer.selectedSyncRequestedMs : 0));
        json += ",\"time_report_valid\":";
        const bool reportValid = peer.timeReportValid &&
            peer.timeReportEpoch == currentCoordinatorEpoch &&
            (uint32_t)(nowMs - peer.timeReportMs) <= CLUSTER_TIME_HOLDOVER_MS;
        json += reportValid ? "true" : "false";
        json += ",\"time_report_command_seq\":" +
            String((unsigned long)(reportValid ? peer.timeReportCommandSeq : 0UL));
        json += ",\"time_report_age_ms\":" +
            String((unsigned long)(reportValid ? (nowMs - peer.timeReportMs) : 0UL));
        json += ",\"time_report_utc_us\":" + int64Text(reportValid ? peer.timeReportUtcUs : 0);
        json += ",\"time_report_offset_us\":" + int64Text(reportValid ? peer.timeReportOffsetUs : 0);
        json += ",\"time_report_rtt_us\":" + int64Text(reportValid ? peer.timeReportRttUs : 0);
        json += ",\"time_report_uncertainty_us\":" + int64Text(reportValid ? peer.timeReportUncertaintyUs : 0);
        json += ",\"lease_sec\":" + String((unsigned long)leaseSeconds);
        json += ",\"lease_remaining_sec\":" +
            String((unsigned long)leaseRemainingSeconds);
        json += ",\"wifi_remaining_sec\":" +
            String((long)peer.wifiRemainingSeconds);
        json += ",\"age_ms\":" + String((unsigned long)ageMs);
        json += '}';
    }

    json += "],\"resources\":[";

    bool firstResource = true;
    for (size_t i = 0; i < CLUSTER_MAX_RESOURCES; ++i) {
        const ClusterResource &resource = resources[i];
        if (!resource.used)
            continue;

        const uint32_t ageMs = nowMs - resource.lastSeenMs;
        const uint64_t ttlMs =
            (uint64_t)clampSeconds(
                resource.ttlSeconds,
                1U,
                CLUSTER_RESOURCE_TTL_MAX_SEC
            ) * 1000ULL;

        if ((uint64_t)ageMs > ttlMs)
            continue;

        if (!firstResource)
            json += ',';
        firstResource = false;

        const uint32_t ttlRemainingSeconds =
            ageMs >= ttlMs
            ? 0
            : (uint32_t)((ttlMs - ageMs + 999ULL) / 1000ULL);

        json += '{';
        json += "\"owner_integration_id\":\"" +
            jsonEscape(String(resource.ownerIntegrationId)) + "\"";
        json += ",\"resource_id\":\"" +
            jsonEscape(String(resource.resourceId)) + "\"";
        json += ",\"type\":\"" +
            jsonEscape(String(resource.resourceType)) + "\"";
        json += ",\"source_ip\":\"" + resource.sourceIp.toString() + "\"";
        json += ",\"timestamp_us\":" +
            String((long long)resource.timestampUs);
        json += ",\"size_bytes\":" +
            String((unsigned long)resource.sizeBytes);
        json += ",\"ttl_sec\":" +
            String((unsigned long)resource.ttlSeconds);
        json += ",\"ttl_remaining_sec\":" +
            String((unsigned long)ttlRemainingSeconds);
        json += ",\"locator\":\"" +
            jsonEscape(String(resource.locator)) + "\"";
        json += '}';
    }

    json += "],\"discovered_nodes\":[";

    bool firstDiscovery = true;
    for (size_t i = 0; i < CLUSTER_MAX_DISCOVERY_NODES; ++i) {
        const ClusterDiscoveryNode &node = discoveryNodes[i];
        if (!node.used)
            continue;
        const uint32_t ageMs = nowMs - node.lastSeenMs;
        if (ageMs > CLUSTER_DISCOVERY_NODE_TTL_MS)
            continue;

        if (!firstDiscovery)
            json += ',';
        firstDiscovery = false;

        json += '{';
        json += "\"cluster_id\":\"" + jsonEscape(String(node.clusterId)) + "\"";
        json += ",\"cluster_name\":\"" + jsonEscape(String(node.clusterName)) + "\"";
        json += ",\"credential_epoch\":" + String((unsigned long)node.credentialEpoch);
        json += ",\"integration_id\":\"" + jsonEscape(String(node.integrationId)) + "\"";
        json += ",\"coordinator_policy\":\"" + String(coordinatorPolicyName(node.coordinatorPolicy)) + "\"";
        json += ",\"coordinator\":" + String(node.coordinator ? "true" : "false");
        json += ",\"release\":\"" + jsonEscape(String(node.release)) + "\"";
        json += ",\"ip\":\"" + node.sourceIp.toString() + "\"";
        const ClusterPeer *authenticatedPeer = findPeer(node.integrationId);
        bool authenticated = false;
        if (authenticatedPeer) {
            const uint32_t authAge = nowMs - authenticatedPeer->lastSeenMs;
            authenticated =
                (uint64_t)authAge <=
                (uint64_t)peerLeaseSeconds(*authenticatedPeer) * 1000ULL;
        }
        json += ",\"authenticated\":" + String(authenticated ? "true" : "false");
        json += ",\"age_ms\":" + String((unsigned long)ageMs);
        json += '}';
    }

    json += "],\"job_probe_diagnostics\":{";
    json += "\"jobs_started\":" + String((unsigned long)probeJobsStarted);
    json += ",\"received\":" + String((unsigned long)probeReceived);
    json += ",\"ack_sent\":" + String((unsigned long)probeAckSent);
    json += ",\"rejected\":" + String((unsigned long)probeRejected);
    json += ",\"last_reject\":\"" + String(probeLastReject) + "\"";
    json += ",\"retransmissions\":" + String((unsigned long)probeRetransmissions);
    json += ",\"ack_received\":" + String((unsigned long)probeAckReceived);
    json += ",\"ack_rejected\":" + String((unsigned long)probeAckRejected);
    json += ",\"last_ack_reject\":\"" + String(probeAckLastReject) + "\"";
    json += "},\"job_restart_diagnostics\":{";
    json += "\"sent\":" + String((unsigned long)restartSent);
    json += ",\"received\":" + String((unsigned long)restartReceived);
    json += ",\"accepted\":" + String((unsigned long)restartAccepted);
    json += ",\"rejected\":" + String((unsigned long)restartRejected);
    json += ",\"last_reject\":\"" + String(restartLastReject) + "\"";
    json += ",\"ack_received\":" + String((unsigned long)restartAckReceived);
    json += ",\"ack_accepted\":" + String((unsigned long)restartAckAccepted);
    json += ",\"ack_rejected\":" + String((unsigned long)restartAckRejected);
    json += ",\"last_ack_reject\":\"" + String(restartAckLastReject) + "\"";
    json += "},\"job_shutdown_diagnostics\":{";
    json += "\"sent\":" + String((unsigned long)shutdownSent);
    json += ",\"received\":" + String((unsigned long)shutdownReceived);
    json += ",\"accepted\":" + String((unsigned long)shutdownAccepted);
    json += ",\"rejected\":" + String((unsigned long)shutdownRejected);
    json += ",\"last_reject\":\"" + String(shutdownLastReject) + "\"";
    json += ",\"ack_received\":" + String((unsigned long)shutdownAckReceived);
    json += ",\"ack_accepted\":" + String((unsigned long)shutdownAckAccepted);
    json += ",\"ack_rejected\":" + String((unsigned long)shutdownAckRejected);
    json += ",\"last_ack_reject\":\"" + String(shutdownAckLastReject) + "\"";
    json += "},\"capture_diagnostics\":{";
    json += "\"sent\":"+String((unsigned long)captureSent);
    json += ",\"accepted\":"+String((unsigned long)captureAccepted);
    json += ",\"completed\":"+String((unsigned long)captureCompleted);
    json += ",\"failed\":"+String((unsigned long)captureFailed);
    json += ",\"local_pending\":"+String(pendingCapture.active?"true":"false");
    json += ",\"local_test_pulse\":\""+String(captureTestPulseState)+"\"";
    json += ",\"result_retry_pending\":"+String(captureResultRetry.active?"true":"false");
    json += ",\"result_retry_attempts\":"+String((unsigned)captureResultRetry.attempts);
    json += ",\"result_retransmissions\":"+String((unsigned long)captureResultRetrySent);
    json += ",\"result_receipts\":"+String((unsigned long)captureResultReceiptReceived);
    json += ",\"result_retry_exhausted\":"+String((unsigned long)captureResultRetryExhausted);
    json += ",\"last_dispatch_offset_us\":"+String((long)captureLastDispatchOffsetUs);
    json += ",\"last_size_bytes\":"+String((unsigned long)captureLastSize);
    json += ",\"last_error\":"+String((unsigned)captureLastError);
    json += ",\"local_frames\":"+clusterCaptureLocalJson();
    json += "},\"job_probes\":[";
    ClusterJobs::Entry probeEntries[ClusterJobs::kMaxJobs];
    const size_t probeCount = ClusterJobs::snapshot(probeEntries, ClusterJobs::kMaxJobs);
    for (size_t j = 0; j < probeCount; ++j) {
        if (j) json += ',';
        const ClusterJobs::Entry &e = probeEntries[j];
        json += "{\"node\":\"" + jsonEscape(String(e.nodeId)) + "\"";
        json += ",\"kind\":" + String((unsigned int)e.kind);
        json += ",\"state\":" + String((unsigned int)e.state);
        json += ",\"result\":" + String((unsigned int)e.resultCode);
        // Keep the 64-bit job identity as two uint32 components: JSON numbers
        // cannot represent all uint64 identifiers exactly in JavaScript.
        json += ",\"job_boot\":" + String((unsigned long)(e.jobId >> 32));
        json += ",\"job_seq\":" + String((unsigned long)(uint32_t)e.jobId);
        if(e.kind==ClusterJobs::Kind::Capture){
            json += ",\"media_uuid\":\""+String(e.mediaUuid)+"\"";
            json += ",\"media_bytes\":"+String((unsigned long)e.mediaBytes);
            char dispatchText[30];snprintf(dispatchText,sizeof(dispatchText),"%lld",(long long)e.dispatchUtcUs);
            json += ",\"dispatch_utc_us\":\""+String(dispatchText)+"\"";
            if(e.mediaUuid[0]){
                const ClusterPeer *mediaPeer=findPeer(e.nodeId);
                if(mediaPeer && usableIp(mediaPeer->ip)){
                    json += ",\"media_url\":\"http://"+mediaPeer->ip.toString()+
                        "/capture_media?uuid="+String(e.mediaUuid)+"\"";
                }
            }
        }
        json += ",\"ack_latency_ms\":";
        if ((e.kind == ClusterJobs::Kind::Restart ||
             e.kind == ClusterJobs::Kind::Shutdown ||
             e.kind == ClusterJobs::Kind::Probe) &&
            (e.state == ClusterJobs::State::Accepted ||
             e.state == ClusterJobs::State::Succeeded))
            json += String((unsigned long)(e.updatedMs - e.createdMs));
        else json += "null";
        if (e.kind == ClusterJobs::Kind::Restart) {
            const RestartEvidence *evidence = nullptr;
            for (size_t k = 0; k < restartEvidenceCount; ++k)
                if (restartEvidence[k].jobId == e.jobId) { evidence = &restartEvidence[k]; break; }
            json += ",\"boot_change_observed\":";
            json += (evidence && evidence->observedNewBoot) ? "true" : "false";
        }

        json += '}';
    }
    json += "],\"known_clusters\":";
    json += clusterProfilesJson();
    json += '}';
    return json;
}


// Monotonic, independently synchronized cluster UTC (not the system RTC).
// Never use this as trusted licensing time or claim microsecond accuracy.
bool clusterTimeNowUs(int64_t &utcUs)
{
    if (!runtimeActive) return false;
    if (localIsCoordinator) {
        utcUs = localUnixTimeUs();
        return utcUs != 0;
    }
    // ESP-NOW provides an independent authenticated UTC estimate. A planned
    // capture must use the same time model even while radio service is paused.
    uint32_t espAgeMs=0;
    double espRttUs=0;
    if (droneModeEnabled() &&
        ClusterEspNowRadio::precisionUtcNow(currentCoordinatorEpoch,currentCoordinatorId,
                                            utcUs, espAgeMs, espRttUs)) return true;
    if (!timeEstimateValid || timeAuthorityEpoch != currentCoordinatorEpoch ||
        strcmp(timeAuthorityId, currentCoordinatorId) != 0 ||
        (uint32_t)(millis() - timeLastSampleMs) > CLUSTER_TIME_HOLDOVER_MS) return false;
    utcUs = timeAnchorUtcUs + (esp_timer_get_time() - timeAnchorMonoUs);
    return true;
}

// Cooperative main-loop priority gate while a local capture is armed.
bool clusterCaptureArmed(){return clusterCaptureArmedFlag;}

// A planned cluster shot is protected from dispatch/acceptance until post-shot.
// This is a local admission gate; it does not stop all third-party Wi-Fi traffic.
bool clusterCaptureProtectionActive() {
    if (clusterCaptureArmedFlag || pendingCapture.active) return true;
    return localIsCoordinator && coordinatorCaptureGuardUntilMs &&
           static_cast<int32_t>(millis()-coordinatorCaptureGuardUntilMs)<0;
}

// Beta126: codec callbacks only. These do NOT authorize a sender, accept a
// grant, queue a packet, or alter the production ESP-NOW beacon schedule.
bool clusterTimeGrantSign(const uint8_t *data, size_t len, uint8_t mac[32])
{
    return grantProtocolMac(data, len, mac);
}

bool clusterTimeGrantVerify(const uint8_t *data, size_t len, const uint8_t mac[32])
{
    if (!mac) return false;
    uint8_t expected[32] = {};
    if (!grantProtocolMac(data, len, expected)) return false;
    const bool valid = constantTimeEqual(expected, mac, sizeof(expected));
    secureWipe(expected, sizeof(expected));
    return valid;
}

bool clusterEspNowDiagnosticBeaconSign(const uint8_t *data, size_t len, uint8_t mac[32])
{
    return diagnosticBeaconMac(data, len, mac);
}

bool clusterEspNowDiagnosticBeaconVerify(const uint8_t *data, size_t len, const uint8_t mac[32])
{
    uint8_t expected[32] = {};
    if (!mac || !diagnosticBeaconMac(data, len, expected)) return false;
    uint8_t difference = 0;
    for (size_t i=0;i<sizeof(expected);++i) difference |= expected[i]^mac[i];
    secureWipe(expected,sizeof(expected));
    return difference==0;
}

// Callbacks for EspNowTimePackets::encode/decode. Never call from the
// ESP-NOW WiFi task: queue frames and verify in the ordinary cluster loop.
bool clusterEspNowTimeSign(const uint8_t *data, size_t len,
                           uint8_t mac[32], void *user)
{
    (void)user;
    return espNowTimeMac(data, len, mac);
}

bool clusterEspNowTimeVerify(const uint8_t *data, size_t len,
                             const uint8_t mac[32], void *user)
{
    (void)user;
    if (!mac) return false;
    uint8_t expected[32] = {};
    if (!espNowTimeMac(data, len, expected)) return false;
    const bool valid = constantTimeEqual(expected, mac, sizeof(expected));
    secureWipe(expected, sizeof(expected));
    return valid;
}
