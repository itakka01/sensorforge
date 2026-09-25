#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t RECORDING_LOAD_SLOW_FRAME_COUNT = 10;

struct RecordingLoadSlowFrame {
    uint32_t frameCall;
    uint32_t totalUs;
    bool stageBreakdownValid;
    uint32_t cameraUs;
    uint32_t headerUs;
    uint32_t audioReadUs;
    uint32_t audioWriteUs;
    uint32_t clusterUs;
    uint32_t subtitleUs;
    uint32_t videoWriteUs;
    uint32_t imageAnalysisUs;
    uint32_t otherUs;

    bool storageIoValid;
    uint32_t storageWriteCalls;
    uint64_t storageWriteBytes;
    uint64_t storageWriteTotalUs;
    uint32_t storageWriteMaxUs;
    uint32_t storageWriteMaxBytes;
    uint32_t storageSlowWriteCalls;
    uint32_t storageSeekCalls;
    uint64_t storageSeekTotalUs;
    uint32_t storageSeekMaxUs;
};

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

    uint8_t slowFrameCount;
    RecordingLoadSlowFrame slowFrames[RECORDING_LOAD_SLOW_FRAME_COUNT];
};

// Long-running load tests are state-machine driven from the normal firmware
// loop. HTTP starts the test and then polls status; no hour-long request is kept
// open and no separate recorder task changes the production scheduling model.
bool recordingLoadTestStart(
    uint32_t durationMs,
    String &error
);

void recordingLoadTestLoop();

bool recordingLoadTestIsActive();
bool recordingLoadTestHasResult();

bool recordingLoadTestGetSnapshot(
    RecordingLoadTestResult &result,
    String &error,
    bool &active,
    bool &resultReady
);

bool recordingLoadTestRequestAbort(
    String &error
);

// Called synchronously by the firmware thermal-emergency path before storage
// is taken offline. The normal emergency recording-start block remains set.
void recordingLoadTestEmergencyStop();
