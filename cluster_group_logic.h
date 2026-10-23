#pragma once
// Pure, host-testable logic for node mount metadata and coordinator-local
// group records. No Arduino, no I/O, no radio: cluster.cpp carries the values
// in the authenticated SFG1 UDP side packet, cluster_groups.cpp persists the
// group records in NVS.
//
// Group membership is display/selection metadata only. It never influences
// coordinator election, ESP-NOW time sync, recovery slots or capture timing.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ClusterGroups {

static const size_t kIdMaxLen = 15;          // group_id / mount_position
static const size_t kMaxGroups = 32;         // persisted group records
static const size_t kMaxActivity = 32;       // RAM activity entries
static const int32_t kUnset = INT32_MIN;     // optional numeric value not set

static const int32_t kHeightMaxMm = 100000;          // 100 m
static const int32_t kDistanceMaxMm = 1000000;       // 1 km
static const int32_t kOrientHMaxDeci = 1800;         // +/-180.0 deg
static const int32_t kOrientVMaxDeci = 900;          // +/-90.0 deg
static const int32_t kGroupOrientMaxDeci = 3599;     // 0.0..359.9 deg

// Absence for at least this long turns stored values into suggestions.
static const uint32_t kLapseMs = 600000UL;

// Normalizes an identifier: trims ASCII whitespace, uppercases, requires
// [A-Z0-9] as first character and [A-Z0-9_.-] afterwards, max kIdMaxLen.
// Empty input is valid and yields "". Returns false on invalid input.
inline bool normalizeId(const char *in, char out[kIdMaxLen + 1])
{
    out[0] = '\0';
    if (!in)
        return true;
    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n')
        ++in;
    size_t len = strlen(in);
    while (len && (in[len - 1] == ' ' || in[len - 1] == '\t' ||
                   in[len - 1] == '\r' || in[len - 1] == '\n'))
        --len;
    if (len == 0)
        return true;
    if (len > kIdMaxLen)
        return false;
    for (size_t i = 0; i < len; ++i) {
        char c = in[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '_' && c != '-' && c != '.')))
            return false;
        out[i] = c;
    }
    out[len] = '\0';
    return true;
}

// Parses an optional decimal number ("12", "-7.5", "12,25") into tenths,
// rounded half away from zero. Empty text yields kUnset when allowEmpty.
inline bool parseDeci(const char *text, int32_t minDeci, int32_t maxDeci,
                      bool allowEmpty, int32_t &out)
{
    out = kUnset;
    if (!text)
        return allowEmpty;
    while (*text == ' ')
        ++text;
    size_t len = strlen(text);
    while (len && text[len - 1] == ' ')
        --len;
    if (len == 0)
        return allowEmpty;
    size_t i = 0;
    bool negative = false;
    if (text[i] == '-' || text[i] == '+') {
        negative = text[i] == '-';
        ++i;
    }
    int64_t whole = 0;
    size_t wholeDigits = 0;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        whole = whole * 10 + (text[i] - '0');
        if (whole > 100000000LL)
            return false;
        ++i;
        ++wholeDigits;
    }
    int64_t frac = 0;   // in thousandths
    size_t fracDigits = 0;
    if (i < len && (text[i] == '.' || text[i] == ',')) {
        ++i;
        while (i < len && text[i] >= '0' && text[i] <= '9') {
            if (fracDigits < 3) {
                frac = frac * 10 + (text[i] - '0');
                ++fracDigits;
            }
            ++i;
        }
        if (fracDigits == 0)
            return false;
        while (fracDigits < 3) {
            frac *= 10;
            ++fracDigits;
        }
    }
    if (i != len || wholeDigits == 0)
        return false;
    int64_t thousandths = whole * 1000 + frac;
    int64_t deci = (thousandths + 50) / 100;
    if (negative)
        deci = -deci;
    if (deci < minDeci || deci > maxDeci)
        return false;
    out = (int32_t)deci;
    return true;
}

// Formats tenths as "12.5" / "-0.5" / "12" ("" for kUnset).
inline void formatDeci(int32_t deci, char out[16])
{
    if (deci == kUnset) {
        out[0] = '\0';
        return;
    }
    const bool negative = deci < 0;
    const uint32_t mag = negative ? (uint32_t)(-(int64_t)deci) : (uint32_t)deci;
    char *p = out;
    if (negative)
        *p++ = '-';
    char digits[12];
    size_t n = 0;
    uint32_t whole = mag / 10;
    do {
        digits[n++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    while (n)
        *p++ = digits[--n];
    if (mag % 10) {
        *p++ = '.';
        *p++ = (char)('0' + mag % 10);
    }
    *p = '\0';
}

// Optional non-negative integer ("" -> kUnset when allowEmpty).
inline bool parseOptionalInt(const char *text, int32_t maxValue, bool allowEmpty,
                             int32_t &out)
{
    int32_t deci = kUnset;
    if (!parseDeci(text, 0, maxValue * 10, allowEmpty, deci))
        return false;
    if (deci == kUnset) {
        out = kUnset;
        return true;
    }
    if (deci % 10 != 0)
        return false;           // whole millimetres only
    out = deci / 10;
    return true;
}

struct Mount {
    char group[kIdMaxLen + 1];
    char position[kIdMaxLen + 1];
    int32_t heightMm;            // kUnset if unknown
    int32_t orientHDeci;         // default 0
    int32_t orientVDeci;         // default 0
};

inline void mountClear(Mount &m)
{
    memset(&m, 0, sizeof(m));
    m.heightMm = kUnset;
}

inline bool mountValid(const Mount &m)
{
    char tmp[kIdMaxLen + 1];
    return normalizeId(m.group, tmp) && strcmp(tmp, m.group) == 0 &&
           normalizeId(m.position, tmp) && strcmp(tmp, m.position) == 0 &&
           (m.heightMm == kUnset || (m.heightMm >= 0 && m.heightMm <= kHeightMaxMm)) &&
           m.orientHDeci >= -kOrientHMaxDeci && m.orientHDeci <= kOrientHMaxDeci &&
           m.orientVDeci >= -kOrientVMaxDeci && m.orientVDeci <= kOrientVMaxDeci;
}

// ---- Packet fields: group|position|height|h_deci|v_deci --------------------
// The cluster splitter collapses empty fields, so "-" encodes an empty ID and
// "-1" an unset height. "-" can never be a valid ID (first char must be alnum).

inline void encodeId(const char *id, char out[kIdMaxLen + 1])
{
    if (id && id[0]) {
        strncpy(out, id, kIdMaxLen);
        out[kIdMaxLen] = '\0';
    } else {
        out[0] = '-';
        out[1] = '\0';
    }
}

inline bool decodeId(const char *field, char out[kIdMaxLen + 1])
{
    if (!field)
        return false;
    if (strcmp(field, "-") == 0) {
        out[0] = '\0';
        return true;
    }
    if (!field[0])
        return false;
    char tmp[kIdMaxLen + 1];
    if (!normalizeId(field, tmp) || strcmp(tmp, field) != 0)
        return false;           // sender must already send canonical form
    memcpy(out, tmp, sizeof(tmp));
    return true;
}

// Parses a strict decimal integer field (optional leading '-').
inline bool parseIntField(const char *text, int32_t minValue, int32_t maxValue,
                          int32_t &out)
{
    if (!text || !*text)
        return false;
    size_t i = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        i = 1;
    }
    if (!text[i])
        return false;
    int64_t v = 0;
    for (; text[i]; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        v = v * 10 + (text[i] - '0');
        if (v > 2147483647LL)
            return false;
    }
    if (negative)
        v = -v;
    if (v < minValue || v > maxValue)
        return false;
    out = (int32_t)v;
    return true;
}

inline bool decodeMountFields(const char *group, const char *position,
                              const char *height, const char *hDeci,
                              const char *vDeci, Mount &out)
{
    Mount m;
    mountClear(m);
    int32_t h = 0;
    if (!decodeId(group, m.group) || !decodeId(position, m.position) ||
        !parseIntField(height, -1, kHeightMaxMm, h) ||
        !parseIntField(hDeci, -kOrientHMaxDeci, kOrientHMaxDeci, m.orientHDeci) ||
        !parseIntField(vDeci, -kOrientVMaxDeci, kOrientVMaxDeci, m.orientVDeci))
        return false;
    m.heightMm = h < 0 ? kUnset : h;
    out = m;
    return true;
}

// ---- Coordinator group records ---------------------------------------------

struct Record {
    char id[kIdMaxLen + 1];      // "" marks an unused slot
    int32_t orientationDeci;     // kUnset or 0..kGroupOrientMaxDeci
    int32_t distanceMm;          // kUnset or 0..kDistanceMaxMm
    uint8_t confirmed;           // 1 = values confirmed for the current use
    uint8_t reserved[3];
};

static const uint32_t kStoreMagic = 0x31475253UL;   // "SRG1"
static const uint16_t kStoreVersion = 1;

struct Store {
    uint32_t magic;
    uint16_t version;
    uint16_t recordSize;
    Record records[kMaxGroups];
};

inline void storeClear(Store &s)
{
    memset(&s, 0, sizeof(s));
    s.magic = kStoreMagic;
    s.version = kStoreVersion;
    s.recordSize = (uint16_t)sizeof(Record);
}

inline bool recordValid(const Record &r)
{
    char tmp[kIdMaxLen + 1];
    if (r.id[kIdMaxLen] != '\0')
        return false;
    if (!r.id[0])
        return true;
    return normalizeId(r.id, tmp) && strcmp(tmp, r.id) == 0 &&
           (r.orientationDeci == kUnset ||
            (r.orientationDeci >= 0 && r.orientationDeci <= kGroupOrientMaxDeci)) &&
           (r.distanceMm == kUnset ||
            (r.distanceMm >= 0 && r.distanceMm <= kDistanceMaxMm)) &&
           r.confirmed <= 1;
}

// Accepts a persisted blob; anything unexpected yields an empty store, so a
// damaged NVS entry can never block the cluster.
inline bool storeLoad(Store &s, const void *blob, size_t len)
{
    storeClear(s);
    if (!blob || len != sizeof(Store))
        return false;
    Store tmp;
    memcpy(&tmp, blob, sizeof(tmp));
    if (tmp.magic != kStoreMagic || tmp.version != kStoreVersion ||
        tmp.recordSize != sizeof(Record))
        return false;
    for (size_t i = 0; i < kMaxGroups; ++i) {
        if (!recordValid(tmp.records[i]))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (tmp.records[i].id[0] &&
                strcmp(tmp.records[i].id, tmp.records[j].id) == 0)
                return false;
    }
    s = tmp;
    return true;
}

inline Record *storeFind(Store &s, const char *id)
{
    if (!id || !id[0])
        return nullptr;
    for (size_t i = 0; i < kMaxGroups; ++i)
        if (s.records[i].id[0] && strcmp(s.records[i].id, id) == 0)
            return &s.records[i];
    return nullptr;
}

inline size_t storeCount(const Store &s)
{
    size_t n = 0;
    for (size_t i = 0; i < kMaxGroups; ++i)
        if (s.records[i].id[0])
            ++n;
    return n;
}

// Creates or updates a record; the saved values count as confirmed.
// Returns nullptr if the id is invalid/empty or the store is full.
inline Record *storeUpsert(Store &s, const char *id, int32_t orientationDeci,
                           int32_t distanceMm)
{
    char norm[kIdMaxLen + 1];
    if (!normalizeId(id, norm) || !norm[0])
        return nullptr;
    if (!(orientationDeci == kUnset ||
          (orientationDeci >= 0 && orientationDeci <= kGroupOrientMaxDeci)) ||
        !(distanceMm == kUnset || (distanceMm >= 0 && distanceMm <= kDistanceMaxMm)))
        return nullptr;
    Record *r = storeFind(s, norm);
    if (!r) {
        for (size_t i = 0; i < kMaxGroups && !r; ++i)
            if (!s.records[i].id[0])
                r = &s.records[i];
        if (!r)
            return nullptr;
        memset(r, 0, sizeof(*r));
        memcpy(r->id, norm, sizeof(norm));
    }
    r->orientationDeci = orientationDeci;
    r->distanceMm = distanceMm;
    r->confirmed = 1;
    return r;
}

inline bool storeRemove(Store &s, const char *id)
{
    Record *r = storeFind(s, id);
    if (!r)
        return false;
    memset(r, 0, sizeof(*r));
    return true;
}

// ---- RAM activity tracking (never persisted on its own) --------------------

struct Activity {
    char id[kIdMaxLen + 1];
    uint32_t lastSeenMs;
    uint16_t nodeCount;          // nodes in the most recent count pass
    uint8_t used;
    uint8_t lapsed;              // was absent >= kLapseMs during this run
};

struct ActivityTable {
    Activity entries[kMaxActivity];
};

inline Activity *activityFind(ActivityTable &t, const char *id)
{
    if (!id || !id[0])
        return nullptr;
    for (size_t i = 0; i < kMaxActivity; ++i)
        if (t.entries[i].used && strcmp(t.entries[i].id, id) == 0)
            return &t.entries[i];
    return nullptr;
}

// Absence that started before boot is unknown; only absence observed for
// kLapseMs after boot counts. uptimeMs is the local uptime.
inline bool activityLapsedNow(const Activity *a, uint32_t nowMs, uint32_t uptimeMs)
{
    if (!a)
        return uptimeMs >= kLapseMs;
    if (a->lapsed)
        return true;
    return (uint32_t)(nowMs - a->lastSeenMs) >= kLapseMs;
}

// Records a mount report for group `id`.
inline void activityNote(ActivityTable &t, const char *id, uint32_t nowMs,
                         uint32_t uptimeMs)
{
    if (!id || !id[0])
        return;
    Activity *a = activityFind(t, id);
    if (a) {
        if ((uint32_t)(nowMs - a->lastSeenMs) >= kLapseMs)
            a->lapsed = 1;
        a->lastSeenMs = nowMs;
        return;
    }
    // New entry: free slot, else the longest-silent one.
    Activity *slot = nullptr;
    uint32_t oldestAge = 0;
    for (size_t i = 0; i < kMaxActivity; ++i) {
        Activity &e = t.entries[i];
        if (!e.used) {
            slot = &e;
            break;
        }
        const uint32_t age = nowMs - e.lastSeenMs;
        if (!slot || age > oldestAge) {
            slot = &e;
            oldestAge = age;
        }
    }
    if (!slot)
        return;
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    strncpy(slot->id, id, kIdMaxLen);
    slot->lastSeenMs = nowMs;
    slot->lapsed = uptimeMs >= kLapseMs ? 1 : 0;
}

// Marks all currently lapsed groups so the information survives until the
// next explicit save, even if the group becomes active again meanwhile.
inline void activityRefreshLapsed(ActivityTable &t, uint32_t nowMs)
{
    for (size_t i = 0; i < kMaxActivity; ++i) {
        Activity &e = t.entries[i];
        if (e.used && (uint32_t)(nowMs - e.lastSeenMs) >= kLapseMs)
            e.lapsed = 1;
    }
}

enum class State : uint8_t {
    New = 0,         // active, no stored values yet
    Confirmed = 1,   // active, stored values confirmed for this use
    Suggestion = 2,  // active, stored values only proposed
    Inactive = 3     // stored, no node currently reports this group
};

// `active`: at least one online node currently reports this group.
inline State groupState(const Record *r, const Activity *a, bool active,
                        uint32_t nowMs, uint32_t uptimeMs)
{
    if (!active)
        return State::Inactive;
    if (!r)
        return State::New;
    if (!r->confirmed || activityLapsedNow(a, nowMs, uptimeMs))
        return State::Suggestion;
    return State::Confirmed;
}

inline const char *stateName(State s)
{
    switch (s) {
    case State::New: return "new";
    case State::Confirmed: return "confirmed";
    case State::Suggestion: return "suggestion";
    default: return "inactive";
    }
}

// Before persisting: a lapsed group loses its confirmation; the group the
// user just saved (savedId) is confirmed and its lapse flag cleared.
inline void applyLapsesForSave(Store &s, ActivityTable &t, const char *savedId,
                               uint32_t nowMs, uint32_t uptimeMs)
{
    activityRefreshLapsed(t, nowMs);
    for (size_t i = 0; i < kMaxGroups; ++i) {
        Record &r = s.records[i];
        if (!r.id[0])
            continue;
        Activity *a = activityFind(t, r.id);
        if (savedId && strcmp(r.id, savedId) == 0) {
            r.confirmed = 1;
            if (a)
                a->lapsed = 0;
            continue;
        }
        if (activityLapsedNow(a, nowMs, uptimeMs))
            r.confirmed = 0;
    }
}

} // namespace ClusterGroups
