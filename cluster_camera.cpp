#include "cluster_camera.h"

#include "cluster.h"
#include "psram_alloc.h"

#include <string.h>

namespace {

using namespace ClusterCamera;

// One entry per reporting device (100-node cluster + rebooted/left ones).
static const size_t kMaxEntries = 128;

struct Entry {
    char integrationId[36];
    uint32_t bootNonce;
    uint32_t reportSeq;
    uint32_t receivedMs;
    Report report;
};

// Lazily allocated in PSRAM, never internal RAM (see psram_alloc.h).
static Entry *g_entries = nullptr;
static bool g_allocFailed = false;

static Entry *entries()
{
    if (g_entries || g_allocFailed)
        return g_entries;
    g_entries = static_cast<Entry *>(sfPsramCalloc(kMaxEntries, sizeof(Entry)));
    if (!g_entries) {
        g_allocFailed = true;
        Serial.println("[CAMERA] no PSRAM for cluster camera table, overview disabled");
    }
    return g_entries;
}

static const Entry *findEntry(const char *integrationId)
{
    Entry *e = entries();
    if (!e)
        return nullptr;
    for (size_t i = 0; i < kMaxEntries; ++i)
        if (e[i].integrationId[0] && strcmp(e[i].integrationId, integrationId) == 0)
            return &e[i];
    return nullptr;
}

static void appendNum(String &json, const char *key, int32_t v)
{
    json += ",\"";
    json += key;
    json += "\":";
    json += v == kUnknown ? String("null") : String((long)v);
}

static void appendDims(String &json, const char *key, int32_t fs)
{
    uint16_t w = 0, h = 0;
    json += ",\"";
    json += key;
    json += "\":";
    if (framesizeDims(fs, w, h))
        json += "\"" + String(w) + "x" + String(h) + "\"";
    else
        json += "null";
}

static const char *sourceName(Source s)
{
    switch (s) {
    case Source::Cluster: return "cluster";
    case Source::Node: return "node";
    default: return "local";
    }
}

static void appendReport(String &json, const Report &r)
{
    static const char *const camStates[] = {"off", "on", "standby"};
    json += ",\"cam_state\":";
    json += r.camState >= 0 && r.camState <= 2 ? "\"" + String(camStates[r.camState]) + "\"" : String("null");
    appendNum(json, "drone", r.drone);
    json += ",\"sensor\":\"" + String(sensorName(r.pid)) + "\"";
    appendNum(json, "pid", r.pid);
    appendNum(json, "xclk_mhz", r.xclkMhz);
    json += ",\"from_sensor\":";
    json += r.fromSensor == 1 ? "true" : "false";
    appendDims(json, "max_resolution", r.maxFramesize);
    appendNum(json, "cap_mask", r.capMask);
    appendDims(json, "resolution", r.framesize);
    appendNum(json, "quality", r.quality);
    appendNum(json, "brightness", r.brightness);
    appendNum(json, "contrast", r.contrast);
    appendNum(json, "saturation", r.saturation);
    appendNum(json, "sharpness", r.sharpness);
    appendNum(json, "denoise", r.denoise);
    appendNum(json, "aec", r.aec);
    appendNum(json, "aec2", r.aec2);
    appendNum(json, "aec_value", r.aecValue);
    appendNum(json, "ae_level", r.aeLevel);
    appendNum(json, "agc", r.agc);
    appendNum(json, "agc_gain", r.agcGain);
    appendNum(json, "gainceiling", r.gainCeiling);
    appendNum(json, "awb", r.awb);
    appendNum(json, "awb_gain", r.awbGain);
    appendNum(json, "wb_mode", r.wbMode);
    appendNum(json, "rotation", r.rotation);
    appendNum(json, "hmirror", r.hmirror);
    appendNum(json, "vflip", r.vflip);
    appendNum(json, "crop_zoom_x10", r.cropZoomX10);
    appendNum(json, "crop_x", r.cropX);
    appendNum(json, "crop_y", r.cropY);
    static const char *const groupKeys[] = {
        "resolution", "quality", "exposure", "gain", "white_balance", "image",
        "rotation", "mirror", "crop"};
    json += ",\"sources\":{";
    for (unsigned g = 0; g < static_cast<unsigned>(Group::Count); ++g) {
        if (g)
            json += ',';
        json += "\"" + String(groupKeys[g]) + "\":\"" +
            sourceName(groupSource((uint32_t)r.sourceMask, static_cast<Group>(g))) + "\"";
    }
    json += '}';
}

static String safeText(const char *text)
{
    String out;
    for (const char *p = text; *p; ++p)
        if (*p != '"' && *p != '\\' && (uint8_t)*p >= 0x20)
            out += *p;
    return out;
}

} // namespace

void clusterCameraStoreReport(const char *integrationId, uint32_t bootNonce,
                              uint32_t reportSeq, const Report &report)
{
    Entry *e = entries();
    if (!e || !integrationId || !integrationId[0])
        return;
    const uint32_t now = millis();
    Entry *slot = nullptr;
    Entry *oldest = nullptr;
    for (size_t i = 0; i < kMaxEntries; ++i) {
        if (e[i].integrationId[0] && strcmp(e[i].integrationId, integrationId) == 0) {
            slot = &e[i];
            break;
        }
        if (!e[i].integrationId[0]) {
            if (!oldest || oldest->integrationId[0])
                oldest = &e[i];
        } else if (!oldest || (oldest->integrationId[0] &&
                               (uint32_t)(now - e[i].receivedMs) > (uint32_t)(now - oldest->receivedMs))) {
            oldest = &e[i];
        }
    }
    if (slot) {
        // Same boot: accept only newer reports (replayed packets are ignored).
        if (slot->bootNonce == bootNonce && reportSeq <= slot->reportSeq)
            return;
    } else {
        slot = oldest;
        if (!slot)
            return;
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->integrationId, integrationId, sizeof(slot->integrationId) - 1);
    }
    slot->bootNonce = bootNonce;
    slot->reportSeq = reportSeq;
    slot->receivedMs = now;
    slot->report = report;
}

String clusterCameraJson()
{
    const size_t cap = clusterMountSnapshotCapacity();
    SfPsramBuffer snapBuffer(cap * sizeof(ClusterMountEntry));
    if (!snapBuffer || !entries())
        return "{\"ok\":false,\"error\":\"psram_unavailable\",\"nodes\":[]}";
    ClusterMountEntry *snap = reinterpret_cast<ClusterMountEntry *>(snapBuffer.data());
    const size_t count = clusterMountSnapshot(snap, cap);
    const uint32_t now = millis();

    String json;
    json.reserve(256 + count * 900);
    json += "{\"ok\":true,\"nodes\":[";
    for (size_t i = 0; i < count; ++i) {
        const ClusterMountEntry &n = snap[i];
        if (i)
            json += ',';
        json += "{\"hostname\":\"" + safeText(n.hostname) + "\"";
        json += ",\"integration_id\":\"" + safeText(n.integrationId) + "\"";
        json += ",\"local\":";
        json += n.local ? "true" : "false";
        Report r;
        bool known = false;
        uint32_t ageS = 0;
        if (n.local) {
            cameraFillClusterReport(r);
            known = true;
        } else if (const Entry *e = findEntry(n.integrationId)) {
            r = e->report;
            known = true;
            ageS = (now - e->receivedMs) / 1000UL;
        }
        json += ",\"report_known\":";
        json += known ? "true" : "false";
        if (known) {
            json += ",\"age_s\":" + String((unsigned long)ageS);
            appendReport(json, r);
        }
        json += '}';
    }
    json += "]}";
    return json;
}
