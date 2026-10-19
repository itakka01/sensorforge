#include "day_archive_tools.h"
#include "day_archive_tools_policy.h"
#include "board_config.h"
#include "logger.h"

#include <FS.h>
#include <mbedtls/sha256.h>
#include <utility>

namespace {

const char kToolsDir[] = "/tools";
const char kConverterName[] = "exapan_archive_convert.cmd";
const char kReadmeName[] = "README_Exapan_Archive_Converter.md";
// Current files are ~17 KB / ~9 KB. The bound keeps a manipulated SD file from
// exhausting memory; buffers above 4 KB are placed in PSRAM by the heap policy.
constexpr size_t kMaxToolBytes = 64U * 1024U;

enum class ReadResult : uint8_t { Ok, Missing, Error };

// Reads a small text file completely. Rejects directories, oversize files,
// short reads and NUL bytes (the content is streamed from a String).
ReadResult readToolFile(const String &path, String &out, String &error) {
    out = "";
    if (!STORAGE.exists(path)) return ReadResult::Missing;
    File file = STORAGE.open(path, FILE_READ);
    if (!file) { error = "Datei kann nicht geoeffnet werden"; return ReadResult::Error; }
    if (file.isDirectory()) { file.close(); error = "Pfad ist ein Verzeichnis"; return ReadResult::Error; }
    const size_t size = file.size();
    if (size == 0 || size > kMaxToolBytes) {
        file.close();
        error = size ? "Datei zu gross (max. 64 KB)" : "Datei ist leer";
        return ReadResult::Error;
    }
    if (!out.reserve(size)) { file.close(); error = "Zu wenig Speicher"; return ReadResult::Error; }
    char buffer[512];
    size_t total = 0;
    while (total < size) {
        const int n = file.read(reinterpret_cast<uint8_t *>(buffer), sizeof(buffer));
        if (n <= 0) break;
        if (memchr(buffer, 0, static_cast<size_t>(n))) {
            file.close(); out = ""; error = "Datei enthaelt Binaerdaten"; return ReadResult::Error;
        }
        out.concat(buffer, static_cast<unsigned int>(n));
        total += static_cast<size_t>(n);
    }
    file.close();
    if (total != size || out.length() != size) {
        out = ""; error = "Lesefehler"; return ReadResult::Error;
    }
    return ReadResult::Ok;
}

bool sha256Hex(const String &data, char hex[65]) {
    uint8_t digest[32];
    if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(data.c_str()), data.length(), digest, 0) != 0)
        return false;
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        hex[i * 2] = kDigits[digest[i] >> 4];
        hex[i * 2 + 1] = kDigits[digest[i] & 0x0F];
    }
    hex[64] = '\0';
    return true;
}

String noticeHeader() {
    return String(
        "SensorForge Tagesarchiv - Hinweis zum EXAPAN Archive Converter\r\n"
        "==============================================================\r\n\r\n");
}

String noticeFooter() {
    return String(
        "\r\nDie Aufnahmen in diesem Archiv sind davon nicht betroffen.\r\n"
        "Konverter-Dateien gehoeren auf die SD-Karte des Geraets nach /tools/:\r\n"
        "  /tools/exapan_archive_convert.cmd\r\n"
        "  /tools/README_Exapan_Archive_Converter.md\r\n");
}

}  // namespace

void dayArchiveCollectTools(std::vector<DayArchiveToolFile> &files, String &notice) {
    using DayArchiveToolsPolicy::ConverterStatus;
    files.clear();
    notice = "";

    const String converterPath = String(kToolsDir) + "/" + kConverterName;
    String converter;
    String error;
    const ReadResult converterRead = readToolFile(converterPath, converter, error);
    if (converterRead == ReadResult::Missing) {
        notice = noticeHeader() +
                 "Der Konverter wurde nicht beigelegt: " + converterPath + " ist nicht vorhanden.\r\n" +
                 noticeFooter();
        return;  // Normal optional state: no log entry.
    }
    if (converterRead == ReadResult::Error) {
        notice = noticeHeader() + "Der Konverter wurde nicht beigelegt: " + converterPath +
                 " (" + error + ").\r\n" + noticeFooter();
        logWrite("[DAYZIP] Converter not included: " + converterPath + " (" + error + ")");
        return;
    }

    char hex[65];
    if (!sha256Hex(converter, hex)) {
        notice = noticeHeader() + "Der Konverter wurde nicht beigelegt: Pruefsumme nicht berechenbar.\r\n" +
                 noticeFooter();
        logWrite("[DAYZIP] Converter not included: SHA-256 failed");
        return;
    }
    const uint16_t marker =
        DayArchiveToolsPolicy::parseVersionMarker(converter.c_str(), converter.length());
    const ConverterStatus status = DayArchiveToolsPolicy::classify(
        marker, hex, kExapanConverterReleases,
        sizeof(kExapanConverterReleases) / sizeof(kExapanConverterReleases[0]),
        kExapanConverterCurrentVersion);
    const String current = "v" + String(kExapanConverterCurrentVersion);

    switch (status) {
    case ConverterStatus::Current:
    case ConverterStatus::Outdated:
        break;
    case ConverterStatus::NoMarker:
        notice = noticeHeader() +
                 "Der Konverter wurde NICHT beigelegt: Die Datei enthaelt keinen Versionseintrag\r\n"
                 "(# EXAPAN_CONVERTER_VERSION=...). Erwartet wird " + current + ".\r\n" + noticeFooter();
        logWrite("[DAYZIP] Converter not included: no version marker");
        return;
    case ConverterStatus::Modified:
        notice = noticeHeader() +
                 "Der Konverter wurde NICHT beigelegt: Die Datei gibt Version v" + String(marker) +
                 " an, ihr Inhalt\r\nentspricht aber keiner freigegebenen Version (veraendert oder beschaedigt).\r\n"
                 "Bitte die freigegebene Datei (" + current + ") erneut auf die SD-Karte kopieren.\r\n" +
                 noticeFooter();
        logWrite("[DAYZIP] Converter not included: v" + String(marker) + " content does not match release");
        return;
    case ConverterStatus::Unknown:
        notice = noticeHeader() +
                 "Der Konverter wurde NICHT beigelegt: Version v" + String(marker) +
                 " ist dieser Firmware unbekannt\r\n(diese Firmware kennt bis " + current +
                 "). Bitte die Firmware aktualisieren.\r\n" + noticeFooter();
        logWrite("[DAYZIP] Converter not included: v" + String(marker) + " unknown to firmware (" + current + ")");
        return;
    }

    DayArchiveToolFile converterFile;
    converterFile.name = kConverterName;
    converterFile.data = std::move(converter);
    files.push_back(std::move(converterFile));

    if (status == ConverterStatus::Outdated) {
        notice = noticeHeader() +
                 "Der beigelegte Konverter ist veraltet: v" + String(marker) + ", aktuell ist " + current +
                 ".\r\nEr funktioniert weiterhin; bitte die Datei auf der SD-Karte aktualisieren.\r\n" +
                 noticeFooter();
        logWrite("[DAYZIP] Converter v" + String(marker) + " included but outdated (" + current + ")");
    }

    // README is documentation only (not executed): included when readable.
    const String readmePath = String(kToolsDir) + "/" + kReadmeName;
    String readme;
    const ReadResult readmeRead = readToolFile(readmePath, readme, error);
    if (readmeRead == ReadResult::Ok) {
        DayArchiveToolFile readmeFile;
        readmeFile.name = kReadmeName;
        readmeFile.data = std::move(readme);
        files.push_back(std::move(readmeFile));
    } else {
        if (notice.isEmpty()) notice = noticeHeader();
        else notice += "\r\n";
        notice += "Die Anleitung " + readmePath +
                  (readmeRead == ReadResult::Missing ? String(" fehlt") : " konnte nicht gelesen werden (" + error + ")") +
                  "; der Konverter selbst wurde beigelegt.\r\n";
        if (notice.indexOf("Konverter-Dateien gehoeren") < 0) notice += noticeFooter();
    }
}
