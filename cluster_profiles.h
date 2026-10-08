#pragma once

#include <Arduino.h>

// Board-local cache of cluster credentials that were successfully used before.
// Passwords are stored only as SFSEC1 ciphertext in NVS and are never exposed
// through JSON/WebConfig. The cache is convenience state; active cluster
// membership continues to come exclusively from config.txt.

static const size_t SENSORFORGE_CLUSTER_PROFILE_COUNT = 6;

struct ClusterKnownProfileInfo {
    bool valid;
    String clusterId;
    String clusterName;
    uint32_t credentialEpoch;
    bool hasPassword;
};

bool clusterProfilesGetInfo(
    const String &clusterId,
    ClusterKnownProfileInfo &info
);

// Returns a remembered plaintext password only inside firmware RAM. If
// expectedCredentialEpoch is non-zero and differs from the stored epoch,
// staleEpoch is set and no password is returned.
bool clusterProfilesGetPassword(
    const String &clusterId,
    uint32_t expectedCredentialEpoch,
    String &password,
    bool &staleEpoch,
    String &error
);

// Remember credentials only after a successful authenticated membership (or
// explicit creation of a new local cluster). Repeated calls with identical
// data do not rewrite NVS.
bool clusterProfilesRemember(
    const String &clusterId,
    const String &clusterName,
    const String &password,
    uint32_t credentialEpoch,
    String &error
);

// Small JSON array for WebConfig. Contains IDs/names/epochs only, never secrets.
String clusterProfilesJson();
