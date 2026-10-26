#include "cluster_firmware_rollout.h"

#include "cluster.h"
#include "firmware_cluster_update.h"
#include "firmware_manifest_probe.h"
#include "firmware_package_format.h"
#include "sensorforge_version.h"

#include <Preferences.h>
#include <algorithm>
#include <vector>

namespace {

using namespace FirmwareClusterUpdate;

enum class State : uint8_t { Pending, Sent, Accepted, Selected, Done, Skipped, Failed, Stopped };

const char *stateName(State s)
{
    switch (s) {
    case State::Pending: return "pending";
    case State::Sent: return "sent";
    case State::Accepted: return "updating";
    case State::Selected: return "restarting";
    case State::Done: return "done";
    case State::Skipped: return "skipped";
    case State::Failed: return "failed";
    case State::Stopped: return "stopped";
    }
    return "unknown";
}

struct Entry {
    String id;
    String hostname;
    String startRelease;
    uint32_t startBoot = 0;
    State state = State::Pending;
    uint8_t code = 0;
    bool codeKnown = false;
    String version;   // release the node selected (from its answer)
    String text;
    String finalRelease;
    uint32_t seq = 0;
    uint8_t sendAttempts = 0;
    uint32_t stateMs = 0;
};

// Answer to the job; a download plus verification on a weak link takes a while.
constexpr uint32_t kAckTimeoutMs = 8000;
constexpr uint8_t kSendAttempts = 2;
constexpr uint32_t kUpdateTimeoutMs = 5UL * 60UL * 1000UL;
// A normal restart takes 12-30 s; a node not back after 2 min needs a power cycle.
constexpr uint32_t kRebootTimeoutMs = 2UL * 60UL * 1000UL;

std::vector<Entry> entries;
String rolloutUrl;
bool rolloutAllowOlder = false;
bool rolloutRunning = false;
String rolloutMessage;
uint32_t lastServiceMs = 0;

void setState(Entry &e, State s)
{
    e.state = s;
    e.stateMs = millis();
}

void stopRollout(const String &message)
{
    rolloutRunning = false;
    rolloutMessage = message;
    for (Entry &e : entries)
        if (e.state == State::Pending)
            setState(e, State::Stopped);
}

String storedUrl()
{
    Preferences p;
    String url;
    if (p.begin("sf_fwroll", true)) {
        url = p.getString("url", "");
        p.end();
    }
    return url;
}

void storeUrl(const String &url)
{
    Preferences p;
    if (p.begin("sf_fwroll", false)) {
        if (p.getString("url", "") != url)
            p.putString("url", url);
        p.end();
    }
}

bool currentPeer(const String &id, ClusterFirmwareEntry &out)
{
    return clusterFirmwarePeer(id.c_str(), out);
}

// Node is back after the restart: new boot, online. Done when the release
// changed (to the one it selected, if that answer arrived).
bool checkReturned(Entry &e)
{
    ClusterFirmwareEntry peer;
    if (!currentPeer(e.id, peer) || !peer.online || peer.bootNonce == e.startBoot)
        return false;
    e.finalRelease = peer.release;
    if (e.version.length() ? e.finalRelease == e.version : e.finalRelease != e.startRelease) {
        setState(e, State::Done);
        e.text = "läuft mit " + e.finalRelease;
    } else {
        setState(e, State::Failed);
        e.text = "nach Neustart weiter " + e.finalRelease + " (Rückfall auf die alte Firmware?)";
    }
    return true;
}

void serviceEntry(Entry &e)
{
    const uint32_t age = millis() - e.stateMs;
    switch (e.state) {
    case State::Pending: {
        String error;
        if (!clusterRequestNodeFirmwareUpdate(e.id, rolloutUrl, rolloutAllowOlder, e.seq, error)) {
            setState(e, State::Failed);
            e.text = error;
            return;
        }
        ++e.sendAttempts;
        setState(e, State::Sent);
        return;
    }
    case State::Sent:
        if (checkReturned(e))
            return;
        if (age < kAckTimeoutMs)
            return;
        if (e.sendAttempts < kSendAttempts) {
            setState(e, State::Pending);
            return;
        }
        setState(e, State::Failed);
        e.text = "keine Antwort auf den Update-Auftrag";
        return;
    case State::Accepted:
        if (checkReturned(e))
            return;
        if (age > kUpdateTimeoutMs) {
            setState(e, State::Failed);
            e.text = "kein Ergebnis innerhalb von 5 min";
        }
        return;
    case State::Selected:
        if (checkReturned(e))
            return;
        if (age > kRebootTimeoutMs) {
            setState(e, State::Failed);
            e.text = "nach dem Neustart nicht zurückgekehrt – Gerät kurz stromlos machen";
        }
        return;
    default:
        return;
    }
}

String jsonEscape(const String &in)
{
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        const char c = in[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c < 32) out += ' ';
        else out += c;
    }
    return out;
}

int result(String &json, int code, bool ok, const String &message)
{
    json = String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" + jsonEscape(message) + "\"}";
    return code;
}

bool argUrl(WebServer &srv, String &url, String &json, int &code)
{
    url = srv.arg("url");
    url.trim();
    const char *error = nullptr;
    if (!FirmwarePackage::urlAllowed(url.c_str(), error)) {
        code = result(json, 400, false, String("Manifest-URL abgelehnt: ") + error);
        return false;
    }
    return true;
}

} // namespace

void clusterFirmwareRolloutNote(const char *nodeId, uint32_t seq, uint8_t code, const String &version,
                                const String &text)
{
    for (Entry &e : entries) {
        if (e.id != nodeId || e.seq != seq)
            continue;
        // A resent job after a lost "accepted": the node is already on it.
        if (code == InProgress && e.state == State::Sent && e.sendAttempts > 1)
            code = Accepted;
        e.code = code;
        e.codeKnown = true;
        if (version.length())
            e.version = version;
        if (text.length())
            e.text = text;
        if (code == Accepted && e.state == State::Sent) {
            setState(e, State::Accepted);
        } else if (code == Selected && (e.state == State::Sent || e.state == State::Accepted)) {
            setState(e, State::Selected);
        } else if (code == NoUpdate && e.state != State::Done) {
            setState(e, State::Skipped);
            if (!text.length())
                e.text = "keine neuere Version für dieses Board";
        } else if (codeIsFailure(code) && e.state != State::Done && e.state != State::Failed) {
            setState(e, State::Failed);
            if (!text.length())
                e.text = codeName(code);
        }
        return;
    }
}

void clusterFirmwareRolloutService()
{
    if ((uint32_t)(millis() - lastServiceMs) < 500UL)
        return;
    lastServiceMs = millis();
    // Finished entries still learn that their node came back.
    for (Entry &e : entries)
        if (!rolloutRunning && (e.state == State::Accepted || e.state == State::Selected || e.state == State::Sent))
            checkReturned(e);
    if (!rolloutRunning)
        return;
    if (!clusterLocalIsCoordinator()) {
        stopRollout("Nicht mehr Coordinator");
        return;
    }
    // One node at a time; a failed node is marked and the rollout continues.
    size_t failedCount = 0;
    for (Entry &e : entries) {
        if (e.state == State::Failed) {
            ++failedCount;
            continue;
        }
        if (e.state == State::Done || e.state == State::Skipped || e.state == State::Stopped)
            continue;
        serviceEntry(e);
        return;
    }
    rolloutRunning = false;
    rolloutMessage = failedCount
        ? String("Fertig, ") + String((unsigned)failedCount) +
              " Gerät(e) rot markiert (kurz stromlos machen und den Rollout erneut starten – "
              "aktuelle Geräte werden übersprungen). Den Coordinator zuletzt mit „Coordinator aktualisieren“."
        : String("Alle Nodes bearbeitet. Den Coordinator zuletzt mit „Coordinator aktualisieren“.");
}

String clusterFirmwareRolloutJson()
{
    const uint32_t now = millis();
    String json = "{\"ok\":true,\"coordinator\":";
    json += clusterLocalIsCoordinator() ? "true" : "false";
    json += ",\"url\":\"" + jsonEscape(rolloutUrl.length() ? rolloutUrl : storedUrl()) + "\"";
    json += ",\"allow_older\":" + String(rolloutAllowOlder ? "true" : "false");
    json += ",\"running\":" + String(rolloutRunning ? "true" : "false");
    json += ",\"local_busy\":" + String(FirmwareClusterUpdate::busy() ? "true" : "false");
    {
        uint8_t lc = 0;
        String lv, lt;
        uint32_t lage = 0;
        if (FirmwareClusterUpdate::lastLocalResult(lc, lv, lt, lage))
            json += ",\"local_result\":{\"code_name\":\"" + String(codeName(lc)) + "\",\"version\":\"" +
                    jsonEscape(lv) + "\",\"text\":\"" + jsonEscape(lt) + "\",\"age_s\":" + String(lage / 1000UL) + "}";
    }
    json += ",\"message\":\"" + jsonEscape(rolloutMessage) + "\",\"devices\":[";
    const size_t cap = clusterFirmwareSnapshotCapacity();
    std::vector<ClusterFirmwareEntry> snap(cap);
    const size_t n = clusterFirmwareSnapshot(snap.data(), cap);
    for (size_t i = 0; i < n; ++i) {
        const ClusterFirmwareEntry &p = snap[i];
        if (i) json += ",";
        json += "{\"id\":\"" + String(p.integrationId) + "\",\"hostname\":\"" + jsonEscape(p.hostname) +
                "\",\"release\":\"" + jsonEscape(p.release) + "\",\"local\":" + (p.local ? "true" : "false") +
                ",\"online\":" + (p.online ? "true" : "false");
        for (const Entry &e : entries) {
            if (e.id != p.integrationId)
                continue;
            json += ",\"state\":\"" + String(stateName(e.state)) + "\",\"code\":" +
                    (e.codeKnown ? String(e.code) : String("null")) + ",\"code_name\":\"" +
                    (e.codeKnown ? String(codeName(e.code)) : String()) + "\",\"version\":\"" +
                    jsonEscape(e.version) + "\",\"text\":\"" + jsonEscape(e.text) + "\",\"state_age_s\":" +
                    String((now - e.stateMs) / 1000UL);
            break;
        }
        json += "}";
    }
    return json + "]}";
}

int clusterFirmwareRolloutRequestFromArgs(WebServer &srv, String &json)
{
    if (!clusterLocalIsCoordinator())
        return result(json, 403, false, "Nur am Coordinator.");
    const String action = srv.arg("action");
    int code = 200;
    if (action == "check") {
        String url, list, detail;
        if (!argUrl(srv, url, json, code))
            return code;
        const bool ok = firmwareManifestList(url, list, detail);
        // A readable manifest is remembered as the URL of this page.
        if (ok) {
            storeUrl(url);
            if (!rolloutRunning)
                rolloutUrl = url;
            detail += " · URL gespeichert";
        }
        json = String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" + jsonEscape(detail) +
               "\",\"releases\":" + list + ",\"installed\":\"" SENSORFORGE_RELEASE_TAG "\"}";
        return ok ? 200 : 502;
    }
    if (action == "start") {
        if (rolloutRunning)
            return result(json, 409, false, "Rollout läuft bereits.");
        String url;
        if (!argUrl(srv, url, json, code))
            return code;
        rolloutUrl = url;
        rolloutAllowOlder = srv.arg("allow_older") == "1";
        storeUrl(url);
        entries.clear();
        const size_t cap = clusterFirmwareSnapshotCapacity();
        std::vector<ClusterFirmwareEntry> snap(cap);
        const size_t n = clusterFirmwareSnapshot(snap.data(), cap);
        for (size_t i = 0; i < n; ++i) {
            if (snap[i].local || !snap[i].online)
                continue;
            Entry e;
            e.id = snap[i].integrationId;
            e.hostname = snap[i].hostname;
            e.startRelease = snap[i].release;
            e.startBoot = snap[i].bootNonce;
            e.stateMs = millis();
            entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry &a, const Entry &b) { return a.hostname < b.hostname; });
        if (entries.empty())
            return result(json, 409, false, "Keine Nodes online.");
        rolloutRunning = true;
        rolloutMessage = "Rollout gestartet: " + String((unsigned)entries.size()) + " Nodes, nacheinander.";
        return result(json, 200, true, rolloutMessage);
    }
    if (action == "stop") {
        if (rolloutRunning)
            stopRollout("Angehalten. Ein gerade laufendes Update läuft zu Ende.");
        return result(json, 200, true, rolloutMessage);
    }
    if (action == "clear") {
        if (rolloutRunning)
            return result(json, 409, false, "Rollout läuft.");
        entries.clear();
        rolloutMessage = "";
        return result(json, 200, true, "Liste geleert.");
    }
    if (action == "local") {
        if (rolloutRunning)
            return result(json, 409, false, "Erst den Rollout der Nodes abschließen.");
        String url;
        if (!argUrl(srv, url, json, code))
            return code;
        storeUrl(url);
        const uint8_t c = FirmwareClusterUpdate::requestLocal(url, srv.arg("allow_older") == "1");
        if (c != Accepted)
            return result(json, 409, false, String("Abgelehnt: ") + codeName(c));
        return result(json, 200, true,
                      "Coordinator lädt die Firmware und startet bei Erfolg neu (danach Seite neu laden).");
    }
    return result(json, 400, false, "action muss check, start, stop, clear oder local sein");
}
