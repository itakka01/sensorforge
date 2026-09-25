#pragma once

#include <Arduino.h>
#include "audio_capture.h"

struct AudioWavResult {
    uint64_t pcmBytes;
    uint32_t captureMs;
    int32_t peakAbs16;
    float rms16;
    uint64_t droppedBytes;
    size_t bufferCapacity;
    size_t bufferHighWater;
};

typedef void (*AudioWavServiceCallback)();

// Diagnostic WAV capture. This is deliberately separate from AVI/MKV in v47;
// it exercises the generic microphone + PSRAM path without modifying the proven
// video writers. The output uses RecordingStorageFile so the existing optional
// SFENC1 encryption policy can be tested too.
bool audioWavRecordTest(
    const String &path,
    uint32_t durationMs,
    const AudioFormat &format,
    bool encrypt,
    AudioWavResult &result,
    String &error,
    AudioWavServiceCallback serviceCallback = nullptr
);
