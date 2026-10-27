#include "cluster_shot_archive.h"
#include "board_config.h"
#include "cluster.h"
#include "cluster_jobs.h"
#include "logger.h"
#include "psram_alloc.h"
#include "storage_guard.h"

#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <string.h>
#include <time.h>
#include <vector>

extern bool sdReady;

namespace {

const char *kDir = "/cluster_shots";
const size_t kMaxItems = 128;
const size_t kMaxPhotoBytes = 4UL * 1024UL * 1024UL;
// After the shot: node results arrive with retries (up to 10 x 2 s).
const uint32_t kResultWaitMs = 60000UL;
// The capture jobs of the shot were queued at most this long before the request.
const uint32_t kJobMatchWindowMs = 120000UL;

enum class ArchiveState : uint8_t { Idle, Waiting, Collecting, Done, Failed };

struct Item {
    char nodeId[ClusterJobs::kNodeIdLength];
    uint64_t jobId;
    bool haveJob;
    bool finished;  // job reached a final state
    bool ready;     // photo available on the node
    bool stored;    // written to the archive
    char uuid[37];
    int64_t dispatchUs;
    char url[200];  // contains the media token: never logged or reported
    char host[64];
    char entryName[112];
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
};

portMUX_TYPE archiveMux = portMUX_INITIALIZER_UNLOCKED;
Item *items = nullptr;
size_t itemCount = 0;
volatile ArchiveState state = ArchiveState::Idle;
ArchiveState loggedState = ArchiveState::Idle;
int64_t targetMs = 0;
uint32_t waitUntilMs = 0;
uint32_t lastPollMs = 0;
volatile uint32_t doneCount = 0;
volatile uint32_t failCount = 0;
volatile uint32_t bytesWritten = 0;
char fileName[64] = "";
char message[192] = "";

void setMessage(const char *text)
{
    portENTER_CRITICAL(&archiveMux);
    strlcpy(message, text, sizeof(message));
    portEXIT_CRITICAL(&archiveMux);
}

const char *stateName(ArchiveState s)
{
    switch (s) {
    case ArchiveState::Waiting: return "waiting";
    case ArchiveState::Collecting: return "collecting";
    case ArchiveState::Done: return "done";
    case ArchiveState::Failed: return "failed";
    default: return "idle";
    }
}

// "2026-10-27T00:03:22.443Z" (sep ':') or file-name safe with sep '-'.
void formatUtc(int64_t utcMs, char *out, size_t length, char sep)
{
    const time_t sec = (time_t)(utcMs / 1000);
    struct tm t = {};
    gmtime_r(&sec, &t);
    snprintf(out, length, "%04d-%02d-%02dT%02d%c%02d%c%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, sep, t.tm_min, sep, t.tm_sec, (int)(utcMs % 1000));
}

uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? (0xEDB88320UL ^ (crc >> 1)) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFUL;
}

void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

struct ZipEntry {
    const char *name;
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
};

// Local file header (stored, UTF-8 name) followed by the name.
bool writeLocalHeader(File &f, const ZipEntry &e, uint16_t dosTime, uint16_t dosDate)
{
    uint8_t h[30] = {};
    const size_t nameLength = strlen(e.name);
    put32(h, 0x04034b50UL); put16(h + 4, 20); put16(h + 6, 0x0800); put16(h + 8, 0);
    put16(h + 10, dosTime); put16(h + 12, dosDate); put32(h + 14, e.crc);
    put32(h + 18, e.size); put32(h + 22, e.size); put16(h + 26, nameLength); put16(h + 28, 0);
    return f.write(h, sizeof(h)) == sizeof(h) &&
           f.write(reinterpret_cast<const uint8_t *>(e.name), nameLength) == nameLength;
}

bool writeCentralHeader(File &f, const ZipEntry &e, uint16_t dosTime, uint16_t dosDate)
{
    uint8_t c[46] = {};
    const size_t nameLength = strlen(e.name);
    put32(c, 0x02014b50UL); put16(c + 4, 20); put16(c + 6, 20); put16(c + 8, 0x0800); put16(c + 10, 0);
    put16(c + 12, dosTime); put16(c + 14, dosDate); put32(c + 16, e.crc); put32(c + 20, e.size);
    put32(c + 24, e.size); put16(c + 28, nameLength); put32(c + 42, e.offset);
    return f.write(c, sizeof(c)) == sizeof(c) &&
           f.write(reinterpret_cast<const uint8_t *>(e.name), nameLength) == nameLength;
}

bool fetchPhoto(const char *url, uint8_t *&data, size_t &length, char *error, size_t errorLength)
{
    data = nullptr;
    length = 0;
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 10000;
    config.buffer_size = 4096;
    config.disable_auto_redirect = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        snprintf(error, errorLength, "HTTP-Client nicht verfügbar");
        return false;
    }
    bool ok = false;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        snprintf(error, errorLength, "keine Verbindung");
    } else {
        const int64_t announced = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            snprintf(error, errorLength, "HTTP %d", status);
        } else if (announced <= 0 || announced > (int64_t)kMaxPhotoBytes) {
            snprintf(error, errorLength, "unerwartete Größe %lld", (long long)announced);
        } else if (!(data = static_cast<uint8_t *>(
                         heap_caps_malloc((size_t)announced, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))) {
            snprintf(error, errorLength, "kein PSRAM frei");
        } else {
            size_t got = 0;
            while (got < (size_t)announced) {
                const size_t want = std::min((size_t)announced - got, (size_t)16384);
                const int n = esp_http_client_read(client, reinterpret_cast<char *>(data) + got, (int)want);
                if (n <= 0)
                    break;
                got += (size_t)n;
            }
            if (got == (size_t)announced) {
                length = got;
                ok = true;
            } else {
                snprintf(error, errorLength, "abgebrochen nach %u B", (unsigned)got);
            }
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    if (!ok && data) {
        heap_caps_free(data);
        data = nullptr;
    }
    return ok;
}

// Writes the archive; runs in the collection task (core 0).
bool writeArchive()
{
    if (!clusterShotArchiveSdReady()) {
        setMessage("SD-Karte nicht bereit");
        return false;
    }
    if (!STORAGE.exists(kDir) && !STORAGE.mkdir(kDir)) {
        setMessage("Ordner /cluster_shots nicht anlegbar");
        return false;
    }
    const String finalPath = String(kDir) + "/" + fileName;
    const String partPath = finalPath + ".part";
    File f = STORAGE.open(partPath.c_str(), FILE_WRITE);
    if (!f) {
        setMessage("Archivdatei nicht anlegbar");
        return false;
    }

    const time_t sec = (time_t)(targetMs / 1000);
    struct tm t = {};
    gmtime_r(&sec, &t);
    const uint16_t dosTime = (uint16_t)((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec >> 1));
    const uint16_t dosDate = (uint16_t)(((t.tm_year + 1900 - 1980) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday);

    uint32_t offset = 0;
    size_t readyTotal = 0, index = 0;
    for (size_t i = 0; i < itemCount; ++i)
        readyTotal += items[i].ready ? 1 : 0;
    String csv = "node;uuid;frame_start_utc;dispatch_utc_us\n";
    bool writeError = false;
    char text[192], error[64];

    for (size_t i = 0; i < itemCount && !writeError; ++i) {
        Item &it = items[i];
        if (!it.ready)
            continue;
        snprintf(text, sizeof(text), "Lade %u/%u: %s …", (unsigned)++index, (unsigned)readyTotal, it.host);
        setMessage(text);
        uint8_t *data = nullptr;
        size_t length = 0;
        if (!fetchPhoto(it.url, data, length, error, sizeof(error))) {
            portENTER_CRITICAL(&archiveMux);
            ++failCount;
            portEXIT_CRITICAL(&archiveMux);
            snprintf(text, sizeof(text), "%s: %s", it.host, error);
            setMessage(text);
            continue;
        }
        if (g_storageLocked.load()) {
            heap_caps_free(data);
            setMessage("SD wurde gesperrt (Wartung) – Archiv abgebrochen");
            writeError = true;
            break;
        }
        char stamp[32];
        formatUtc(it.dispatchUs / 1000, stamp, sizeof(stamp), '-');
        snprintf(it.entryName, sizeof(it.entryName), "%s_%s.jpg", it.host, stamp);
        it.crc = crc32(data, length);
        it.size = (uint32_t)length;
        it.offset = offset;
        const ZipEntry e = {it.entryName, it.crc, it.size, it.offset};
        if (!writeLocalHeader(f, e, dosTime, dosDate) || f.write(data, length) != length) {
            heap_caps_free(data);
            setMessage("Schreibfehler auf der SD-Karte");
            writeError = true;
            break;
        }
        heap_caps_free(data);
        it.stored = true;
        offset += 30 + strlen(it.entryName) + it.size;
        char iso[32];
        formatUtc(it.dispatchUs / 1000, iso, sizeof(iso), ':');
        char dispatch[24];
        snprintf(dispatch, sizeof(dispatch), "%lld", (long long)it.dispatchUs);
        csv += String(it.host) + ";" + it.uuid + ";" + iso + ";" + dispatch + "\n";
        portENTER_CRITICAL(&archiveMux);
        ++doneCount;
        bytesWritten = offset;
        portEXIT_CRITICAL(&archiveMux);
    }

    uint16_t entries = 0;
    if (!writeError && doneCount) {
        const ZipEntry info = {"shot_info.csv", crc32(reinterpret_cast<const uint8_t *>(csv.c_str()), csv.length()),
                               (uint32_t)csv.length(), offset};
        writeError = !writeLocalHeader(f, info, dosTime, dosDate) ||
                     f.write(reinterpret_cast<const uint8_t *>(csv.c_str()), csv.length()) != csv.length();
        offset += 30 + strlen(info.name) + info.size;
        const uint32_t centralStart = offset;
        for (size_t i = 0; i < itemCount && !writeError; ++i) {
            const Item &it = items[i];
            if (!it.stored)
                continue;
            const ZipEntry e = {it.entryName, it.crc, it.size, it.offset};
            writeError = !writeCentralHeader(f, e, dosTime, dosDate);
            offset += 46 + strlen(it.entryName);
            ++entries;
        }
        if (!writeError) {
            writeError = !writeCentralHeader(f, info, dosTime, dosDate);
            offset += 46 + strlen(info.name);
            ++entries;
        }
        if (!writeError) {
            uint8_t end[22] = {};
            put32(end, 0x06054b50UL); put16(end + 8, entries); put16(end + 10, entries);
            put32(end + 12, offset - centralStart); put32(end + 16, centralStart);
            writeError = f.write(end, sizeof(end)) != sizeof(end);
            offset += sizeof(end);
        }
        if (writeError)
            setMessage("Schreibfehler auf der SD-Karte");
    }
    f.close();

    if (writeError || !doneCount) {
        STORAGE.remove(partPath.c_str());
        if (!writeError)
            setMessage("Kein Bild geladen");
        return false;
    }
    if (STORAGE.exists(finalPath.c_str()))
        STORAGE.remove(finalPath.c_str());
    if (!STORAGE.rename(partPath.c_str(), finalPath.c_str())) {
        setMessage("Umbenennen der Archivdatei fehlgeschlagen");
        return false;
    }
    portENTER_CRITICAL(&archiveMux);
    bytesWritten = offset;
    portEXIT_CRITICAL(&archiveMux);
    snprintf(text, sizeof(text), "Gespeichert: %u Bild(er)%s", (unsigned)doneCount,
             failCount ? " – nicht alle Nodes" : "");
    setMessage(text);
    return true;
}

void collectTask(void *)
{
    const bool ok = writeArchive();
    state = ok ? ArchiveState::Done : ArchiveState::Failed;
    vTaskDelete(nullptr);
}

const ClusterJobs::Entry *findJob(const ClusterJobs::Entry *jobs, size_t count, const Item &it)
{
    for (size_t j = 0; j < count; ++j)
        if (jobs[j].jobId == it.jobId && strcmp(jobs[j].nodeId, it.nodeId) == 0)
            return &jobs[j];
    return nullptr;
}

String jsonText(const char *text)
{
    String out;
    for (const char *p = text; *p; ++p) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
        else if ((uint8_t)*p < 0x20) out += ' ';
        else out += *p;
    }
    return out;
}

void sanitizeHost(char *host)
{
    for (char *p = host; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            *p = '_';
}

} // namespace

bool clusterShotArchiveSdReady()
{
    return sdReady && !g_storageLocked.load();
}

bool clusterShotArchiveBusy()
{
    return state == ArchiveState::Collecting;
}

bool clusterShotArchiveRequest(const String &nodeIds, int64_t targetUtcMs, String &out)
{
    if (!clusterLocalIsCoordinator()) { out = "Nur am Coordinator"; return false; }
    if (!clusterShotArchiveSdReady()) { out = "Keine SD-Karte im Coordinator"; return false; }
    if (state == ArchiveState::Waiting || state == ArchiveState::Collecting) {
        out = "Das Archiv des vorigen Shots wird noch erstellt";
        return false;
    }
    std::vector<String> ids;
    int start = 0;
    while (start <= (int)nodeIds.length() && ids.size() < kMaxItems) {
        int comma = nodeIds.indexOf(',', start);
        if (comma < 0) comma = nodeIds.length();
        String id = nodeIds.substring(start, comma);
        id.trim();
        if (id.length() && id.length() < ClusterJobs::kNodeIdLength)
            ids.push_back(id);
        start = comma + 1;
    }
    if (ids.empty()) { out = "Keine Nodes angegeben"; return false; }

    Item *table = static_cast<Item *>(sfPsramCalloc(ids.size(), sizeof(Item)));
    SfPsramBuffer jobBuffer(ClusterJobs::kMaxJobs * sizeof(ClusterJobs::Entry));
    ClusterJobs::Entry *jobs = reinterpret_cast<ClusterJobs::Entry *>(jobBuffer.data());
    if (!table || !jobs) {
        heap_caps_free(table);
        out = "Kein Speicher frei";
        return false;
    }
    const size_t jobCount = ClusterJobs::snapshot(jobs, ClusterJobs::kMaxJobs);
    const uint32_t now = millis();
    for (size_t i = 0; i < ids.size(); ++i) {
        Item &it = table[i];
        strlcpy(it.nodeId, ids[i].c_str(), sizeof(it.nodeId));
        uint32_t newestAge = UINT32_MAX;
        for (size_t j = 0; j < jobCount; ++j) {
            const ClusterJobs::Entry &e = jobs[j];
            const uint32_t age = now - e.createdMs;
            if (e.kind == ClusterJobs::Kind::Capture && strcmp(e.nodeId, it.nodeId) == 0 &&
                age <= kJobMatchWindowMs && age < newestAge) {
                newestAge = age;
                it.jobId = e.jobId;
                it.haveJob = true;
            }
        }
        it.finished = !it.haveJob;
    }

    int64_t nowUtcUs = 0;
    const int64_t leftMs = clusterTimeNowUs(nowUtcUs) ? std::max<int64_t>(0, targetUtcMs - nowUtcUs / 1000) : 0;
    heap_caps_free(items);
    portENTER_CRITICAL(&archiveMux);
    items = table;
    itemCount = ids.size();
    targetMs = targetUtcMs;
    doneCount = failCount = bytesWritten = 0;
    message[0] = '\0';
    portEXIT_CRITICAL(&archiveMux);
    waitUntilMs = now + (uint32_t)std::min<int64_t>(leftMs, 1800000) + kResultWaitMs;
    lastPollMs = now;
    char stamp[32];
    formatUtc(targetUtcMs, stamp, sizeof(stamp), '-');
    snprintf(fileName, sizeof(fileName), "shot_%s.zip", stamp);
    setMessage("Wartet auf die Bilder der Nodes …");
    state = ArchiveState::Waiting;
    out = String("Archiv ") + fileName + " vorgemerkt";
    return true;
}

void clusterShotArchiveLoop()
{
    if (state != loggedState) {
        if (state == ArchiveState::Done || state == ArchiveState::Failed)
            logWrite(String("Shot-Archiv ") + fileName + ": " + message);
        loggedState = state;
    }
    if (state != ArchiveState::Waiting)
        return;
    const uint32_t now = millis();
    if (now - lastPollMs < 1000UL)
        return;
    lastPollMs = now;

    SfPsramBuffer jobBuffer(ClusterJobs::kMaxJobs * sizeof(ClusterJobs::Entry));
    ClusterJobs::Entry *jobs = reinterpret_cast<ClusterJobs::Entry *>(jobBuffer.data());
    if (!jobs)
        return;
    const size_t jobCount = ClusterJobs::snapshot(jobs, ClusterJobs::kMaxJobs);
    bool allFinished = true;
    size_t ready = 0;
    for (size_t i = 0; i < itemCount; ++i) {
        Item &it = items[i];
        if (!it.finished) {
            const ClusterJobs::Entry *e = findJob(jobs, jobCount, it);
            if (!e) {
                it.finished = true;  // dropped from the job table
            } else if (e->state == ClusterJobs::State::Succeeded) {
                it.finished = true;
                if (e->mediaUuid[0]) {
                    strlcpy(it.uuid, e->mediaUuid, sizeof(it.uuid));
                    it.dispatchUs = e->dispatchUtcUs;
                    it.ready = true;
                }
            } else if (e->state == ClusterJobs::State::Failed || e->state == ClusterJobs::State::TimedOut ||
                       e->state == ClusterJobs::State::Cancelled) {
                it.finished = true;
            }
        }
        allFinished = allFinished && it.finished;
        ready += it.ready ? 1 : 0;
    }
    char text[96];
    snprintf(text, sizeof(text), "Wartet auf die Bilder der Nodes … %u/%u bereit", (unsigned)ready, (unsigned)itemCount);
    setMessage(text);
    if (!allFinished && (int32_t)(now - waitUntilMs) < 0)
        return;

    // Resolve the signed photo URLs here (loop task: cluster tables, crypto).
    ready = 0;
    for (size_t i = 0; i < itemCount; ++i) {
        Item &it = items[i];
        if (!it.ready)
            continue;
        String url, host;
        if (!clusterCaptureMediaSource(it.nodeId, it.uuid, url, host) || url.length() >= sizeof(it.url)) {
            it.ready = false;
            continue;
        }
        strlcpy(it.url, url.c_str(), sizeof(it.url));
        strlcpy(it.host, host.c_str(), sizeof(it.host));
        sanitizeHost(it.host);
        ++ready;
    }
    portENTER_CRITICAL(&archiveMux);
    failCount = itemCount - ready;
    portEXIT_CRITICAL(&archiveMux);
    if (!ready) {
        setMessage("Kein Bild des Shots verfügbar");
        state = ArchiveState::Failed;
        return;
    }
    state = ArchiveState::Collecting;
    if (xTaskCreatePinnedToCore(collectTask, "sf_shot_zip", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
        setMessage("Sammel-Task nicht startbar");
        state = ArchiveState::Failed;
    }
}

String clusterShotArchiveStatusJson()
{
    char msg[sizeof(message)];
    portENTER_CRITICAL(&archiveMux);
    strlcpy(msg, message, sizeof(msg));
    const uint32_t done = doneCount, failed = failCount, bytes = bytesWritten;
    const size_t total = itemCount;
    const int64_t target = targetMs;
    portEXIT_CRITICAL(&archiveMux);
    char targetText[24];
    snprintf(targetText, sizeof(targetText), "%lld", (long long)target);
    return String("{\"sd\":") + (clusterShotArchiveSdReady() ? "true" : "false") +
           ",\"state\":\"" + stateName(state) + "\",\"target_utc_ms\":" + targetText +
           ",\"total\":" + String((unsigned)total) + ",\"done\":" + String((unsigned long)done) +
           ",\"failed\":" + String((unsigned long)failed) + ",\"bytes\":" + String((unsigned long)bytes) +
           ",\"file\":\"" + jsonText(fileName) + "\",\"message\":\"" + jsonText(msg) + "\"}";
}

String clusterShotArchiveListJson(size_t maxFiles)
{
    if (!clusterShotArchiveSdReady())
        return "[]";
    File dir = STORAGE.open(kDir);
    if (!dir || !dir.isDirectory())
        return "[]";
    std::vector<std::pair<String, uint32_t>> files;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        const String name = e.name();
        if (!e.isDirectory() && name.endsWith(".zip"))
            files.push_back({name, (uint32_t)e.size()});
        e.close();
    }
    dir.close();
    // Names carry the shot time: newest first.
    std::sort(files.begin(), files.end(),
              [](const std::pair<String, uint32_t> &a, const std::pair<String, uint32_t> &b) { return a.first > b.first; });
    String json = "[";
    for (size_t i = 0; i < files.size() && i < maxFiles; ++i)
        json += String(i ? "," : "") + "{\"name\":\"" + jsonText(files[i].first.c_str()) +
                "\",\"bytes\":" + String((unsigned long)files[i].second) + "}";
    return json + "]";
}

bool clusterShotArchivePath(const String &name, String &path)
{
    if (name.length() < 5 || name.length() > 80 || !name.endsWith(".zip") || name.indexOf("..") >= 0)
        return false;
    for (size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.')
            return false;
    }
    path = String(kDir) + "/" + name;
    return true;
}
