#pragma once

#include <Arduino.h>

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
// Capture time in UTC microseconds; shared across all selected nodes.
bool clusterRequestNodeCapture(const String &nodeId, int64_t utcUs, String &error);
bool clusterRuntimeActive();
bool clusterLocalIsCoordinator();
// Sends an authenticated advisory time-sync trigger; no system-clock changes.
bool clusterRequestTimeSync(String &error);
bool clusterRequestNodeTimeSync(const String &nodeId, String &error);
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

// Independent UTC estimate, not applied to system clock/RTC or recording.
// false if authority time is missing or estimate too old.
bool clusterTimeNowUs(int64_t &utcUs);

// Beta72: harmless authenticated transport probe; coordinator only.
bool clusterSendJobProbe(const String &nodeId, String &error);
// Schedule a single remote restart. ACK confirms scheduling only.
bool clusterRequestNodeRestart(const String &nodeId, String &error);

// Schedule one remote protected shutdown; ACK only confirms scheduling.
bool clusterRequestNodeShutdown(const String &nodeId, String &error);
