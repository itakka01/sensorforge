#include "cluster_profiles.h"

#include "config_secrets.h"

#include <Preferences.h>
#include <string.h>

namespace {

static const char *PROFILE_NAMESPACE = "sfclprof";

static bool idValid(const String &id)
{
    if (id.length() != 35 || !id.startsWith("cl-"))
        return false;

    for (size_t i = 3; i < id.length(); ++i) {
        const char c = id[i];
        const bool hex =
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f');
        if (!hex)
            return false;
    }
    return true;
}

static String keyFor(char prefix, size_t index)
{
    String key;
    key.reserve(4);
    key += prefix;
    key += String((unsigned int)index);
    return key;
}

static String passwordAad(const String &clusterId)
{
    return String("cluster_profile_password:") + clusterId;
}

static bool loadSlot(
    Preferences &prefs,
    size_t index,
    ClusterKnownProfileInfo &info,
    String *storedPassword = nullptr
)
{
    info = ClusterKnownProfileInfo{};

    const String id = prefs.getString(keyFor('i', index).c_str(), "");
    if (!idValid(id))
        return false;

    info.valid = true;
    info.clusterId = id;
    info.clusterName = prefs.getString(keyFor('n', index).c_str(), "");
    info.credentialEpoch = prefs.getUInt(keyFor('e', index).c_str(), 1);
    if (info.credentialEpoch == 0)
        info.credentialEpoch = 1;

    const String encrypted = prefs.getString(keyFor('p', index).c_str(), "");
    info.hasPassword = encrypted.length() != 0;
    if (storedPassword)
        *storedPassword = encrypted;
    return true;
}

static int findSlot(Preferences &prefs, const String &clusterId)
{
    for (size_t i = 0; i < SENSORFORGE_CLUSTER_PROFILE_COUNT; ++i) {
        ClusterKnownProfileInfo info;
        if (loadSlot(prefs, i, info) && info.clusterId == clusterId)
            return (int)i;
    }
    return -1;
}

static int freeSlot(Preferences &prefs)
{
    for (size_t i = 0; i < SENSORFORGE_CLUSTER_PROFILE_COUNT; ++i) {
        ClusterKnownProfileInfo info;
        if (!loadSlot(prefs, i, info))
            return (int)i;
    }
    return -1;
}

static String jsonEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t c = (uint8_t)value[i];
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned int)c);
                    out += buffer;
                } else {
                    out += (char)c;
                }
                break;
        }
    }
    return out;
}

} // namespace

bool clusterProfilesGetInfo(
    const String &clusterId,
    ClusterKnownProfileInfo &info
)
{
    info = ClusterKnownProfileInfo{};
    if (!idValid(clusterId))
        return false;

    Preferences prefs;
    if (!prefs.begin(PROFILE_NAMESPACE, true))
        return false;

    const int slot = findSlot(prefs, clusterId);
    bool ok = false;
    if (slot >= 0)
        ok = loadSlot(prefs, (size_t)slot, info);
    prefs.end();
    return ok;
}

bool clusterProfilesGetPassword(
    const String &clusterId,
    uint32_t expectedCredentialEpoch,
    String &password,
    bool &staleEpoch,
    String &error
)
{
    password = "";
    staleEpoch = false;
    error = "";

    if (!idValid(clusterId)) {
        error = "invalid cluster profile ID";
        return false;
    }

    Preferences prefs;
    // On a brand-new device the profile namespace does not exist yet.
    // Opening NVS read-only reports that state as begin()==false, which is not
    // a storage failure. This lookup is part of an explicit save/join action,
    // so open read-write here to create the empty namespace if needed. A real
    // NVS failure still makes begin() fail and remains an error.
    if (!prefs.begin(PROFILE_NAMESPACE, false)) {
        error = "cluster profile storage unavailable";
        return false;
    }

    const int slot = findSlot(prefs, clusterId);
    if (slot < 0) {
        prefs.end();
        return false;
    }

    ClusterKnownProfileInfo info;
    String storedPassword;
    if (!loadSlot(prefs, (size_t)slot, info, &storedPassword)) {
        prefs.end();
        return false;
    }
    prefs.end();

    if (expectedCredentialEpoch != 0 &&
        info.credentialEpoch != expectedCredentialEpoch) {
        staleEpoch = true;
        return false;
    }

    if (!storedPassword.length())
        return false;

    bool wasEncrypted = false;
    String decodeError;
    if (!configSecretDecode(
            passwordAad(clusterId).c_str(),
            storedPassword,
            password,
            wasEncrypted,
            decodeError
        ) || !wasEncrypted) {
        password = "";
        error = decodeError.length()
            ? decodeError
            : String("cluster profile password is not protected");
        return false;
    }

    return password.length() != 0;
}

bool clusterProfilesRemember(
    const String &clusterId,
    const String &clusterName,
    const String &password,
    uint32_t credentialEpoch,
    String &error
)
{
    error = "";
    if (!idValid(clusterId) || !clusterName.length() ||
        password.length() < 8 || password.length() > 63 ||
        credentialEpoch == 0) {
        error = "invalid cluster profile data";
        return false;
    }

    Preferences prefs;
    if (!prefs.begin(PROFILE_NAMESPACE, false)) {
        error = "cluster profile storage unavailable";
        return false;
    }

    int slot = findSlot(prefs, clusterId);
    if (slot < 0)
        slot = freeSlot(prefs);
    if (slot < 0) {
        const uint32_t next = prefs.getUInt("next", 0);
        slot = (int)(next % SENSORFORGE_CLUSTER_PROFILE_COUNT);
        prefs.putUInt(
            "next",
            (uint32_t)((slot + 1) % SENSORFORGE_CLUSTER_PROFILE_COUNT)
        );
    }

    ClusterKnownProfileInfo existing;
    String existingProtected;
    const bool hadExisting = loadSlot(
        prefs,
        (size_t)slot,
        existing,
        &existingProtected
    );

    if (hadExisting && existing.clusterId == clusterId &&
        existing.clusterName == clusterName &&
        existing.credentialEpoch == credentialEpoch &&
        existingProtected.length()) {
        String existingPassword;
        bool wasEncrypted = false;
        String decodeError;
        if (configSecretDecode(
                passwordAad(clusterId).c_str(),
                existingProtected,
                existingPassword,
                wasEncrypted,
                decodeError
            ) && wasEncrypted && existingPassword == password) {
            prefs.end();
            return true;
        }
    }

    String protectedPassword;
    if (!configSecretEncrypt(
            passwordAad(clusterId).c_str(),
            password,
            protectedPassword,
            error
        )) {
        prefs.end();
        return false;
    }

    bool ok = true;
    ok = prefs.putString(keyFor('i', slot).c_str(), clusterId) > 0 && ok;
    ok = prefs.putString(keyFor('n', slot).c_str(), clusterName) > 0 && ok;
    ok = prefs.putUInt(keyFor('e', slot).c_str(), credentialEpoch) > 0 && ok;
    ok = prefs.putString(keyFor('p', slot).c_str(), protectedPassword) > 0 && ok;
    prefs.end();

    if (!ok) {
        error = "cluster profile write failed";
        return false;
    }
    return true;
}

String clusterProfilesJson()
{
    String json = "[";

    Preferences prefs;
    if (!prefs.begin(PROFILE_NAMESPACE, true)) {
        json += ']';
        return json;
    }

    bool first = true;
    for (size_t i = 0; i < SENSORFORGE_CLUSTER_PROFILE_COUNT; ++i) {
        ClusterKnownProfileInfo info;
        if (!loadSlot(prefs, i, info))
            continue;

        if (!first)
            json += ',';
        first = false;

        json += "{\"cluster_id\":\"" + jsonEscape(info.clusterId) + "\"";
        json += ",\"cluster_name\":\"" + jsonEscape(info.clusterName) + "\"";
        json += ",\"credential_epoch\":" + String((unsigned long)info.credentialEpoch);
        json += ",\"password_saved\":" + String(info.hasPassword ? "true" : "false");
        json += '}';
    }
    prefs.end();

    json += ']';
    return json;
}
