#include "cluster_camera_profile.h"

#include "camera_profile.h"
#include "camera_profile_store.h"
#include "cluster.h"
#include "cluster_camera.h"
#include "psram_alloc.h"

#include <Preferences.h>
#include <esp_random.h>

namespace {

using namespace ClusterCamera;

const char *const kNamespace = "sf_camref";
// Sensor types in one cluster (OV2640, OV3660, OV5640 and spare).
constexpr size_t kSlots = 4;
// A reference report older than this is not used for a new profile.
constexpr uint32_t kReferenceMaxAgeMs = 60000UL;

struct Slot {
    uint32_t pid;              // 0 = free
    char reference[36];        // integration id of the reference camera
    char profile[160];         // last distributed profile text (camera_profile.h)
};

Slot *slots = nullptr;
bool slotsLoaded = false;

// Coordinator's own camera: last result of a local profile application.
bool localResultKnown = false;
uint8_t localResult = 0;
uint32_t localResultVersion = 0;
uint32_t localResultMs = 0;

// NVS text per slot "s<i>": "<pid hex>|<reference id>|<profile text>".
void loadSlots()
{
    if (slotsLoaded)
        return;
    slotsLoaded = true;
    slots = static_cast<Slot *>(sfPsramCalloc(kSlots, sizeof(Slot)));
    if (!slots)
        return;
    Preferences prefs;
    if (!prefs.begin(kNamespace, true))
        return;
    for (size_t i = 0; i < kSlots; ++i) {
        char key[4];
        snprintf(key, sizeof(key), "s%u", (unsigned)i);
        const String text = prefs.getString(key, "");
        const int a = text.indexOf('|');
        const int b = a >= 0 ? text.indexOf('|', a + 1) : -1;
        if (a <= 0 || b < 0)
            continue;
        slots[i].pid = (uint32_t)strtoul(text.substring(0, a).c_str(), nullptr, 16);
        strncpy(slots[i].reference, text.substring(a + 1, b).c_str(), sizeof(slots[i].reference) - 1);
        strncpy(slots[i].profile, text.substring(b + 1).c_str(), sizeof(slots[i].profile) - 1);
    }
    prefs.end();
}

bool saveSlot(size_t i)
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, false))
        return false;
    char key[4];
    snprintf(key, sizeof(key), "s%u", (unsigned)i);
    bool ok;
    if (!slots[i].pid) {
        ok = !prefs.isKey(key) || prefs.remove(key);
    } else {
        char pid[8];
        snprintf(pid, sizeof(pid), "%lx", (unsigned long)slots[i].pid);
        const String text = String(pid) + "|" + slots[i].reference + "|" + slots[i].profile;
        ok = prefs.putString(key, text) == text.length();
    }
    prefs.end();
    return ok;
}

Slot *findSlot(uint32_t pid, bool create)
{
    loadSlots();
    if (!slots || !pid)
        return nullptr;
    for (size_t i = 0; i < kSlots; ++i)
        if (slots[i].pid == pid)
            return &slots[i];
    if (!create)
        return nullptr;
    for (size_t i = 0; i < kSlots; ++i)
        if (!slots[i].pid) {
            memset(&slots[i], 0, sizeof(Slot));
            slots[i].pid = pid;
            return &slots[i];
        }
    return nullptr;
}

size_t slotIndex(const Slot *slot) { return (size_t)(slot - slots); }

String jsonText(const char *text)
{
    String out;
    for (const char *p = text; *p; ++p)
        if (*p != '"' && *p != '\\' && (uint8_t)*p >= 0x20)
            out += *p;
    return out;
}

// Snapshot of local + online peers with their latest report.
struct NodeView {
    ClusterMountEntry entry;
    Report report;
    bool known;
    uint32_t ageMs;
};

template <typename F>
bool forEachNode(F &&visit)
{
    const size_t cap = clusterMountSnapshotCapacity();
    SfPsramBuffer buffer(cap * sizeof(ClusterMountEntry));
    if (!buffer)
        return false;
    ClusterMountEntry *snap = reinterpret_cast<ClusterMountEntry *>(buffer.data());
    const size_t count = clusterMountSnapshot(snap, cap);
    for (size_t i = 0; i < count; ++i) {
        NodeView v;
        v.entry = snap[i];
        v.ageMs = 0;
        reportClear(v.report);
        if (snap[i].local) {
            cameraFillClusterReport(v.report);
            v.known = true;
        } else {
            v.known = clusterCameraLatestReport(snap[i].integrationId, v.report, v.ageMs);
        }
        visit(v);
    }
    return true;
}

// Profile from the reference camera's live settings.
bool profileFromReport(const Report &r, CameraProfile::Profile &p, String &error)
{
    if (r.pid == kUnknown || r.fromSensor != 1) {
        error = "Referenzkamera ist aus oder meldet keine Treiberwerte – Kamera einschalten (z. B. Bildabstimmung öffnen).";
        return false;
    }
    uint16_t w = 0, h = 0;
    if (!framesizeDims(r.framesize, w, h) || !CameraProfile::resolutionSupported(w, h)) {
        error = "Auflösung der Referenzkamera wird für Profile nicht unterstützt.";
        return false;
    }
    const int32_t needed[] = {r.quality, r.aec, r.aeLevel, r.agc, r.awb, r.wbMode, r.awbGain,
                              r.brightness, r.contrast, r.saturation};
    for (int32_t v : needed)
        if (v == kUnknown) {
            error = "Referenzkamera meldet nicht alle Werte.";
            return false;
        }
    p.pid = (uint32_t)r.pid;
    p.width = w;
    p.height = h;
    p.quality = r.quality;
    p.autoExposure = r.aec;
    p.aeLevel = r.aeLevel;
    CameraTuning::Values &t = p.tuning;
    t.set = true;
    t.aecValue = (r.aec == 0 && r.aecValue != kUnknown) ? r.aecValue : 300;
    t.autoGain = r.agc;
    t.agcGain = (r.agc == 0 && r.agcGain != kUnknown) ? r.agcGain : 0;
    // OV3660/OV5640 report the raw ceiling register until it is set once via
    // the tuning page; only the index 0..6 can be transferred.
    t.gainCeiling = 0;
    if (r.agc == 1) {
        if (r.gainCeiling == kUnknown || r.gainCeiling < 0 || r.gainCeiling > CameraTuning::kGainCeilingMax) {
            error = "Verstärkungsobergrenze der Referenz ist nicht übertragbar – bitte in der Bildabstimmung "
                    "einmal setzen oder die Verstärkungsautomatik ausschalten.";
            return false;
        }
        t.gainCeiling = r.gainCeiling;
    }
    t.awb = r.awb;
    t.wbMode = r.wbMode;
    t.awbGain = r.awbGain;
    t.brightness = r.brightness;
    t.contrast = r.contrast;
    t.saturation = r.saturation;
    p.version = 1;   // placeholder for valid(); the caller assigns the real one
    if (!CameraProfile::valid(p)) {
        error = "Werte der Referenzkamera liegen außerhalb des übertragbaren Bereichs.";
        return false;
    }
    return true;
}

uint32_t newVersion()
{
    uint32_t v = esp_random() & 0x7FFFFFFFu;
    return v ? v : 1;
}

void noteLocalResult(uint8_t result, uint32_t version)
{
    localResultKnown = true;
    localResult = result;
    localResultVersion = version;
    localResultMs = millis();
}

String failJson(const char *key, const String &message)
{
    return String("{\"ok\":false,\"error\":\"") + key + "\",\"message\":\"" + jsonText(message.c_str()) + "\"}";
}

int setReference(const String &nodeId, String &json)
{
    Report r;
    reportClear(r);
    bool found = false;
    forEachNode([&](const NodeView &v) {
        if (nodeId == v.entry.integrationId && v.known) {
            r = v.report;
            found = true;
        }
    });
    if (!found) {
        json = failJson("unknown_node", "Gerät nicht online oder ohne Kameradaten.");
        return 404;
    }
    if (r.pid == kUnknown) {
        json = failJson("sensor_unknown", "Sensortyp des Geräts ist noch unbekannt (Kamera noch nie gestartet).");
        return 409;
    }
    Slot *slot = findSlot((uint32_t)r.pid, true);
    if (!slot) {
        json = failJson("no_slot", "Zu viele Sensortypen oder kein PSRAM.");
        return 507;
    }
    strncpy(slot->reference, nodeId.c_str(), sizeof(slot->reference) - 1);
    slot->reference[sizeof(slot->reference) - 1] = '\0';
    if (!saveSlot(slotIndex(slot))) {
        json = failJson("save_failed", "Referenz konnte nicht gespeichert werden.");
        return 500;
    }
    json = String("{\"ok\":true,\"pid\":\"") + String((unsigned long)r.pid, HEX) + "\",\"sensor\":\"" +
           sensorName(r.pid) + "\"}";
    return 200;
}

int distribute(uint32_t pid, uint8_t mode, String &json)
{
    Slot *slot = findSlot(pid, mode != CameraProfile::ModeClear);
    String profileText;
    uint32_t version = 0;
    if (mode != CameraProfile::ModeClear) {
        if (!slot || !slot->reference[0]) {
            json = failJson("no_reference", "Für diesen Sensortyp ist keine Referenzkamera gewählt.");
            return 409;
        }
        Report ref;
        reportClear(ref);
        bool found = false;
        uint32_t ageMs = 0;
        forEachNode([&](const NodeView &v) {
            if (strcmp(v.entry.integrationId, slot->reference) == 0 && v.known) {
                ref = v.report;
                ageMs = v.ageMs;
                found = true;
            }
        });
        if (!found || ageMs > kReferenceMaxAgeMs) {
            json = failJson("reference_offline", "Referenzkamera ist nicht online oder ihre Meldung ist älter als 60 s.");
            return 409;
        }
        if ((uint32_t)ref.pid != pid) {
            json = failJson("reference_sensor", "Die Referenzkamera hat inzwischen einen anderen Sensor.");
            return 409;
        }
        CameraProfile::Profile profile;
        String error;
        if (!profileFromReport(ref, profile, error)) {
            json = failJson("reference_values", error);
            return 409;
        }
        // Same settings as last time: keep the version, nodes already using it stay in sync.
        CameraProfile::Profile last;
        profile.version = CameraProfile::parse(slot->profile, last) && CameraProfile::sameSettings(last, profile)
            ? last.version : newVersion();
        char text[160];
        CameraProfile::format(profile, text, sizeof(text));
        strncpy(slot->profile, text, sizeof(slot->profile) - 1);
        if (!saveSlot(slotIndex(slot))) {
            json = failJson("save_failed", "Profil konnte am Coordinator nicht gespeichert werden.");
            return 500;
        }
        profileText = text;
        version = profile.version;
    } else if (slot && slot->profile[0]) {
        slot->profile[0] = '\0';
        saveSlot(slotIndex(slot));
    }

    size_t sent = 0, failed = 0, otherSensor = 0, unknownSensor = 0;
    String errors;
    forEachNode([&](const NodeView &v) {
        if (!v.known || v.report.pid == kUnknown) {
            ++unknownSensor;
            return;
        }
        if ((uint32_t)v.report.pid != pid) {
            ++otherSensor;
            return;
        }
        if (v.entry.local) {
            CameraProfile::Profile p;
            if (mode != CameraProfile::ModeClear)
                CameraProfile::parse(profileText.c_str(), p);
            noteLocalResult(cameraProfileApplyJob(p, mode), version);
            ++sent;
            return;
        }
        String error;
        if (clusterRequestNodeCameraProfile(String(v.entry.integrationId), profileText, mode, error)) {
            ++sent;
        } else {
            ++failed;
            if (errors.length() < 300)
                errors += String(v.entry.hostname) + ": " + error + " ";
        }
        // Many nodes: keep ESP-NOW time exchanges answered between sends.
        clusterServiceTimeCritical();
    });

    char versionText[12];
    snprintf(versionText, sizeof(versionText), "%lx", (unsigned long)version);
    json = String("{\"ok\":true,\"pid\":\"") + String((unsigned long)pid, HEX) + "\",\"version\":" +
           (version ? "\"" + String(versionText) + "\"" : String("null")) +
           ",\"sent\":" + String((unsigned long)sent) + ",\"failed\":" + String((unsigned long)failed) +
           ",\"other_sensor\":" + String((unsigned long)otherSensor) +
           ",\"unknown_sensor\":" + String((unsigned long)unknownSensor) +
           ",\"errors\":\"" + jsonText(errors.c_str()) + "\"}";
    return sent || !failed ? 202 : 409;
}

void appendProfileJson(String &json, const char *text)
{
    CameraProfile::Profile p;
    if (!CameraProfile::parse(text, p)) {
        json += "null";
        return;
    }
    char version[12];
    snprintf(version, sizeof(version), "%lx", (unsigned long)p.version);
    const CameraTuning::Values &t = p.tuning;
    json += "{\"version\":\"" + String(version) + "\",\"text\":\"" + jsonText(text) + "\"";
    json += ",\"resolution\":\"" + String(p.width) + "x" + String(p.height) + "\"";
    json += ",\"quality\":" + String(p.quality) + ",\"aec\":" + String(p.autoExposure) +
            ",\"ae_level\":" + String(p.aeLevel) + ",\"aec_value\":" + String(t.aecValue) +
            ",\"agc\":" + String(t.autoGain) + ",\"agc_gain\":" + String(t.agcGain) +
            ",\"gainceiling\":" + String(t.gainCeiling) + ",\"awb\":" + String(t.awb) +
            ",\"wb_mode\":" + String(t.wbMode) + ",\"awb_gain\":" + String(t.awbGain) +
            ",\"brightness\":" + String(t.brightness) + ",\"contrast\":" + String(t.contrast) +
            ",\"saturation\":" + String(t.saturation) + "}";
}

} // namespace

void clusterCameraAppendLocalProfileResult(String &json, uint32_t nowMs)
{
    if (!localResultKnown)
        return;
    char version[12];
    snprintf(version, sizeof(version), "%lx", (unsigned long)localResultVersion);
    json += ",\"profile_result\":\"" + String(CameraProfile::resultName(localResult)) + "\"";
    json += ",\"profile_result_version\":\"" + String(version) + "\"";
    json += ",\"profile_result_age_s\":" + String((unsigned long)((nowMs - localResultMs) / 1000UL));
}

String clusterCameraProfileJson()
{
    loadSlots();
    if (!slots)
        return "{\"ok\":false,\"error\":\"psram_unavailable\",\"sensors\":[]}";
    // Sensor types: stored ones plus every type reported right now.
    uint32_t pids[kSlots * 4] = {};
    size_t count = 0;
    auto add = [&](uint32_t pid) {
        if (!pid)
            return;
        for (size_t i = 0; i < count; ++i)
            if (pids[i] == pid)
                return;
        if (count < sizeof(pids) / sizeof(pids[0]))
            pids[count++] = pid;
    };
    for (size_t i = 0; i < kSlots; ++i)
        add(slots[i].pid);
    forEachNode([&](const NodeView &v) {
        if (v.known && v.report.pid != kUnknown)
            add((uint32_t)v.report.pid);
    });

    String json = "{\"ok\":true,\"coordinator\":";
    json += clusterLocalIsCoordinator() ? "true" : "false";
    json += ",\"sensors\":[";
    for (size_t i = 0; i < count; ++i) {
        const uint32_t pid = pids[i];
        const Slot *slot = findSlot(pid, false);
        if (i)
            json += ',';
        json += "{\"pid\":\"" + String((unsigned long)pid, HEX) + "\",\"sensor\":\"" + sensorName((int32_t)pid) + "\"";
        json += ",\"reference\":";
        json += slot && slot->reference[0] ? "\"" + jsonText(slot->reference) + "\"" : String("null");
        json += ",\"profile\":";
        if (slot && slot->profile[0])
            appendProfileJson(json, slot->profile);
        else
            json += "null";
        json += '}';
    }
    json += "]}";
    return json;
}

int clusterCameraProfileRequestFromArgs(WebServer &srv, String &json)
{
    if (!clusterLocalIsCoordinator()) {
        json = failJson("not_coordinator", "Nur am aktiven Coordinator.");
        return 403;
    }
    const String action = srv.arg("action");
    if (action == "reference") {
        const String nodeId = srv.arg("node_id");
        if (!nodeId.length()) {
            json = failJson("node_id_missing", "node_id fehlt.");
            return 400;
        }
        return setReference(nodeId, json);
    }
    const String pidText = srv.arg("pid");
    char *end = nullptr;
    const unsigned long pid = strtoul(pidText.c_str(), &end, 16);
    if (!pidText.length() || !end || *end || !pid || pid > 0xFFFF) {
        json = failJson("invalid_pid", "pid (Sensor-ID hex, z. B. 5640) fehlt oder ist ungültig.");
        return 400;
    }
    if (action == "clear")
        return distribute((uint32_t)pid, CameraProfile::ModeClear, json);
    if (action == "distribute") {
        const String mode = srv.arg("mode");
        if (mode != "session" && mode != "persistent") {
            json = failJson("invalid_mode", "mode muss session oder persistent sein.");
            return 400;
        }
        return distribute((uint32_t)pid, mode == "session" ? CameraProfile::ModeSession : CameraProfile::ModePersistent,
                           json);
    }
    json = failJson("invalid_action", "action muss reference, distribute oder clear sein.");
    return 400;
}
