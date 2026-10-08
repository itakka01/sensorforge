#include "cluster.h"

#include "config.h"
#include "cluster_profiles.h"
#include "device_identity.h"
#include "logger.h"
#include "recorder.h"
#include "sensorforge_version.h"
#include "streamer.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <psa/crypto.h>
#include <esp_system.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

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
static const size_t CLUSTER_PACKET_MAX = 511;
static const char *CLUSTER_SERVICE = "sfcluster";
static const char *CLUSTER_PROTO = "udp";
static const char *CLUSTER_DISCOVERY_MAGIC = "SFD1";
static const char *CLUSTER_PACKET_MAGIC = "SFC1";
static const char *CLUSTER_META_MAGIC = "SFM1";
static const char *CLUSTER_LEAVE_MAGIC = "SFL1";
static const char *CLUSTER_RESOURCE_MAGIC = "SFR1";
static const char *CLUSTER_COORDINATION_MAGIC = "SFCO1";
static const char *CLUSTER_NODE_HEARTBEAT_MAGIC = "SFNH1";
static const char *CLUSTER_NODE_ACK_MAGIC = "SFNA1";

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

static WiFiUDP clusterUdp;
static WiFiUDP coordinatorUdp;
static ClusterPeer peers[CLUSTER_MAX_PEERS] = {};
static ClusterResource resources[CLUSTER_MAX_RESOURCES] = {};
static ClusterDiscoveryNode discoveryNodes[CLUSTER_MAX_DISCOVERY_NODES] = {};
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
static uint32_t lastCoordinatorAckSequence = 0;
static int32_t localWifiRemainingSeconds = -1;
static CoordinatorPolicy runtimeCoordinatorPolicy = COORDINATOR_POLICY_AUTO;
static bool localIsCoordinator = false;
static uint32_t localCoordinatorEpoch = 0;
static char currentCoordinatorId[36] = {};
static IPAddress currentCoordinatorIp;
static uint32_t currentCoordinatorEpoch = 0;
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

static bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t length)
{
    if (!a || !b)
        return false;

    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i)
        difference |= (uint8_t)(a[i] ^ b[i]);
    return difference == 0;
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

    if (runtimeCoordinatorPolicy != COORDINATOR_POLICY_NODE_ONLY) {
        bestId = deviceIntegrationId().c_str();
        bestPolicy = runtimeCoordinatorPolicy;
    }

    for (size_t i = 0; i < CLUSTER_MAX_PEERS; ++i) {
        ClusterPeer &peer = peers[i];
        if (!peer.used || peer.coordinatorPolicy == COORDINATOR_POLICY_NODE_ONLY)
            continue;

        const uint32_t leaseSeconds = peerLeaseSeconds(peer);
        const uint64_t ageMs = (uint64_t)(uint32_t)(now - peer.lastSeenMs);
        if (ageMs > (uint64_t)leaseSeconds * 1000ULL)
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
        }
    }

    const bool changed =
        strcmp(currentCoordinatorId, newCoordinatorId) != 0 ||
        currentCoordinatorEpoch != newCoordinatorEpoch ||
        localIsCoordinator != newLocalIsCoordinator;

    if (changed) {
        const bool wasLocalCoordinator = localIsCoordinator;
        strncpy(currentCoordinatorId, newCoordinatorId, sizeof(currentCoordinatorId) - 1);
        currentCoordinatorId[sizeof(currentCoordinatorId) - 1] = '\0';
        currentCoordinatorIp = newCoordinatorIp;
        currentCoordinatorEpoch = newCoordinatorEpoch;
        localIsCoordinator = newLocalIsCoordinator;
        coordinationDirty = true;
        nextCoordinatorHeartbeatMs = 0;
        lastCoordinatorAckMs = 0;
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

    if (peer->coordinationSequence != 0 && remoteSequence < peer->coordinationSequence)
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
    peer->advertisedCoordinatorEpoch = advertisedEpoch;
    peer->lastCoordinatorInfoMs = now;
    peer->lastSeenMs = now;
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

    sendSignedPayloadTo(payload, remoteIp, CLUSTER_COORDINATOR_PORT, "cluster node ACK");
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
    if (fieldCount != 8 ||
        strcmp(fields[0], CLUSTER_NODE_ACK_MAGIC) != 0 ||
        strcmp(fields[1], runtimeClusterTag) != 0 ||
        !integrationIdValid(fields[2]) ||
        !integrationIdValid(fields[5])) {
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
        return;
    }

    if (localIsCoordinator ||
        nodeBoot != bootNonce ||
        strcmp(fields[5], deviceIntegrationId().c_str()) != 0 ||
        strcmp(fields[2], currentCoordinatorId) != 0 ||
        coordinatorEpoch == 0 ||
        coordinatorEpoch != currentCoordinatorEpoch) {
        return;
    }

    if (lastCoordinatorAckSequence != 0 &&
        heartbeatSequence <= lastCoordinatorAckSequence) {
        return;
    }

    const uint32_t now = millis();
    lastCoordinatorAckMs = now;
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

static void processPacket(
    char *packet,
    size_t packetLength,
    const IPAddress &remoteIp
)
{
    if (!authenticatePacket(packet, packetLength))
        return;

    char *fields[12] = {};
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
    payload += "0"; // reserved coordination/time-sync capability bitset
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
        if (lastError.startsWith("cluster coordinator heartbeat"))
            lastError = "";
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
    sequenceNumber = 0;
    nextHeartbeatMs = 0;
    nextCoordinatorHeartbeatMs = 0;
    directHeartbeatSequence = 0;
    lastCoordinatorAckMs = 0;
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
    if (runtimeActive) {
        // Best-effort signed goodbye before the owning WiFi lifecycle tears the
        // radio down. Unexpected power loss still falls back to the peer lease.
        sendLeave();
        consoleWrite("CLUSTER", "Runtime OFF");
    }

    clusterUdp.stop();
    coordinatorUdp.stop();
    runtimeActive = false;
    discoverySocketActive = false;
    discoveryScanLeaseUntilMs = 0;
    runtimeProfileRemembered = false;
    mdnsServiceAdvertised = false;
    nextHeartbeatMs = 0;
    nextCoordinatorHeartbeatMs = 0;
    directHeartbeatSequence = 0;
    lastCoordinatorAckMs = 0;
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

    if (runtimeHmacKey != 0) {
        psa_destroy_key(runtimeHmacKey);
        runtimeHmacKey = 0;
    }
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
    const uint32_t now = millis();

    if (discoveryScanLeaseUntilMs != 0 &&
        !discoveryScanWanted(now)) {
        discoveryScanLeaseUntilMs = 0;
        if (!runtimeActive && discoverySocketActive) {
            clusterUdp.stop();
            discoverySocketActive = false;
        }
    }

    if (discoveryScanLeaseUntilMs != 0)
        expireDiscoveryNodes(now);

    if (!runtimeActive && !discoverySocketActive)
        return;

    if (runtimeActive) {
        expirePeers(now);
        expireResources(now);
    }

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

    // Election is based only on currently leased authenticated nodes. A single
    // missed multicast packet therefore cannot change the Coordinator.
    recomputeCoordinator(now);

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
    json += ",\"local_role\":\"" +
        String(localIsCoordinator ? "coordinator" : "node") + "\"";
    json += ",\"coordinator_heartbeat_sec\":" +
        String((unsigned long)(CLUSTER_COORDINATOR_HEARTBEAT_MS / 1000UL));
    json += ",\"coordinator_lease_sec\":" +
        String((unsigned long)CLUSTER_COORDINATOR_LEASE_SEC);
    json += ",\"coordinator_port\":" +
        String((unsigned long)CLUSTER_COORDINATOR_PORT);
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
    json += ",\"release\":\"" + jsonEscape(String(SENSORFORGE_RELEASE_TAG)) + "\"";
    json += ",\"uptime_seconds\":" + String((unsigned long)(nowMs / 1000UL));
    json += ",\"time_valid\":";
    json += localTimeValid ? "true" : "false";
    json += ",\"epoch\":" + String((long long)localEpoch);
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
        json += ",\"time_valid\":";
        json += peer.timeValid ? "true" : "false";
        json += ",\"epoch\":" + String((long long)peer.epochSeconds);
        json += ",\"recording\":";
        json += peer.recording ? "true" : "false";
        json += ",\"streaming\":";
        json += peer.streaming ? "true" : "false";
        json += ",\"transport\":";
        json += peer.transportMode ? "true" : "false";
        json += ",\"coordinator_policy\":\"" +
            String(coordinatorPolicyName(peer.coordinatorPolicy)) + "\"";
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

    json += "],\"known_clusters\":";
    json += clusterProfilesJson();
    json += '}';
    return json;
}

