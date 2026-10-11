#include "firmware_manifest_probe.h"
#include "board_config.h"
#include <WiFi.h>
#include <esp_http_client.h>
#include <stdlib.h>
#include <string.h>
#include "sensorforge_version.h"
#include <cJSON.h>
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

namespace {
constexpr size_t kMaxManifest = 8192;
struct DownloadBuffer {
    char *data;
    size_t count;
    bool overflow;
};

esp_err_t onHttpEvent(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data) return ESP_OK;
    auto *buffer = static_cast<DownloadBuffer *>(event->user_data);
    if (event->data_len <= 0) return ESP_OK;
    const size_t len = static_cast<size_t>(event->data_len);
    if (len > kMaxManifest - buffer->count) {
        buffer->overflow = true;
        return ESP_FAIL;
    }
    memcpy(buffer->data + buffer->count, event->data, len);
    buffer->count += len;
    return ESP_OK;
}
// Metadata checks are deliberately not an authorization decision. The actual
// .sfw package still requires its independent cryptographic verification.
const char *jsonString(const cJSON *obj, const char *key)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(field) ? field->valuestring : nullptr;
}

bool validHttpsPackageUrl(const char *url)
{
    if (!url) return false;
    const size_t len = strlen(url);
    if (len < 12 || len > 240 || strncmp(url, "https://", 8) != 0) return false;
    // Forbid credentials and fragments; do not accept redirects later without
    // a separate explicit URL policy.
    const char *hostEnd = strpbrk(url + 8, "/?#");
    const size_t hostLen = hostEnd ? static_cast<size_t>(hostEnd - (url + 8)) : strlen(url + 8);
    if (hostLen == 0) return false;
    for (size_t i = 8; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(url[i]);
        if (c <= 32 || c >= 127 || c == '@' || c == '#') return false;
    }
    return true;
}

bool validSha256(const char *sha)
{
    if (!sha || strlen(sha) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        const char c = sha[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// Accept only the project's explicit numeric beta scheme for comparisons.
// Unknown naming conventions must never be guessed into an upgrade order.
bool parseBetaVersion(const char *version, unsigned long &major, unsigned long &beta)
{
    if (!version || version[0] != 'v') return false;
    const char *p = version + 1;
    if (*p < '0' || *p > '9') return false;
    char *end = nullptr;
    major = strtoul(p, &end, 10);
    if (!end || end == p || major > 100000UL || strncmp(end, "-beta", 5) != 0) return false;
    p = end + 5;
    if (*p < '0' || *p > '9') return false;
    beta = strtoul(p, &end, 10);
    return end && *end == 0 && beta <= 100000UL;
}

bool validRelease(const cJSON *release)
{
    if (!cJSON_IsObject(release)) return false;
    const char *board = jsonString(release, "board");
    const char *channel = jsonString(release, "channel");
    const char *version = jsonString(release, "version");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(release, "size");
    if (!board || (strcmp(board, "xiao") && strcmp(board, "freenove"))) return false;
    if (!channel || (strcmp(channel, "stable") && strcmp(channel, "beta"))) return false;
    if (!version || !*version || strlen(version) > 64) return false;
    for (const char *p = version; *p; ++p) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' ||
               *p == '_' || *p == '+')) return false;
    }
    // cJSON stores numbers as doubles: require an exact positive integer.
    if (!cJSON_IsNumber(size) || size->valuedouble < 1 ||
        size->valuedouble > 16777216 ||
        size->valuedouble != static_cast<double>(static_cast<uint32_t>(size->valuedouble))) return false;
    return validHttpsPackageUrl(jsonString(release, "url")) &&
           validSha256(jsonString(release, "sha256"));
}

} // namespace

bool firmwareManifestFindCandidate(const String &url, FirmwareManifestCandidate &candidate, String &result)
{
    candidate = FirmwareManifestCandidate{};
    result = "Manifest-Abfrage nicht gestartet";
    if (WiFi.status() != WL_CONNECTED) {
        result = "Kein WLAN verbunden";
        return false;
    }
    if (!url.startsWith("https://") || url.length() > 240) {
        result = "Nur HTTPS-Manifest-URLs erlaubt";
        return false;
    }
#if !defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) || !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    result = "TLS-Zertifikatspruefung nicht verfuegbar (CA-Bundle fehlt)";
    return false;
#else
    // Keep RAM bounded on memory-constrained ESP32-S3 devices.
    char *data = static_cast<char *>(malloc(kMaxManifest + 1));
    if (!data) {
        result = "Zu wenig freier RAM fuer Manifest";
        return false;
    }
    DownloadBuffer buffer{data, 0, false};
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 4000;
    config.event_handler = onHttpEvent;
    config.user_data = &buffer;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(data);
        result = "HTTPS-Client konnte nicht initialisiert werden";
        return false;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || buffer.overflow || status != 200 || buffer.count == 0) {
        result = buffer.overflow ? "Manifest zu gross (maximal 8 KiB)" :
                 (err != ESP_OK ? "HTTPS-Verbindung oder Zertifikatspruefung fehlgeschlagen" :
                 "HTTP-Status " + String(status) + " (erwartet 200)");
        free(data);
        return false;
    }
    data[buffer.count] = 0;
    cJSON *root = cJSON_ParseWithLength(data, buffer.count);
    free(data);
    if (!root) {
        result = "Antwort ist kein gueltiges JSON";
        return false;
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *releases = cJSON_GetObjectItemCaseSensitive(root, "releases");
    const bool valid = cJSON_IsObject(root) && cJSON_IsNumber(schema) &&
                       schema->valuedouble == 1 && cJSON_IsArray(releases) &&
                       cJSON_GetArraySize(releases) <= 32;
    if (!valid) {
        cJSON_Delete(root);
        result = "Manifest-Schema ungueltig (erwartet schema=1 und releases-Array, max. 32)";
        return false;
    }
    const int count = cJSON_GetArraySize(releases);
    int boardMatches = 0;
    int badEntries = 0;
    int comparableNewer = 0;
    int unknownVersion = 0;
    int duplicates = 0;
    String candidateVersion;
    String candidateUrl;
    String candidateSha;
    uint32_t candidateSize = 0;
    unsigned long candidateMajor = 0, candidateBeta = 0;
    unsigned long currentMajor = 0, currentBeta = 0;
    const bool currentComparable = parseBetaVersion(SENSORFORGE_RELEASE_TAG, currentMajor, currentBeta);
#if defined(BOARD_XIAO)
    const char *localBoard = "xiao";
#elif defined(BOARD_FREENOVE)
    const char *localBoard = "freenove";
#else
    const char *localBoard = "unknown";
#endif
    for (int i = 0; i < count; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(releases, i);
        if (!validRelease(entry)) {
            ++badEntries;
            continue;
        }
        if (strcmp(jsonString(entry, "board"), localBoard) != 0) continue;
        ++boardMatches;
        // A release never becomes installable through this read-only probe.
        // Only compare beta-to-beta; stable and unknown schemes require a
        // separate explicit version policy before any automatic selection.
        if (strcmp(jsonString(entry, "channel"), "beta") != 0) continue;
        unsigned long major = 0, beta = 0;
        const char *version = jsonString(entry, "version");
        if (!currentComparable || !parseBetaVersion(version, major, beta)) {
            ++unknownVersion;
            continue;
        }
        if (major < currentMajor || (major == currentMajor && beta <= currentBeta)) continue;
        ++comparableNewer;
        if (candidateVersion.length() && major == candidateMajor && beta == candidateBeta) {
            ++duplicates;
        }
        if (!candidateVersion.length() || major > candidateMajor ||
            (major == candidateMajor && beta > candidateBeta)) {
            candidateMajor = major;
            candidateBeta = beta;
            candidateVersion = version;
            candidateUrl = jsonString(entry, "url");
            candidateSha = jsonString(entry, "sha256");
            candidateSize = static_cast<uint32_t>(cJSON_GetObjectItemCaseSensitive(entry, "size")->valuedouble);
            duplicates = 0;
        }
    }
    cJSON_Delete(root);
    if (badEntries) {
        result = "Manifest abgelehnt: " + String(badEntries) +
                 " ungueltige Release-Eintraege von " + String(count) +
                 ". Erwartet board, channel, version, url (HTTPS), size, sha256.";
        return false;
    }
    result = "Manifest per HTTPS gelesen: " + String(count) +
             " formal gueltige Eintraege; " + String(boardMatches) +
             " fuer Board " + localBoard + ". Installiert: " SENSORFORGE_RELEASE_TAG ". ";
    if (!currentComparable) {
        result += "Versionsschema der installierten Firmware nicht vergleichbar. ";
    } else if (candidateVersion.length() && duplicates == 0) {
        result += "Neuerer Beta-Kandidat (UNVERIFIZIERT): " + candidateVersion + ". ";
    } else if (duplicates) {
        result += "Mehrdeutige hoechste Beta-Version; keine Auswahl. ";
    } else {
        result += "Kein eindeutig neuerer Beta-Kandidat gefunden. ";
    }
    if (currentComparable && candidateVersion.length() && duplicates == 0) {
        candidate.version = candidateVersion;
        candidate.packageUrl = candidateUrl;
        candidate.sha256Hex = candidateSha;
        candidate.packageSize = candidateSize;
        candidate.available = true;
    }
    if (unknownVersion) result += String(unknownVersion) + " Beta-Version(en) nicht vergleichbar. ";
    result += "Nur Metadaten; keine Manifest-Signaturpruefung, kein Paketdownload und keine Updatefreigabe.";
    return true;
#endif
}

// Preserve the existing WebConfig read-only API.
bool firmwareManifestProbe(const String &url, String &result)
{
    FirmwareManifestCandidate ignored;
    return firmwareManifestFindCandidate(url, ignored, result);
}
