#include "firmware_manifest_probe.h"
#include "board_config.h"
#include <WiFi.h>
#include <esp_http_client.h>
#include <stdlib.h>
#include <string.h>
#include "sensorforge_version.h"
#include "firmware_package_format.h"
#include "firmware_security.h"
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

// https:// or http:// to a private IPv4 (firmware_package_format.h).
bool validPackageUrl(const char *url)
{
    const char *error = nullptr;
    return url && FirmwarePackage::urlAllowed(url, error);
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

bool validRelease(const cJSON *release)
{
    if (!cJSON_IsObject(release)) return false;
    const char *board = jsonString(release, "board");
    const char *channel = jsonString(release, "channel");
    const char *version = jsonString(release, "version");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(release, "size");
    if (!board || (strcmp(board, "xiao") && strcmp(board, "freenove") && strcmp(board, "diymore"))) return false;
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
    return validPackageUrl(jsonString(release, "url")) &&
           validSha256(jsonString(release, "sha256"));
}

} // namespace

namespace {
// Download (https://, or http:// to a private IPv4) and schema check. On
// success root is the parsed manifest (caller cJSON_Delete) and releases its
// array.
bool fetchManifest(const String &url, cJSON *&root, const cJSON *&releases, String &result)
{
    root = nullptr;
    releases = nullptr;
    result = "Manifest-Abfrage nicht gestartet";
    if (WiFi.status() != WL_CONNECTED) {
        result = "Kein WLAN verbunden";
        return false;
    }
    const char *urlError = nullptr;
    if (!FirmwarePackage::urlAllowed(url.c_str(), urlError)) {
        result = String("Manifest-URL abgelehnt: ") + urlError;
        return false;
    }
    const bool https = url.startsWith("https://");
#if !defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) || !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (https) {
        result = "TLS-Zertifikatspruefung nicht verfuegbar (CA-Bundle fehlt)";
        return false;
    }
#endif
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
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (https)
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    config.disable_auto_redirect = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(data);
        result = "HTTP-Client konnte nicht initialisiert werden";
        return false;
    }
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || buffer.overflow || status != 200 || buffer.count == 0) {
        result = buffer.overflow ? "Manifest zu gross (maximal 8 KiB)" :
                 (err != ESP_OK ? (https ? "HTTPS-Verbindung oder Zertifikatspruefung fehlgeschlagen" : "HTTP-Verbindung fehlgeschlagen") :
                 "HTTP-Status " + String(status) + " (erwartet 200)");
        free(data);
        return false;
    }
    data[buffer.count] = 0;
    root = cJSON_ParseWithLength(data, buffer.count);
    free(data);
    if (!root) {
        result = "Antwort ist kein gueltiges JSON";
        return false;
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    releases = cJSON_GetObjectItemCaseSensitive(root, "releases");
    const bool valid = cJSON_IsObject(root) && cJSON_IsNumber(schema) &&
                       schema->valuedouble == 1 && cJSON_IsArray(releases) &&
                       cJSON_GetArraySize(releases) <= 32;
    if (!valid) {
        cJSON_Delete(root);
        root = nullptr;
        result = "Manifest-Schema ungueltig (erwartet schema=1 und releases-Array, max. 32)";
        return false;
    }
    return true;
}
} // namespace

bool firmwareManifestFindCandidate(const String &url, FirmwareManifestCandidate &candidate, String &result,
                                   bool allowOlder)
{
    candidate = FirmwareManifestCandidate{};
    cJSON *root = nullptr;
    const cJSON *releases = nullptr;
    if (!fetchManifest(url, root, releases, result))
        return false;
    const int count = cJSON_GetArraySize(releases);
    int boardMatches = 0;
    int badEntries = 0;
    int unknownVersion = 0;
    int duplicates = 0;
    String candidateVersion;
    String candidateUrl;
    String candidateSha;
    uint32_t candidateSize = 0;
    FirmwarePackage::Release best, current;
    const bool currentComparable = FirmwarePackage::parseRelease(SENSORFORGE_RELEASE_TAG, current);
    const char *localBoard = firmwareSecurityLocalBoardId();
    for (int i = 0; i < count; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(releases, i);
        if (!validRelease(entry)) {
            ++badEntries;
            continue;
        }
        if (strcmp(jsonString(entry, "board"), localBoard) != 0) continue;
        ++boardMatches;
        // Highest release for this board; normally only newer than the
        // installed one, with allowOlder (explicit downgrade) any release.
        FirmwarePackage::Release r;
        const char *version = jsonString(entry, "version");
        if (!currentComparable || !FirmwarePackage::parseRelease(version, r)) {
            ++unknownVersion;
            continue;
        }
        // The installed release itself is never a candidate (no reinstall).
        const int vsCurrent = FirmwarePackage::compareRelease(r, current);
        if (vsCurrent == 0 || (!allowOlder && vsCurrent < 0)) continue;
        if (candidateVersion.length() && FirmwarePackage::compareRelease(r, best) == 0) {
            ++duplicates;
        }
        if (!candidateVersion.length() || FirmwarePackage::compareRelease(r, best) > 0) {
            best = r;
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
                 ". Erwartet board, channel, version, url (https:// oder http:// im LAN), size, sha256.";
        return false;
    }
    result = String("Manifest gelesen: ") + String(count) +
             " formal gueltige Eintraege; " + String(boardMatches) +
             " fuer Board " + localBoard + ". Installiert: " SENSORFORGE_RELEASE_TAG ". ";
    if (!currentComparable) {
        result += "Versionsschema der installierten Firmware nicht vergleichbar. ";
    } else if (candidateVersion.length() && duplicates == 0) {
        result += String(allowOlder ? "Kandidat" : "Neuerer Kandidat") + " (UNVERIFIZIERT): " + candidateVersion + ". ";
    } else if (duplicates) {
        result += "Mehrdeutige hoechste Version; keine Auswahl. ";
    } else {
        result += "Kein eindeutig neuerer Kandidat gefunden. ";
    }
    if (currentComparable && candidateVersion.length() && duplicates == 0) {
        candidate.version = candidateVersion;
        candidate.packageUrl = candidateUrl;
        candidate.sha256Hex = candidateSha;
        candidate.packageSize = candidateSize;
        candidate.available = true;
        candidate.allowOlder = allowOlder;
    }
    if (unknownVersion) result += String(unknownVersion) + " Version(en) nicht vergleichbar. ";
    result += "Nur Metadaten; keine Manifest-Signaturpruefung, kein Paketdownload und keine Updatefreigabe.";
    return true;
}

// Preserve the existing WebConfig read-only API.
bool firmwareManifestProbe(const String &url, String &result)
{
    FirmwareManifestCandidate ignored;
    return firmwareManifestFindCandidate(url, ignored, result);
}

bool firmwareManifestList(const String &url, String &json, String &result)
{
    json = "[]";
    cJSON *root = nullptr;
    const cJSON *releases = nullptr;
    if (!fetchManifest(url, root, releases, result))
        return false;
    String out = "[";
    int bad = 0, n = 0;
    const int count = cJSON_GetArraySize(releases);
    for (int i = 0; i < count; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(releases, i);
        if (!validRelease(entry)) {
            ++bad;
            continue;
        }
        // Values passed validRelease(): no quotes or control characters.
        if (n++) out += ",";
        out += String("{\"board\":\"") + jsonString(entry, "board") + "\",\"version\":\"" +
               jsonString(entry, "version") + "\",\"channel\":\"" + jsonString(entry, "channel") +
               "\",\"size\":" + String((unsigned long)cJSON_GetObjectItemCaseSensitive(entry, "size")->valuedouble) + "}";
    }
    cJSON_Delete(root);
    json = out + "]";
    result = String(n) + " Eintraege" + (bad ? ", " + String(bad) + " ungueltig" : String());
    return bad == 0;
}
