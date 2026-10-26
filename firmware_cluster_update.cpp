#include "firmware_cluster_update.h"

#include "cluster.h"
#include "firmware_auto_runner.h"
#include "firmware_package_format.h"

namespace FirmwareClusterUpdate {
namespace {

struct Pending {
    bool active = false;
    bool local = false;
    bool allowOlder = false;
    String url;
    uint32_t coordBoot = 0, epoch = 0, seq = 0;
    uint32_t acceptedMs = 0;
};
Pending pending;
bool running = false;
bool localResultKnown = false;
uint8_t localResultCode = 0;
String localResultVersion, localResultText;
uint32_t localResultMs = 0;

// Give the ACK a moment on the air before the loop blocks for the download.
constexpr uint32_t kStartDelayMs = 300;

uint8_t take(const String &url, bool allowOlder, bool local, uint32_t coordBoot, uint32_t epoch, uint32_t seq)
{
    const char *error = nullptr;
    if (!FirmwarePackage::urlAllowed(url.c_str(), error))
        return Invalid;
    if (pending.active || running)
        return InProgress;
    pending.active = true;
    pending.local = local;
    pending.allowOlder = allowOlder;
    pending.url = url;
    pending.coordBoot = coordBoot;
    pending.epoch = epoch;
    pending.seq = seq;
    pending.acceptedMs = millis();
    return Accepted;
}

void report(const Pending &job, uint8_t code, const String &version, const String &text)
{
    Serial.printf("[OTA-CLUSTER] %s %s %s\n", codeName(code), version.c_str(), text.c_str());
    if (job.local) {
        localResultKnown = true;
        localResultCode = code;
        localResultVersion = version;
        localResultText = text;
        localResultMs = millis();
    }
    if (!job.local)
        clusterSendFirmwareUpdateResult(job.coordBoot, job.epoch, job.seq, code, version, text);
}

} // namespace

const char *codeName(uint8_t code)
{
    switch (code) {
    case Accepted: return "accepted";
    case Selected: return "selected";
    case NoUpdate: return "no_update";
    case Busy: return "busy";
    case IsCoordinator: return "is_coordinator";
    case Invalid: return "invalid";
    case CheckFailed: return "check_failed";
    case StageFailed: return "stage_failed";
    case ActivationFailed: return "activation_failed";
    case InProgress: return "in_progress";
    default: return "unknown";
    }
}

uint8_t requestFromCoordinator(const String &manifestUrl, bool allowOlder, uint32_t coordBoot,
                               uint32_t epoch, uint32_t seq)
{
    return take(manifestUrl, allowOlder, false, coordBoot, epoch, seq);
}

uint8_t requestLocal(const String &manifestUrl, bool allowOlder)
{
    return take(manifestUrl, allowOlder, true, 0, 0, 0);
}

bool busy()
{
    return pending.active || running;
}

bool lastLocalResult(uint8_t &code, String &version, String &text, uint32_t &ageMs)
{
    if (!localResultKnown)
        return false;
    code = localResultCode;
    version = localResultVersion;
    text = localResultText;
    ageMs = millis() - localResultMs;
    return true;
}

void service()
{
    if (!pending.active || running || (uint32_t)(millis() - pending.acceptedMs) < kStartDelayMs)
        return;
    const Pending job = pending;
    pending = Pending{};
    running = true;

    String version, detail;
    const FirmwareAutoActivateStatus st =
        firmwareClusterStageAndSelectOnce(job.url, job.allowOlder, version, detail);
    uint8_t code;
    switch (st) {
    case FirmwareAutoActivateStatus::BootSelected: code = Selected; break;
    case FirmwareAutoActivateStatus::NoUpdate: code = NoUpdate; break;
    case FirmwareAutoActivateStatus::Busy: code = Busy; break;
    case FirmwareAutoActivateStatus::CheckFailed: code = CheckFailed; break;
    case FirmwareAutoActivateStatus::StageFailed: code = StageFailed; break;
    default: code = ActivationFailed; break;
    }
    report(job, code, version, detail);
    if (code != Selected) {
        running = false;
        return;
    }
    // UDP has no ACK: send the result once more, then boot the new image. The
    // coordinator also recognises the update by the new boot and release.
    delay(200);
    report(job, code, version, detail);
    Serial.println("[OTA-CLUSTER] Signiertes Image gewaehlt, Neustart. Boot und 60-s-Pruefung stehen aus.");
    Serial.flush();
    delay(300);
    ESP.restart();
}

} // namespace FirmwareClusterUpdate
