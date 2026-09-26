#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "recording_storage.h"

enum RecordingWriteBufferInitStatus : uint8_t {
    RECORDING_WRITE_BUFFER_NOT_ATTEMPTED = 0,
    RECORDING_WRITE_BUFFER_READY,
    RECORDING_WRITE_BUFFER_NO_PSRAM,
    RECORDING_WRITE_BUFFER_RING_ALLOC_FAILED,
    RECORDING_WRITE_BUFFER_SCRATCH_ALLOC_FAILED,
    RECORDING_WRITE_BUFFER_MUTEX_ALLOC_FAILED,
    RECORDING_WRITE_BUFFER_TASK_CREATE_FAILED
};

const char *recordingWriteBufferInitStatusName(
    RecordingWriteBufferInitStatus status
);

struct RecordingWriteBufferStats {
    bool enabled;
    bool failed;
    RecordingWriteBufferInitStatus initStatus;
    size_t capacity;
    size_t queuedBytes;
    size_t highWater;
    uint64_t bytesQueued;
    uint64_t bytesCommitted;
    uint32_t producerWaitCount;
    uint64_t producerWaitUs;
    uint32_t drainWriteCalls;
    uint64_t drainWriteTotalUs;
    uint32_t drainWriteMaxUs;
    uint32_t drainWriteMaxBytes;
    uint32_t slowDrainWriteCalls;
};

// Sequential PSRAM write-behind layer for recording containers.
//
// The producer sees logical append semantics immediately. A low-priority task
// drains queued logical bytes through the existing RecordingStorageFile, so the
// same path is used for plain and SFENC1-encrypted media. Any seek/flush/finalize
// first drains the queue completely, preserving the existing random-access
// container finalization semantics.
class RecordingWriteBufferedFile {
public:
    RecordingWriteBufferedFile();
    ~RecordingWriteBufferedFile();

    RecordingWriteBufferedFile(const RecordingWriteBufferedFile &) = delete;
    RecordingWriteBufferedFile &operator=(const RecordingWriteBufferedFile &) = delete;

    bool openWrite(const String &path, bool encrypt);
    size_t write(const uint8_t *buffer, size_t length);
    bool seek(uint32_t position);
    size_t position() const;
    size_t size() const;
    void flush();
    bool closeChecked();
    void close();

    bool isOpen() const;
    bool failed() const;
    const char *lastError() const;
    bool writeBehindEnabled() const;
    RecordingWriteBufferStats stats() const;

    explicit operator bool() const;

private:
    static void drainTaskThunk(void *arg);
    void drainTask();

    bool prepareWriteBehind();
    bool startDrainTask();
    bool waitUntilDrained(uint32_t timeoutMs = 15000UL);
    bool stopDrainTask(uint32_t timeoutMs = 15000UL);
    void releaseWriteBehind();
    size_t writeSynchronous(const uint8_t *buffer, size_t length);

    RecordingStorageFile file_;

    uint8_t *ring_ = nullptr;
    uint8_t *drainScratch_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    TaskHandle_t taskHandle_ = nullptr;

    volatile bool stopRequested_ = false;
    volatile bool asyncFailed_ = false;
    volatile bool taskRunning_ = false;

    size_t capacity_ = 0;
    size_t writePos_ = 0;
    size_t readPos_ = 0;
    size_t queued_ = 0;
    size_t highWater_ = 0;

    uint64_t logicalPosition_ = 0;
    uint64_t logicalSize_ = 0;

    uint64_t bytesQueued_ = 0;
    uint64_t bytesCommitted_ = 0;
    uint32_t producerWaitCount_ = 0;
    uint64_t producerWaitUs_ = 0;
    uint32_t drainWriteCalls_ = 0;
    uint64_t drainWriteTotalUs_ = 0;
    uint32_t drainWriteMaxUs_ = 0;
    uint32_t drainWriteMaxBytes_ = 0;
    uint32_t slowDrainWriteCalls_ = 0;

    RecordingWriteBufferInitStatus initStatus_ =
        RECORDING_WRITE_BUFFER_NOT_ATTEMPTED;
    char lastError_[224] = {};
};
