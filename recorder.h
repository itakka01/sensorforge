#pragma once

#include <Arduino.h>

struct RecorderFrameTiming {
    bool valid;
    bool stageBreakdownValid;
    uint32_t totalUs;
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

// Global recording-start lock.
// Set to true from any module that must temporarily prevent a NEW recording
// from starting. An already running recording is not interrupted.
extern volatile bool g_recordingStartBlocked;

bool recorderStart(const String &fullpath, int fps);
void recorderAddFrame();

void recorderSetDetailedFrameTimingEnabled(bool enabled);
bool recorderGetLastFrameTiming(RecorderFrameTiming &timing);

// Finalize and rename <final>.part -> <final>.
bool recorderEnd();

bool recorderIsOpen();
bool recorderIsHealthy();
bool recorderHitSizeLimit();
uint32_t recorderGetFrameCount();
uint64_t recorderGetBytesWritten();

String recorderGetFormat();
String recorderGetFinalPath();
