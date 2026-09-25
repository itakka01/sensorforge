#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// Result of the explicit production-path recording load test.
// The test uses the currently saved recording configuration and the normal
// camera -> recorder/container -> RecordingStorageFile -> SD path. It is
// intentionally separate from normal motion automation and deletes its test
// media after measurement.
struct RecordingLoadTestResult {
    bool completed;
    bool finalized;
    bool recorderHealthy;
    bool encrypted;
    bool audioRequested;
    bool audioActive;

    String format;

    uint32_t requestedDurationMs;
    uint32_t elapsedMs;
    uint32_t targetFps;
    uint32_t frameBudgetUs;
    uint32_t frameCalls;
    uint32_t framesWritten;
    uint32_t nearBudgetFrames;
    uint32_t overBudgetFrames;
    uint32_t averageCallUs;
    uint32_t p95CallUs;
    uint32_t p99CallUs;
    uint32_t worstCallUs;

    uint64_t mediaBytesBeforeFinalize;
    uint32_t finalizeMs;

    uint64_t audioBytesCaptured;
    uint64_t audioBytesDelivered;
    uint64_t audioBytesDropped;
    size_t audioBufferCapacity;
    size_t audioBufferHighWater;

    uint32_t internalHeapBefore;
    uint32_t internalHeapMin;
    uint32_t internalHeapAfter;
    uint32_t psramBefore;
    uint32_t psramMin;
    uint32_t psramAfter;

    float cpuTempStartC;
    float cpuTempMaxC;
    float cpuTempEndC;
    bool thermalWarningSeen;
};

bool recordingLoadTestRun(
    uint32_t durationMs,
    RecordingLoadTestResult &result,
    String &error
);
