#include "cluster_groups.h"

#include "cluster.h"
#include "cluster_group_logic.h"
#include "psram_alloc.h"

#include <Preferences.h>
#include <string.h>

namespace {

static const char *GROUPS_NAMESPACE = "sfgroups";
static const char *GROUPS_KEY = "store";

struct GroupsState {
    ClusterGroups::Store store;
    ClusterGroups::ActivityTable activity;
    bool loaded;
};

// Lazily allocated in PSRAM (never internal RAM, see psram_alloc.h). Without
// PSRAM the group view reports an error; the cluster itself is unaffected.
static GroupsState *g_state = nullptr;
static bool g_allocFailed = false;

static GroupsState *state()
{
    if (g_state || g_allocFailed)
        return g_state;
    g_state = static_cast<GroupsState *>(sfPsramCalloc(1, sizeof(GroupsState)));
    if (!g_state) {
        g_allocFailed = true;
        Serial.println("[GROUPS] no PSRAM for group table, group management disabled");
        return nullptr;
    }
    ClusterGroups::storeClear(g_state->store);
    return g_state;
}

static GroupsState *loadedState()
{
    GroupsState *s = state();
    if (!s || s->loaded)
        return s;
    s->loaded = true;
    Preferences prefs;
    if (!prefs.begin(GROUPS_NAMESPACE, true))
        return s;               // namespace not created yet: empty store
    const size_t len = prefs.getBytesLength(GROUPS_KEY);
    if (len == sizeof(ClusterGroups::Store)) {
        SfPsramBuffer blob(len);
        if (blob && prefs.getBytes(GROUPS_KEY, blob.data(), len) == len &&
            !ClusterGroups::storeLoad(s->store, blob.data(), len))
            Serial.println("[GROUPS] stored group table invalid, ignored");
    } else if (len) {
        Serial.println("[GROUPS] stored group table has unknown size, ignored");
    }
    prefs.end();
    return s;
}

static bool persist(const ClusterGroups::Store &store, String &error)
{
    Preferences prefs;
    if (!prefs.begin(GROUPS_NAMESPACE, false)) {
        error = "NVS nicht verfügbar";
        return false;
    }
    const size_t written = prefs.putBytes(GROUPS_KEY, &store, sizeof(store));
    prefs.end();
    if (written != sizeof(store)) {
        error = "Gruppen konnten nicht gespeichert werden (NVS)";
        return false;
    }
    return true;
}

struct Snapshot {
    ClusterMountEntry *entries;
    size_t count;
    Snapshot() : entries(nullptr), count(0)
    {
        const size_t cap = clusterMountSnapshotCapacity();
        entries = static_cast<ClusterMountEntry *>(sfPsramCalloc(cap, sizeof(ClusterMountEntry)));
        if (entries)
            count = clusterMountSnapshot(entries, cap);
    }
    ~Snapshot() { heap_caps_free(entries); }
    Snapshot(const Snapshot &) = delete;
    Snapshot &operator=(const Snapshot &) = delete;

    size_t nodesIn(const char *groupId) const
    {
        size_t n = 0;
        for (size_t i = 0; i < count; ++i)
            if (entries[i].mountKnown && strcmp(entries[i].mount.group, groupId) == 0)
                ++n;
        return n;
    }
};

static bool normalizedId(const String &text, char out[ClusterGroups::kIdMaxLen + 1], String &error)
{
    if (!ClusterGroups::normalizeId(text.c_str(), out) || !out[0]) {
        error = "Ungültige Gruppen-ID";
        return false;
    }
    return true;
}

static void appendDeci(String &json, int32_t deci)
{
    if (deci == ClusterGroups::kUnset) {
        json += "null";
        return;
    }
    char text[16];
    ClusterGroups::formatDeci(deci, text);
    json += text;
}

} // namespace

void clusterGroupsNoteActive(const char *groupId)
{
    if (!groupId || !groupId[0])
        return;
    GroupsState *s = state();
    if (!s)
        return;
    const uint32_t now = millis();
    ClusterGroups::activityNote(s->activity, groupId, now, now);
}

String clusterGroupsJson()
{
    GroupsState *s = loadedState();
    if (!s)
        return "{\"ok\":false,\"error\":\"psram_unavailable\",\"groups\":[]}";
    Snapshot snap;
    if (!snap.entries)
        return "{\"ok\":false,\"error\":\"psram_unavailable\",\"groups\":[]}";

    const uint32_t now = millis();

    // Union of stored groups and groups reported by online nodes, sorted.
    const size_t maxIds = ClusterGroups::kMaxGroups + snap.count;
    char (*ids)[ClusterGroups::kIdMaxLen + 1] =
        static_cast<char (*)[ClusterGroups::kIdMaxLen + 1]>(sfPsramCalloc(maxIds, ClusterGroups::kIdMaxLen + 1));
    if (!ids)
        return "{\"ok\":false,\"error\":\"psram_unavailable\",\"groups\":[]}";
    size_t idCount = 0;
    auto addId = [&](const char *id) {
        if (!id[0])
            return;
        for (size_t i = 0; i < idCount; ++i)
            if (strcmp(ids[i], id) == 0)
                return;
        if (idCount < maxIds)
            strncpy(ids[idCount++], id, ClusterGroups::kIdMaxLen);
    };
    for (size_t i = 0; i < ClusterGroups::kMaxGroups; ++i)
        addId(s->store.records[i].id);
    size_t ungrouped = 0;
    size_t unknown = 0;
    for (size_t i = 0; i < snap.count; ++i) {
        if (!snap.entries[i].mountKnown)
            ++unknown;
        else if (!snap.entries[i].mount.group[0])
            ++ungrouped;
        else
            addId(snap.entries[i].mount.group);
    }
    for (size_t i = 1; i < idCount; ++i)
        for (size_t j = i; j > 0 && strcmp(ids[j - 1], ids[j]) > 0; --j) {
            char tmp[ClusterGroups::kIdMaxLen + 1];
            memcpy(tmp, ids[j], sizeof(tmp));
            memcpy(ids[j], ids[j - 1], sizeof(tmp));
            memcpy(ids[j - 1], tmp, sizeof(tmp));
        }

    String json;
    json.reserve(256 + idCount * 160);
    json += "{\"ok\":true,\"stored\":" + String((unsigned)ClusterGroups::storeCount(s->store));
    json += ",\"max_stored\":" + String((unsigned)ClusterGroups::kMaxGroups);
    json += ",\"ungrouped_nodes\":" + String((unsigned)ungrouped);
    json += ",\"unknown_mount_nodes\":" + String((unsigned)unknown);
    json += ",\"groups\":[";
    for (size_t g = 0; g < idCount; ++g) {
        const char *id = ids[g];
        const ClusterGroups::Record *rec = ClusterGroups::storeFind(s->store, id);
        const ClusterGroups::Activity *act = ClusterGroups::activityFind(s->activity, id);
        const size_t nodeCount = snap.nodesIn(id);
        const ClusterGroups::State st =
            ClusterGroups::groupState(rec, act, nodeCount > 0, now, now);
        if (g)
            json += ',';
        json += "{\"id\":\"" + String(id) + "\"";
        json += ",\"state\":\"" + String(ClusterGroups::stateName(st)) + "\"";
        json += ",\"stored\":";
        json += rec ? "true" : "false";
        json += ",\"orientation_deg\":";
        appendDeci(json, rec ? rec->orientationDeci : ClusterGroups::kUnset);
        json += ",\"distance_mm\":";
        json += rec && rec->distanceMm != ClusterGroups::kUnset ? String((long)rec->distanceMm) : String("null");
        json += ",\"last_seen_age_s\":";
        json += act ? String((unsigned long)((now - act->lastSeenMs) / 1000UL)) : String("null");
        json += ",\"nodes\":[";
        bool first = true;
        for (size_t i = 0; i < snap.count; ++i) {
            const ClusterMountEntry &e = snap.entries[i];
            if (!e.mountKnown || strcmp(e.mount.group, id) != 0)
                continue;
            if (!first)
                json += ',';
            first = false;
            String host;
            for (const char *p = e.hostname; *p; ++p)
                if (*p != '"' && *p != '\\' && (uint8_t)*p >= 0x20)
                    host += *p;
            json += "{\"hostname\":\"" + host + "\",\"position\":\"" + String(e.mount.position) + "\"";
            json += ",\"local\":";
            json += e.local ? "true" : "false";
            json += '}';
        }
        json += "]}";
    }
    json += "]}";
    heap_caps_free(ids);
    return json;
}

bool clusterGroupsSave(const String &groupId, const String &orientationDeg,
                       const String &distanceMm, String &error)
{
    GroupsState *s = loadedState();
    if (!s) {
        error = "Kein PSRAM für die Gruppenverwaltung";
        return false;
    }
    char id[ClusterGroups::kIdMaxLen + 1];
    if (!normalizedId(groupId, id, error))
        return false;
    int32_t orientation = ClusterGroups::kUnset;
    if (!ClusterGroups::parseDeci(orientationDeg.c_str(), 0, ClusterGroups::kGroupOrientMaxDeci, true, orientation)) {
        error = "Ausrichtung: leer oder 0 bis 359,9°";
        return false;
    }
    int32_t distance = ClusterGroups::kUnset;
    if (!ClusterGroups::parseOptionalInt(distanceMm.c_str(), ClusterGroups::kDistanceMaxMm, true, distance)) {
        error = "Abstand: leer oder ganze Millimeter 0 bis 1000000";
        return false;
    }

    // Modify a PSRAM copy (the tables are too large for the web handler
    // stack), so a failed NVS write leaves the RAM view unchanged.
    SfPsramBuffer copy(sizeof(GroupsState));
    if (!copy) {
        error = "Kein PSRAM für die Gruppenverwaltung";
        return false;
    }
    GroupsState &next = *reinterpret_cast<GroupsState *>(copy.data());
    next = *s;
    if (!ClusterGroups::storeUpsert(next.store, id, orientation, distance)) {
        error = "Maximal " + String((unsigned)ClusterGroups::kMaxGroups) +
            " Gruppen speicherbar – bitte nicht mehr benötigte inaktive Gruppen löschen";
        return false;
    }
    const uint32_t now = millis();
    ClusterGroups::applyLapsesForSave(next.store, next.activity, id, now, now);
    if (!persist(next.store, error))
        return false;
    *s = next;
    return true;
}

bool clusterGroupsDelete(const String &groupId, String &error)
{
    GroupsState *s = loadedState();
    if (!s) {
        error = "Kein PSRAM für die Gruppenverwaltung";
        return false;
    }
    char id[ClusterGroups::kIdMaxLen + 1];
    if (!normalizedId(groupId, id, error))
        return false;
    {
        Snapshot snap;
        if (!snap.entries) {
            error = "Kein PSRAM für die Gruppenverwaltung";
            return false;
        }
        if (snap.nodesIn(id)) {
            error = "Gruppe hat aktive Nodes und kann nicht gelöscht werden";
            return false;
        }
    }
    SfPsramBuffer copy(sizeof(ClusterGroups::Store));
    if (!copy) {
        error = "Kein PSRAM für die Gruppenverwaltung";
        return false;
    }
    ClusterGroups::Store &next = *reinterpret_cast<ClusterGroups::Store *>(copy.data());
    next = s->store;
    if (!ClusterGroups::storeRemove(next, id)) {
        error = "Gruppe ist nicht gespeichert";
        return false;
    }
    if (!persist(next, error))
        return false;
    s->store = next;
    return true;
}
