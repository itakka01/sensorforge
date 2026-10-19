#pragma once
// Pure decisions for the optional EXAPAN converter in day ZIP downloads.
// No Arduino/ESP-IDF dependencies (host test: test_day_archive_tools.cpp.test).
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "archive_tools_manifest.h"

namespace DayArchiveToolsPolicy {

enum class ConverterStatus : uint8_t {
    Current,      // released, newest version -> include
    Outdated,     // released, older version  -> include + notice
    NoMarker,     // no "# EXAPAN_CONVERTER_VERSION=" line -> exclude + notice
    Modified,     // marker known, SHA-256 does not match -> exclude + notice
    Unknown       // marker newer than this firmware knows -> exclude + notice
};

// Parses the first line of the form "# EXAPAN_CONVERTER_VERSION=<n>".
// Returns 0 when absent or malformed.
inline uint16_t parseVersionMarker(const char *data, size_t length) {
    static const char kKey[] = "# EXAPAN_CONVERTER_VERSION=";
    const size_t keyLength = sizeof(kKey) - 1;
    size_t lineStart = 0;
    while (lineStart < length) {
        size_t lineEnd = lineStart;
        while (lineEnd < length && data[lineEnd] != '\n') ++lineEnd;
        if (lineEnd - lineStart > keyLength && memcmp(data + lineStart, kKey, keyLength) == 0) {
            uint32_t value = 0;
            size_t i = lineStart + keyLength;
            size_t digits = 0;
            while (i < lineEnd && data[i] >= '0' && data[i] <= '9' && digits < 5) {
                value = value * 10U + static_cast<uint32_t>(data[i] - '0');
                ++i; ++digits;
            }
            while (i < lineEnd && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r')) ++i;
            if (!digits || i != lineEnd || value == 0 || value > 0xFFFFU) return 0;
            return static_cast<uint16_t>(value);
        }
        lineStart = lineEnd + 1;
    }
    return 0;
}

// The SHA-256 decides; the marker only explains why a file is rejected.
inline ConverterStatus classify(uint16_t markerVersion, const char *sha256Hex,
                                const ArchiveToolRelease *releases, size_t releaseCount,
                                uint16_t currentVersion) {
    if (!markerVersion) return ConverterStatus::NoMarker;
    for (size_t i = 0; i < releaseCount; ++i) {
        if (releases[i].version == markerVersion && strcmp(releases[i].sha256Hex, sha256Hex) == 0)
            return markerVersion >= currentVersion ? ConverterStatus::Current : ConverterStatus::Outdated;
    }
    return markerVersion > currentVersion ? ConverterStatus::Unknown : ConverterStatus::Modified;
}

}  // namespace DayArchiveToolsPolicy
