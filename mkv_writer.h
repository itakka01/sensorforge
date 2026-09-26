#pragma once

#include <Arduino.h>
#include <time.h>

#include "recording_write_buffer.h"

struct MkvFrameTiming {
    bool valid;
    uint32_t cameraUs;
    uint32_t headerUs;
    uint32_t audioReadUs;
    uint32_t audioWriteUs;
    uint32_t clusterUs;
    uint32_t subtitleUs;
    uint32_t videoWriteUs;
    uint32_t imageAnalysisUs;
};

void mkvStart(const String &fullpath, int fps);
void mkvAddFrame();

// Detailed stage timing is disabled during normal production recordings.
// The explicit Recording Load Test enables it temporarily so diagnostic timer
// calls do not become permanent per-frame overhead.
void mkvSetDetailedFrameTimingEnabled(bool enabled);
bool mkvGetLastFrameTiming(MkvFrameTiming &timing);

// PSRAM write-behind statistics for diagnostics and normal recording summaries.
bool mkvGetWriteBufferStats(RecordingWriteBufferStats &stats);
bool mkvWriteBehindEnabled();

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
const char *mkvGetLastError();
bool mkvHitSizeLimit();
uint32_t mkvGetFrameCount();
uint64_t mkvGetBytesWritten();
