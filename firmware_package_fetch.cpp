#include "firmware_package_fetch.h"
#include "firmware_security.h"
#include "firmware_compat_matcher.h"
#include "firmware_ota_verify.h"
#include "firmware_update_admission.h"
#include "firmware_ota_activation.h"
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <string.h>
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

extern const char *firmwareExpectedCompatibilityMarker();

namespace {
// mbedTLS 2.x exposes *_ret (int result); mbedTLS 3.x exposes the
// unsuffixed SHA-256 functions (void result). Keep failures fail-closed.
static bool sha256Start(mbedtls_sha256_context *ctx) {
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_starts(ctx, 0);
    return true;
#else
    return mbedtls_sha256_starts_ret(ctx, 0) == 0;
#endif
}
static bool sha256Update(mbedtls_sha256_context *ctx, const unsigned char *buf, size_t len) {
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_update(ctx, buf, len);
    return true;
#else
    return mbedtls_sha256_update_ret(ctx, buf, len) == 0;
#endif
}
static bool sha256Finish(mbedtls_sha256_context *ctx, unsigned char out[32]) {
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_finish(ctx, out);
    return true;
#else
    return mbedtls_sha256_finish_ret(ctx, out) == 0;
#endif
}
constexpr uint32_t kMaxPackageSize = 16U * 1024U * 1024U;
constexpr int kChunk = 2048;

// https:// or http:// to a private IPv4 (firmware_package_format.h); the
// package signature authenticates the firmware either way.
bool validUrl(const String &url) {
    const char *error = nullptr;
    return FirmwarePackage::urlAllowed(url.c_str(), error);
}

bool validDigest(const String &sha) {
    if (sha.length() != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        const char c = sha[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

String hexDigest(const unsigned char bytes[32]) {
    const char *digits = "0123456789abcdef";
    char out[65];
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 0x0f];
    }
    out[64] = 0;
    return String(out);
}
} // namespace

namespace {
bool firmwarePackageDownloadInternal(const FirmwareManifestCandidate &candidate,
                                     String &result, bool stage, bool callerOwnsAdmission) {
    struct AdmissionGuard {
        bool held = false;
        ~AdmissionGuard() { if (held) FirmwareUpdateAdmission::release(); }
    } admission;
    if (stage) {
        if (callerOwnsAdmission) {
            // The runner already owns the exclusive OTA reservation.
            // Never acquire (or release) it twice.
            if (!FirmwareUpdateAdmission::active()) {
                result = "OTA-Reservierung fehlt.";
                return false;
            }
        } else {
            String reason;
            if (!FirmwareUpdateAdmission::tryAcquire(reason)) {
                result = "OTA-Reservierung verweigert: " + reason;
                return false;
            }
            admission.held = true;
        }
    }
    if (stage) FirmwareOtaActivation::forget(); // Never reuse an earlier stage.
    esp_ota_handle_t otaHandle = 0;
    bool otaActive = false;

    result = "Paketdownload nicht gestartet.";
    if (!candidate.available || candidate.packageSize == 0 ||
        candidate.packageSize > kMaxPackageSize ||
        !validUrl(candidate.packageUrl) || !validDigest(candidate.sha256Hex)) {
        result = "Unvollstaendige oder ungueltige Paketmetadaten.";
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        result = "Kein Wi-Fi verbunden.";
        return false;
    }
    const bool https = candidate.packageUrl.startsWith("https://");
    esp_http_client_config_t config = {};
    config.url = candidate.packageUrl.c_str();
    config.timeout_ms = 5000;
    if (https) {
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        config.crt_bundle_attach = esp_crt_bundle_attach;
#else
        result = "TLS-Zertifikatspruefung nicht verfuegbar.";
        return false;
#endif
    }
    config.disable_auto_redirect = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { result = "HTTP-Client nicht verfuegbar."; return false; }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_err_t error = esp_http_client_open(client, 0);
    if (error != ESP_OK) {
        result = https ? "HTTPS-Verbindung fehlgeschlagen." : "HTTP-Verbindung fehlgeschlagen.";
        esp_http_client_cleanup(client);
        return false;
    }
    const int64_t announced = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200 || (announced >= 0 && announced != candidate.packageSize)) {
        result = status != 200 ? "HTTP-Status nicht 200 (Weiterleitungen verboten)." : "Content-Length stimmt nicht.";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    // Check a signed SensorForge .sfw as it streams in. Nothing is written to
    // flash and no boot partition is modified by this diagnostic downloader.
    const esp_partition_t *inactive = esp_ota_get_next_update_partition(nullptr);
    if (!inactive) {
        result = "Keine inaktive OTA-Partition verfuegbar.";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    FirmwareCompatibilityMatcher marker;
    if (!marker.begin(firmwareExpectedCompatibilityMarker())) {
        result = "Board-Kompatibilitaetsmarker fehlt.";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    uint8_t header[SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE] = {};
    size_t headerUsed = 0;
    SensorForgeFirmwarePackageInfo package;
    SensorForgeFirmwareHashContext payloadHash;
    bool headerOk = false;
    bool imageMagicOk = false;
    size_t payloadUsed = 0;
    String packageError;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = sha256Start(&sha);
    uint32_t count = 0;
    uint8_t chunk[kChunk];
    // An overall elapsed-time guard prevents a server from keeping a download
    // alive indefinitely by delivering a byte just before every read timeout.
    const uint32_t startMs = millis();
    while (ok && count < candidate.packageSize) {
        if (static_cast<uint32_t>(millis() - startMs) > 180000U) { ok = false; result = "Download-Zeitlimit erreicht."; break; }
        // Runs in the loop task (30 s task watchdog); a download takes longer.
        esp_task_wdt_reset();
        const int want = candidate.packageSize - count > kChunk ? kChunk : static_cast<int>(candidate.packageSize - count);
        const int n = esp_http_client_read(client, reinterpret_cast<char *>(chunk), want);
        if (n <= 0) { ok = false; result = "Paket unvollstaendig oder Verbindung abgebrochen."; break; }
        ok = sha256Update(&sha, chunk, static_cast<size_t>(n));
        // .sfw = 256-byte signed header (format 2) followed by the raw ESP application.
        size_t pos = 0;
        if (ok && !headerOk) {
            const size_t take = static_cast<size_t>(n) < sizeof(header) - headerUsed
                ? static_cast<size_t>(n) : sizeof(header) - headerUsed;
            memcpy(header + headerUsed, chunk, take);
            headerUsed += take;
            pos += take;
            if (headerUsed == sizeof(header)) {
                ok = firmwareSecurityParseHeader(header, sizeof(header), package, packageError);
                // Board/downgrade check before anything is written (repeated
                // after the signature check in firmwareOtaVerifyCompletePackage).
                if (ok) {
                    package.allowOlder = candidate.allowOlder;
                    ok = firmwareSecurityCheckPolicy(package, packageError);
                }
                if (ok) {
                    ok = package.firmwareSize > 0 &&
                         package.firmwareSize <= inactive->size &&
                         static_cast<uint64_t>(package.firmwareSize) + sizeof(header) == candidate.packageSize;
                    if (!ok) packageError = "Paketgroesse oder OTA-Partitionsgroesse ungueltig";
                }
                if (ok) ok = firmwareSecurityHashBegin(payloadHash, packageError);
                if (ok && stage) {
                    const esp_err_t e = esp_ota_begin(inactive, package.firmwareSize, &otaHandle);
                    if (e != ESP_OK) {
                        ok = false;
                        packageError = "OTA-Partition konnte nicht vorbereitet werden";
                    } else {
                        otaActive = true;
                    }
                }
                headerOk = ok;
            }
        }
        if (ok && headerOk && pos < static_cast<size_t>(n)) {
            const uint8_t *payload = chunk + pos;
            const size_t len = static_cast<size_t>(n) - pos;
            if (!imageMagicOk) {
                imageMagicOk = payload[0] == 0xE9;
                if (!imageMagicOk) packageError = "ESP-Image-Magic ungueltig";
            }
            if (imageMagicOk && len <= package.firmwareSize - payloadUsed) {
                marker.feed(payload, len);
                ok = firmwareSecurityHashUpdate(payloadHash, payload, len, packageError);
                if (ok && stage) {
                    if (!otaActive || esp_ota_write(otaHandle, payload, len) != ESP_OK) {
                        ok = false;
                        packageError = "OTA-Schreibfehler";
                    }
                }
                if (ok) payloadUsed += len;
            } else {
                ok = false;
                if (packageError.isEmpty()) packageError = "Payloadgroesse ungueltig";
            }
        }
        count += static_cast<uint32_t>(n);
    }
    if (ok && (!headerOk || !imageMagicOk || !marker.found())) {
        ok = false;
        packageError = "Paketheader, ESP-Image oder Boardmarker ungueltig";
    }
    // Verify the independently signed image, not merely a manifest checksum.
    if (ok) ok = firmwareOtaVerifyCompletePackage(package, payloadHash,
        marker.found(), payloadUsed, count, candidate.packageSize, packageError);
    if (payloadHash.active) firmwareSecurityHashAbort(payloadHash);
    unsigned char digest[32] = {};
    if (ok) ok = sha256Finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (ok) {
        String expected = candidate.sha256Hex;
        expected.toLowerCase();
        if (hexDigest(digest) != expected) {
            ok = false;
            result = "Paket-SHA-256 stimmt nicht mit Manifest ueberein.";
        }
    }
    if (otaActive) {
        if (ok && stage) {
            const esp_err_t e = esp_ota_end(otaHandle);
            otaActive = false;  // esp_ota_end consumes the handle, including on failure
            if (e != ESP_OK) {
                ok = false;
                packageError = "ESP-Image-Validierung beim OTA-Abschluss fehlgeschlagen";
            }
        } else {
            esp_ota_abort(otaHandle);
            otaActive = false;
        }
    }
    if (!ok) {
        if (!packageError.isEmpty()) result = "Signiertes .sfw abgelehnt: " + packageError;
        else if (result == "Paketdownload nicht gestartet.") result = "Download- oder SHA-256-Verarbeitung fehlgeschlagen.";
        return false;
    }
    // The signed release, not the (untrusted) manifest version.
    if (stage && !FirmwareOtaActivation::remember(inactive, String(package.header.release), result)) return false;
    result = stage
        ? "Signiertes Firmwarepaket in inaktiver OTA-Partition bereitgestellt; Fingerabdruck erfasst, kein Neustart."
        : "Paket vollstaendig geladen; SHA-256, Boardmarker und .sfw-Signatur verifiziert. Keine Speicherung, keine Installation.";
    return true;
}

} // namespace

bool firmwarePackageVerifyDownload(const FirmwareManifestCandidate &candidate, String &result) {
    return firmwarePackageDownloadInternal(candidate, result, false, false);
}

bool firmwarePackageStageDownload(const FirmwareManifestCandidate &candidate, String &result) {
    return firmwarePackageDownloadInternal(candidate, result, true, false);
}

// Only for callers that already hold OTA admission throughout manifest selection.
bool firmwarePackageStageDownloadAdmitted(const FirmwareManifestCandidate &candidate, String &result) {
    return firmwarePackageDownloadInternal(candidate, result, true, true);
}
