#pragma once

#include <Arduino.h>
#include <atomic>

#include "recording_write_buffer.h"

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

// =============================================================
// REFERENCE-COUNTED RECORDING BLOCK (Thread-Safe)
// =============================================================

// Internal reference counter for recording block depth.
// Do NOT access directly - use RecordingBlockGuard instead.
extern std::atomic<int> g_recordingBlockDepth;

// Global recording-start lock.
// Set to true from any module that must temporarily prevent a NEW recording
// from starting. An already running recording is not interrupted.
//
// SAFETY: Changed from volatile bool to std::atomic<bool>. The lock state
// is now automatically managed by g_recordingBlockDepth reference counter.
extern std::atomic<bool> g_recordingStartBlocked;

// =============================================================
// RAII RECORDING BLOCK GUARD (Preferred API)
// =============================================================

// Thread-safe RAII guard for blocking recording starts.
// Automatically increments reference counter on construction,
// decrements on destruction. Recording is unblocked only when
// depth reaches zero (all guards released).
//
// Usage:
//   {
//       RecordingBlockGuard block;
//       // ... recording starts are blocked here ...
//   }  // automatic unblock
//
class RecordingBlockGuard {
public:
    RecordingBlockGuard();
    ~RecordingBlockGuard();

    // Non-copyable, non-movable (RAII pattern)
    RecordingBlockGuard(const RecordingBlockGuard&) = delete;
    RecordingBlockGuard& operator=(const RecordingBlockGuard&) = delete;
    RecordingBlockGuard(RecordingBlockGuard&&) = delete;
    RecordingBlockGuard& operator=(RecordingBlockGuard&&) = delete;
};

// Initialize RAII guard mutex - call from setup() before any Guard usage
void initRecordingBlockGuard();

// Legacy direct access (deprecated - use RecordingBlockGuard instead)

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
bool recorderGetWriteBufferStats(RecordingWriteBufferStats &stats);
String recorderGetLastError();

String recorderGetFormat();
String recorderGetFinalPath();
