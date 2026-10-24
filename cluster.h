#pragma once

#include <Arduino.h>
#include "cluster_group_logic.h"

// Optional local SensorForge cluster runtime.
//
// Ownership rules:
// - WiFi lifecycle remains owned by sensorforge.ino.
// - The central mDNS responder remains owned by sensorforge.ino.
// - clusterNetworkStart()/Stop() only own the cluster UDP multicast and
//   Coordinator-unicast sockets plus their volatile peer/resource tables.
// - clusterMdnsAdvertise() only adds the cluster DNS-SD service after the
//   existing MDNS.begin() succeeded.
// - No recording/streaming/sleep policy is changed by this module.
// - The cluster must never start, prolong or keep WiFi alive.
// - Coordinator election is runtime-only; config defines candidacy policy, not a
//   forced master role. Time-Master/laser sync and remote commands are separate
//   future layers.

void clusterNetworkStart();
void clusterNetworkStop();
void clusterMdnsAdvertise();
void clusterLoop();

bool clusterRequestNodeWipe(const String &nodeId,String &error);
bool clusterRequestNodeDrone(const String &nodeId, bool enabled, String &error);

// --- Central WiFi TX power (cluster job SFJT1, WebConfig and API) ---
// Mode: Session = until reboot; Persistent = node applies, keeps the value
// only if it still reaches the coordinator after CLUSTER_TX_PROBATION_MS and
// then writes wifi_tx_power_dbm to its config (otherwise reverts itself);
// Config = drop a session value and return to the stored config value.
enum ClusterTxPowerMode : uint8_t {
    CLUSTER_TX_MODE_SESSION = 0,
    CLUSTER_TX_MODE_PERSISTENT = 1,
    CLUSTER_TX_MODE_CONFIG = 2
};
enum ClusterTxPowerState : uint8_t {
    CLUSTER_TX_STATE_NONE = 0,
    CLUSTER_TX_STATE_SENT = 1,       // coordinator: request sent, no answer yet
    CLUSTER_TX_STATE_SESSION = 2,    // applied until reboot
    CLUSTER_TX_STATE_PROBATION = 3,  // applied, waiting for link proof before saving
    CLUSTER_TX_STATE_PERSISTED = 4,  // applied and saved in config
    CLUSTER_TX_STATE_REVERTED = 5,   // link lost during probation, old value back
    CLUSTER_TX_STATE_CONFIG = 6,     // session value dropped, config value active
    CLUSTER_TX_STATE_FAILED = 7
};
enum ClusterTxPowerResult : uint8_t {
    CLUSTER_TX_RESULT_OK = 0,
    CLUSTER_TX_RESULT_APPLY_FAILED = 1,
    CLUSTER_TX_RESULT_SAVE_FAILED = 2,
    CLUSTER_TX_RESULT_INVALID = 3,
    CLUSTER_TX_RESULT_NO_ANSWER = 4
};
bool clusterTxPowerModeFromText(const String &text, ClusterTxPowerMode &mode);
const char *clusterTxPowerStateName(uint8_t state);
// One node (coordinator only).
bool clusterRequestNodeTxPower(const String &nodeId, int16_t valueX10,
                               ClusterTxPowerMode mode, String &error);
// This device (coordinator or node), same rules incl. probation.
bool clusterApplyLocalTxPower(int16_t valueX10, ClusterTxPowerMode mode, String &error);
// All online nodes (coordinator only), optionally this device too. Returns
// the number of requests sent; failed counts nodes that could not be sent.
size_t clusterRequestTxPowerAll(int16_t valueX10, ClusterTxPowerMode mode,
                                bool includeLocal, size_t &failed, String &error);
// Board limits, local state and the last result per node.
String clusterTxPowerJson();
// Capture time in UTC microseconds; shared across all selected nodes.
// testPulse: request the optional GPIO test pulse (SFJC1 field "tp1"), default off.
bool clusterRequestNodeCapture(const String &nodeId, int64_t utcUs, String &error, bool testPulse = false);
// Transport-only SFHT1 sender; not enabled in WebConfig until measurement safety is complete.
bool clusterRequestNodeHwTime(const String &nodeId, uint8_t channel, int64_t utcUs, String &error, uint32_t *sequenceOut = nullptr);
// Three-node one-shot GPIO diagnostic, disabled by default hardware pin mapping.
bool clusterHwTimeStart(const String nodeIds[3], String &error);
String clusterHwTimeStatusJson();
String clusterHwTimeCsv();
bool clusterRuntimeActive();
bool clusterLocalIsCoordinator();
bool clusterRestartRequired();
const char *clusterLastError();

// Stable public cluster identity. Existing Beta-37 configs without cluster_id
// receive a deterministic legacy ID derived from the cluster name; new clusters
// should persist the returned ID on the next save.
String clusterEffectiveId();
String clusterGenerateId();
bool clusterIdValid(const String &clusterId);

// Temporary passive discovery listener used only while the Cluster WebConfig
// page is actively polling. It never starts/extends WiFi and never transmits.
void clusterDiscoveryTouch();
bool clusterDiscoveryScannerActive();

// Number of currently authenticated remote peers in the active cluster. Used
// to block unsafe local password replacement of a healthy multi-node cluster.
size_t clusterAuthenticatedPeerCount();

// Result of validating candidate credentials against recent authenticated
// multicast Presence traffic from a currently discovered remote cluster.
// No password or derived key is transmitted; verification is entirely local.
enum ClusterCredentialCheckResult : uint8_t {
    CLUSTER_CREDENTIAL_NOT_DISCOVERED = 0,
    CLUSTER_CREDENTIAL_VERIFIED = 1,
    CLUSTER_CREDENTIAL_INVALID_PASSWORD = 2,
    CLUSTER_CREDENTIAL_PROOF_PENDING = 3,
    CLUSTER_CREDENTIAL_EPOCH_MISMATCH = 4,
    CLUSTER_CREDENTIAL_INTERNAL_ERROR = 5
};

ClusterCredentialCheckResult clusterVerifyDiscoveredCredentials(
    const String &clusterId,
    uint32_t credentialEpoch,
    const String &password,
    String &error,
    uint32_t &verifiedEpoch
);


// Approximate remaining lifetime of the current WiFi session as calculated by
// the existing SensorForge WiFi lifecycle. -1 means unknown/unbounded. This is
// status metadata only; cluster code must never act on it to keep WiFi alive.
void clusterSetWifiRemainingSeconds(int32_t remainingSeconds);

// Generic resource announcement foundation. Only small metadata is sent over
// multicast; the resource bytes themselves are never broadcast. A concrete
// producer (for example a captured JPEG) supplies a stable resource ID, type,
// capture timestamp and a future pull locator. Automatic resource transfer is
// deliberately a separate step and is not performed by this function.
struct ClusterResourceAnnouncement {
    String resourceId;       // stable within the source node while advertised
    String resourceType;     // e.g. "image/jpeg"
    int64_t timestampUs;     // UTC epoch microseconds if known, otherwise 0
    uint32_t sizeBytes;      // 0 if not known yet
    uint32_t ttlSeconds;     // 1..3600
    String locator;          // compact source-relative locator, not resource data
};

bool clusterAnnounceResource(
    const ClusterResourceAnnouncement &announcement,
    String &error
);

// Lightweight JSON snapshot used by the dedicated WebConfig status page.
// Contains the local node, only authenticated peers from the same cluster and
// currently valid resource announcements.
String clusterStatusJson();
// Same JSON in parts (each >= 4 KB except the last) for chunked HTTP, so a
// 100-node status never has to exist as one String.
typedef void (*ClusterStatusChunkSink)(String &chunk, void *ctx);
void clusterStatusJsonStream(ClusterStatusChunkSink sink, void *ctx);
// ESP-NOW time service only; call between parts of long HTTP responses.
void clusterServiceTimeCritical();

// Independent UTC estimate, not applied to system clock/RTC or recording.
// false if authority time is missing or estimate too old.
bool clusterTimeNowUs(int64_t &utcUs);

// Beta72: harmless authenticated transport probe; coordinator only.
bool clusterSendJobProbe(const String &nodeId, String &error);
// Schedule a single remote restart. ACK confirms scheduling only.
bool clusterRequestNodeRestart(const String &nodeId, String &error);

// Schedule one remote protected shutdown; ACK only confirms scheduling.
bool clusterRequestNodeShutdown(const String &nodeId, String &error);

// ESP-NOW time authentication adapters, using a dedicated domain-separated
// HMAC key derived from the active authenticated cluster runtime.
// Crypto work must not run directly in an ESP-NOW radio callback.
bool clusterEspNowTimeSign(const uint8_t *data, size_t length,
                           uint8_t mac[32], void *user);
bool clusterEspNowTimeVerify(const uint8_t *data, size_t length,
                             const uint8_t mac[32], void *user);

// True from accepting/planning a shot through its protected deadline.
// Storage/OTA admission must treat this as stronger than the 2s armed flag.
bool clusterCaptureProtectionActive();

// Optional node mount metadata (group, position, height, orientation).
// Carried in the authenticated SFG1 UDP side packet next to the heartbeat;
// display/selection only, never used for election, sync or capture timing.
struct ClusterMountEntry {
    char integrationId[36];
    char hostname[64];
    uint32_t bootNonce;           // current boot of the device (joins ESP-NOW reports)
    bool local;
    bool mountKnown;              // false: peer firmware sends no mount data
    ClusterGroups::Mount mount;
};

// Local device plus authenticated online peers; returns the number written.
size_t clusterMountSnapshot(ClusterMountEntry *out, size_t capacity);
size_t clusterMountSnapshotCapacity();
