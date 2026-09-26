#include "recording_write_buffer.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <string.h>

namespace {

static const size_t WRITE_BEHIND_BYTES =
    512U * 1024U;

static const size_t DRAIN_CHUNK_BYTES =
    32U * 1024U;

static const uint32_t SLOW_DRAIN_WRITE_US =
    20000UL;

static uint32_t elapsedUs(uint64_t startUs)
{
    uint64_t nowUs =
        (uint64_t)esp_timer_get_time();

    uint64_t elapsed =
        nowUs >= startUs
        ? nowUs - startUs
        : 0ULL;

    return
        elapsed > 0xFFFFFFFFULL
        ? 0xFFFFFFFFUL
        : (uint32_t)elapsed;
}

static void reportWriteBehindInitFailure(
    RecordingWriteBufferInitStatus status
)
{
    Serial.printf(
        "Recording write-behind unavailable | reason=%s | internal_free=%u | internal_largest=%u | psram_free=%u | psram_largest=%u\n",
        recordingWriteBufferInitStatusName(status),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
}

} // namespace

const char *recordingWriteBufferInitStatusName(
    RecordingWriteBufferInitStatus status
)
{
    switch (status) {
        case RECORDING_WRITE_BUFFER_READY: return "ready";
        case RECORDING_WRITE_BUFFER_NO_PSRAM: return "no_psram";
        case RECORDING_WRITE_BUFFER_RING_ALLOC_FAILED: return "ring_alloc_failed";
        case RECORDING_WRITE_BUFFER_SCRATCH_ALLOC_FAILED: return "scratch_alloc_failed";
        case RECORDING_WRITE_BUFFER_MUTEX_ALLOC_FAILED: return "mutex_alloc_failed";
        case RECORDING_WRITE_BUFFER_TASK_CREATE_FAILED: return "task_create_failed";
        default: return "not_attempted";
    }
}


RecordingWriteBufferedFile::RecordingWriteBufferedFile()
{
}


RecordingWriteBufferedFile::~RecordingWriteBufferedFile()
{
    close();
}


bool RecordingWriteBufferedFile::prepareWriteBehind()
{
    releaseWriteBehind();

    stopRequested_ = false;
    asyncFailed_ = false;
    taskRunning_ = false;
    highWater_ = 0;
    bytesQueued_ = 0;
    bytesCommitted_ = 0;
    producerWaitCount_ = 0;
    producerWaitUs_ = 0;
    drainWriteCalls_ = 0;
    drainWriteTotalUs_ = 0;
    drainWriteMaxUs_ = 0;
    drainWriteMaxBytes_ = 0;
    slowDrainWriteCalls_ = 0;
    initStatus_ = RECORDING_WRITE_BUFFER_NOT_ATTEMPTED;

    if (!psramFound()) {
        initStatus_ = RECORDING_WRITE_BUFFER_NO_PSRAM;
        reportWriteBehindInitFailure(initStatus_);
        return false;
    }

    // Reserve the large contiguous PSRAM regions before SFENC1 allocates its
    // own chunk caches. This makes encrypted and plain recording compete for
    // memory in the same deterministic order.
    ring_ = static_cast<uint8_t *>(
        heap_caps_malloc(
            WRITE_BEHIND_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        )
    );

    if (!ring_) {
        initStatus_ = RECORDING_WRITE_BUFFER_RING_ALLOC_FAILED;
        reportWriteBehindInitFailure(initStatus_);
        releaseWriteBehind();
        return false;
    }

    drainScratch_ = static_cast<uint8_t *>(
        heap_caps_malloc(
            DRAIN_CHUNK_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        )
    );

    if (!drainScratch_) {
        initStatus_ = RECORDING_WRITE_BUFFER_SCRATCH_ALLOC_FAILED;
        reportWriteBehindInitFailure(initStatus_);
        releaseWriteBehind();
        return false;
    }

    mutex_ = xSemaphoreCreateMutex();

    if (!mutex_) {
        initStatus_ = RECORDING_WRITE_BUFFER_MUTEX_ALLOC_FAILED;
        reportWriteBehindInitFailure(initStatus_);
        releaseWriteBehind();
        return false;
    }

    capacity_ = WRITE_BEHIND_BYTES;
    writePos_ = 0;
    readPos_ = 0;
    queued_ = 0;
    highWater_ = 0;

    return true;
}

bool RecordingWriteBufferedFile::startDrainTask()
{
    if (!ring_ || !drainScratch_ || !mutex_ || capacity_ == 0)
        return false;

    TaskHandle_t task = nullptr;

    BaseType_t created = xTaskCreatePinnedToCore(
        drainTaskThunk,
        "sf_recording_io",
        8192,
        this,
        1,
        &task,
        0
    );

    if (created != pdPASS || !task) {
        initStatus_ = RECORDING_WRITE_BUFFER_TASK_CREATE_FAILED;
        reportWriteBehindInitFailure(initStatus_);
        releaseWriteBehind();
        return false;
    }

    taskHandle_ = task;
    initStatus_ = RECORDING_WRITE_BUFFER_READY;
    return true;
}


void RecordingWriteBufferedFile::releaseWriteBehind()
{
    if (mutex_) {
        vSemaphoreDelete(
            mutex_
        );
        mutex_ = nullptr;
    }

    if (ring_) {
        heap_caps_free(ring_);
        ring_ = nullptr;
    }

    if (drainScratch_) {
        heap_caps_free(drainScratch_);
        drainScratch_ = nullptr;
    }

    taskHandle_ = nullptr;
    capacity_ = 0;
    writePos_ = 0;
    readPos_ = 0;
    queued_ = 0;
}


bool RecordingWriteBufferedFile::openWrite(
    const String &path,
    bool encrypt
)
{
    (void)closeChecked();
    lastError_[0] = '\0';

    // A pathological stuck drain task deliberately keeps ownership of its
    // resources. Never free/reuse those objects underneath that task.
    if (file_.isOpen() || taskHandle_) {
        snprintf(
            lastError_,
            sizeof(lastError_),
            "previous recording storage did not close cleanly"
        );
        return false;
    }

    // Reserve the large PSRAM queue before encrypted storage claims its chunk
    // caches. Failure remains non-fatal: recording falls back synchronously.
    bool writeBehindPrepared = prepareWriteBehind();

    if (!file_.openWrite(path, encrypt)) {
        const char *storageError = file_.lastError();
        snprintf(
            lastError_,
            sizeof(lastError_),
            "%s",
            storageError && storageError[0]
                ? storageError
                : "recording storage open failed"
        );

        releaseWriteBehind();
        return false;
    }

    logicalPosition_ = (uint64_t)file_.position();
    logicalSize_ = (uint64_t)file_.size();

    if (writeBehindPrepared && !startDrainTask()) {
        // startDrainTask already reported the exact reason and released the
        // prepared resources. Keep the proven synchronous path.
    }

    return true;
}


size_t RecordingWriteBufferedFile::writeSynchronous(
    const uint8_t *buffer,
    size_t length
)
{
    size_t written =
        file_.write(buffer, length);

    if (written != length) {
        const char *storageError = file_.lastError();
        snprintf(
            lastError_,
            sizeof(lastError_),
            "%s",
            storageError && storageError[0]
                ? storageError
                : "synchronous recording storage write failed"
        );
    }

    logicalPosition_ =
        (uint64_t)file_.position();
    logicalSize_ =
        (uint64_t)file_.size();

    return written;
}


size_t RecordingWriteBufferedFile::write(
    const uint8_t *buffer,
    size_t length
)
{
    if (
        !file_ ||
        failed() ||
        !buffer ||
        length == 0
    ) {
        return 0;
    }

    if (!writeBehindEnabled())
        return writeSynchronous(buffer, length);

    size_t total = 0;
    bool waitingForSpace = false;
    uint64_t waitStartUs = 0;

    while (total < length) {
        if (failed())
            break;

        size_t copied = 0;

        if (
            xSemaphoreTake(
                mutex_,
                portMAX_DELAY
            ) == pdTRUE
        ) {
            size_t freeBytes =
                capacity_ - queued_;

            if (freeBytes > 0) {
                size_t contiguous =
                    capacity_ - writePos_;

                copied =
                    length - total;

                if (copied > freeBytes)
                    copied = freeBytes;
                if (copied > contiguous)
                    copied = contiguous;

                memcpy(
                    ring_ + writePos_,
                    buffer + total,
                    copied
                );

                writePos_ =
                    (writePos_ + copied) %
                    capacity_;

                queued_ += copied;

                if (queued_ > highWater_)
                    highWater_ = queued_;

                bytesQueued_ +=
                    (uint64_t)copied;
            }

            xSemaphoreGive(
                mutex_
            );
        }

        if (copied > 0) {
            if (waitingForSpace) {
                producerWaitUs_ +=
                    (uint64_t)esp_timer_get_time() -
                    waitStartUs;
                waitingForSpace = false;
            }

            total += copied;
            logicalPosition_ +=
                (uint64_t)copied;

            if (logicalPosition_ > logicalSize_)
                logicalSize_ = logicalPosition_;

            TaskHandle_t task =
                taskHandle_;

            if (task)
                xTaskNotifyGive(task);

            continue;
        }

        if (!waitingForSpace) {
            waitingForSpace = true;
            waitStartUs =
                (uint64_t)esp_timer_get_time();
            producerWaitCount_++;
        }

        delay(1);
    }

    if (waitingForSpace) {
        producerWaitUs_ +=
            (uint64_t)esp_timer_get_time() -
            waitStartUs;
    }

    return total;
}


void RecordingWriteBufferedFile::drainTaskThunk(void *arg)
{
    RecordingWriteBufferedFile *self =
        static_cast<RecordingWriteBufferedFile *>(arg);

    if (self)
        self->drainTask();

    vTaskDelete(nullptr);
}


void RecordingWriteBufferedFile::drainTask()
{
    taskRunning_ = true;

    for (;;) {
        ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(20)
        );

        if (failed())
            break;

        // A tiny coalescing window avoids turning the EBML writer's many small
        // logical writes into equally small physical SD transactions.
        if (!stopRequested_)
            vTaskDelay(pdMS_TO_TICKS(2));

        for (;;) {
            size_t take = 0;

            if (
                xSemaphoreTake(
                    mutex_,
                    portMAX_DELAY
                ) == pdTRUE
            ) {
                if (queued_ > 0) {
                    take = queued_;
                    if (take > DRAIN_CHUNK_BYTES)
                        take = DRAIN_CHUNK_BYTES;

                    size_t contiguous =
                        capacity_ - readPos_;
                    if (take > contiguous)
                        take = contiguous;

                    memcpy(
                        drainScratch_,
                        ring_ + readPos_,
                        take
                    );
                }

                xSemaphoreGive(
                    mutex_
                );
            }

            if (take == 0)
                break;

            uint64_t startUs =
                (uint64_t)esp_timer_get_time();

            size_t written =
                file_.write(
                    drainScratch_,
                    take
                );

            uint32_t writeUs =
                elapsedUs(startUs);

            if (
                xSemaphoreTake(
                    mutex_,
                    portMAX_DELAY
                ) == pdTRUE
            ) {
                drainWriteCalls_++;
                drainWriteTotalUs_ +=
                    (uint64_t)writeUs;

                if (writeUs > drainWriteMaxUs_) {
                    drainWriteMaxUs_ = writeUs;
                    drainWriteMaxBytes_ =
                        take > 0xFFFFFFFFULL
                        ? 0xFFFFFFFFUL
                        : (uint32_t)take;
                }

                if (writeUs >= SLOW_DRAIN_WRITE_US)
                    slowDrainWriteCalls_++;

                if (written == take) {
                    readPos_ =
                        (readPos_ + take) %
                        capacity_;
                    queued_ -= take;
                    bytesCommitted_ +=
                        (uint64_t)take;
                } else {
                    const char *storageError = file_.lastError();
                    snprintf(
                        lastError_,
                        sizeof(lastError_),
                        "%s",
                        storageError && storageError[0]
                            ? storageError
                            : "asynchronous recording storage write failed"
                    );
                    asyncFailed_ = true;
                }

                xSemaphoreGive(
                    mutex_
                );
            }

            if (written != take)
                break;

            taskYIELD();
        }

        bool drained = false;

        if (
            xSemaphoreTake(
                mutex_,
                portMAX_DELAY
            ) == pdTRUE
        ) {
            drained = queued_ == 0;
            xSemaphoreGive(
                mutex_
            );
        }

        if (stopRequested_ && drained)
            break;
    }

    taskRunning_ = false;
    taskHandle_ = nullptr;
}


bool RecordingWriteBufferedFile::waitUntilDrained(
    uint32_t timeoutMs
)
{
    if (!writeBehindEnabled())
        return !failed();

    TaskHandle_t task =
        taskHandle_;

    if (task)
        xTaskNotifyGive(task);

    uint32_t startMs = millis();

    for (;;) {
        if (failed())
            return false;

        size_t queuedNow = 0;

        if (
            xSemaphoreTake(
                mutex_,
                portMAX_DELAY
            ) == pdTRUE
        ) {
            queuedNow = queued_;
            xSemaphoreGive(
                mutex_
            );
        }

        if (queuedNow == 0)
            return true;

        if (
            timeoutMs > 0 &&
            (uint32_t)(millis() - startMs) >=
                timeoutMs
        ) {
            snprintf(
                lastError_,
                sizeof(lastError_),
                "write-behind drain timeout | queued=%u | timeout_ms=%lu",
                (unsigned)queuedNow,
                (unsigned long)timeoutMs
            );
            asyncFailed_ = true;
            return false;
        }

        delay(1);
    }
}


bool RecordingWriteBufferedFile::stopDrainTask(
    uint32_t timeoutMs
)
{
    if (!taskHandle_)
        return true;

    stopRequested_ = true;

    TaskHandle_t task =
        taskHandle_;

    if (task)
        xTaskNotifyGive(task);

    uint32_t startMs = millis();

    while (taskHandle_) {
        if (
            timeoutMs > 0 &&
            (uint32_t)(millis() - startMs) >=
                timeoutMs
        ) {
            snprintf(
                lastError_,
                sizeof(lastError_),
                "write-behind task stop timeout | timeout_ms=%lu",
                (unsigned long)timeoutMs
            );
            asyncFailed_ = true;
            return false;
        }

        delay(1);
    }

    return true;
}


bool RecordingWriteBufferedFile::seek(uint32_t position)
{
    if (!file_ || failed())
        return false;

    if ((uint64_t)position > logicalSize_)
        return false;

    if (!waitUntilDrained())
        return false;

    if (!file_.seek(position))
        return false;

    logicalPosition_ =
        (uint64_t)position;

    return true;
}


size_t RecordingWriteBufferedFile::position() const
{
    return
        logicalPosition_ > SIZE_MAX
        ? SIZE_MAX
        : (size_t)logicalPosition_;
}


size_t RecordingWriteBufferedFile::size() const
{
    return
        logicalSize_ > SIZE_MAX
        ? SIZE_MAX
        : (size_t)logicalSize_;
}


void RecordingWriteBufferedFile::flush()
{
    if (!file_ || failed())
        return;

    if (!waitUntilDrained())
        return;

    file_.flush();
}


bool RecordingWriteBufferedFile::closeChecked()
{
    if (!file_.isOpen() && !taskHandle_) {
        releaseWriteBehind();
        return !failed();
    }

    bool ok = !failed();

    bool taskStopped = true;

    if (writeBehindEnabled()) {
        if (!waitUntilDrained())
            ok = false;

        taskStopped = stopDrainTask();
        if (!taskStopped)
            ok = false;
    }

    // Never close/free objects underneath a task that could still be inside
    // RecordingStorageFile::write(). A pathological stuck SD transaction is
    // already a hard recorder failure; leaving the handle owned is safer than
    // creating a use-after-free on the storage task.
    if (!taskStopped)
        return false;

    if (file_.isOpen()) {
        if (!file_.closeChecked())
            ok = false;
    }

    if (asyncFailed_)
        ok = false;

    releaseWriteBehind();

    logicalPosition_ = 0;
    logicalSize_ = 0;
    stopRequested_ = false;

    return ok;
}


void RecordingWriteBufferedFile::close()
{
    (void)closeChecked();
}


bool RecordingWriteBufferedFile::isOpen() const
{
    return file_.isOpen();
}

bool RecordingWriteBufferedFile::failed() const
{
    return
        asyncFailed_ ||
        file_.failed();
}


const char *RecordingWriteBufferedFile::lastError() const
{
    if (lastError_[0])
        return lastError_;

    return file_.lastError();
}

bool RecordingWriteBufferedFile::writeBehindEnabled() const
{
    return
        ring_ &&
        drainScratch_ &&
        mutex_ &&
        taskHandle_ &&
        capacity_ > 0;
}


RecordingWriteBufferStats RecordingWriteBufferedFile::stats() const
{
    RecordingWriteBufferStats result = {};

    result.enabled =
        ring_ &&
        drainScratch_ &&
        mutex_ &&
        capacity_ > 0;

    result.failed = failed();
    result.initStatus = initStatus_;
    result.capacity = capacity_;
    result.highWater = highWater_;
    result.bytesQueued = bytesQueued_;
    result.bytesCommitted = bytesCommitted_;
    result.producerWaitCount = producerWaitCount_;
    result.producerWaitUs = producerWaitUs_;
    result.drainWriteCalls = drainWriteCalls_;
    result.drainWriteTotalUs = drainWriteTotalUs_;
    result.drainWriteMaxUs = drainWriteMaxUs_;
    result.drainWriteMaxBytes = drainWriteMaxBytes_;
    result.slowDrainWriteCalls = slowDrainWriteCalls_;

    if (mutex_) {
        if (
            xSemaphoreTake(
                mutex_,
                pdMS_TO_TICKS(20)
            ) == pdTRUE
        ) {
            result.queuedBytes = queued_;
            xSemaphoreGive(
                mutex_
            );
        }
    }

    return result;
}


RecordingWriteBufferedFile::operator bool() const
{
    return
        (bool)file_ &&
        !failed();
}
