#pragma once

#include <Arduino.h>
#include <time.h>

void mkvStart(const String &fullpath, int fps);
void mkvAddFrame();

// Sparse MJPEG path used by the continuous shooter. JPEGs are supplied from
// PSRAM and retain their real relative capture timestamps; no camera capture or
// JPEG re-encoding occurs in the writer. Timestamp subtitles are deliberately
// disabled for sparse files so long gaps do not create thousands of subtitle
// blocks. DateUTC still records the first frame's wall-clock time when valid.
bool mkvStartSparseJpeg(
    const String &fullpath,
    uint16_t width,
    uint16_t height,
    time_t startEpoch,
    uint32_t nominalFrameDurationMs
);

bool mkvAddSparseJpeg(
    const uint8_t *jpegData,
    size_t jpegSize,
    uint16_t width,
    uint16_t height,
    uint64_t relativeTimestampMs
);

bool mkvEnd();

bool mkvIsOpen();
bool mkvIsHealthy();
bool mkvHitSizeLimit();
uint32_t mkvGetFrameCount();
uint64_t mkvGetBytesWritten();
