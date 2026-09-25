#include "audio_capture.h"

#include "board_config.h"

#include <ESP_I2S.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

namespace {

static I2SClass audioI2S(I2S_NUM_0);

static uint8_t *ringBuffer = nullptr;
static size_t ringCapacity = 0;
static volatile size_t ringHead = 0;
static volatile size_t ringTail = 0;

static uint8_t *captureScratch = nullptr;
static size_t captureScratchBytes = 0;

static TaskHandle_t captureTaskHandle = nullptr;
static volatile bool captureRunning = false;
static volatile bool captureStopRequested = false;
static bool i2sStarted = false;

static AudioFormat activeFormat = {0, 0, 0};

static portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t statBytesCaptured = 0;
static uint64_t statBytesDelivered = 0;
static uint64_t statBytesDropped = 0;
static size_t statBufferHighWater = 0;

static const size_t AUDIO_RING_MIN_BYTES = 256U * 1024U;
static const size_t AUDIO_RING_MAX_BYTES = 1024U * 1024U;
static const size_t AUDIO_CAPTURE_SCRATCH_BYTES = 4096U;

static size_t ringAvailableUnsafe()
{
    size_t head = ringHead;
    size_t tail = ringTail;

    if (head >= tail)
        return head - tail;

    return ringCapacity - (tail - head);
}

static size_t ringFreeUnsafe()
{
    if (ringCapacity < 2U)
        return 0;

    return ringCapacity - ringAvailableUnsafe() - 1U;
}

static size_t ringWrite(
    const uint8_t *data,
    size_t length
)
{
    if (!ringBuffer || ringCapacity < 2U || !data || length == 0)
        return 0;

    size_t freeBytes = ringFreeUnsafe();
    size_t toWrite = length < freeBytes ? length : freeBytes;

    if (toWrite == 0)
        return 0;

    size_t head = ringHead;
    size_t first = toWrite;

    if (head + first > ringCapacity)
        first = ringCapacity - head;

    memcpy(ringBuffer + head, data, first);

    size_t second = toWrite - first;

    if (second)
        memcpy(ringBuffer, data + first, second);

    __sync_synchronize();
    ringHead = (head + toWrite) % ringCapacity;

    size_t available = ringAvailableUnsafe();

    portENTER_CRITICAL(&statsMux);
    if (available > statBufferHighWater)
        statBufferHighWater = available;
    portEXIT_CRITICAL(&statsMux);

    return toWrite;
}

static size_t ringRead(
    uint8_t *data,
    size_t length
)
{
    if (!ringBuffer || ringCapacity < 2U || !data || length == 0)
        return 0;

    size_t available = ringAvailableUnsafe();
    size_t toRead = length < available ? length : available;

    if (toRead == 0)
        return 0;

    size_t tail = ringTail;
    size_t first = toRead;

    if (tail + first > ringCapacity)
        first = ringCapacity - tail;

    memcpy(data, ringBuffer + tail, first);

    size_t second = toRead - first;

    if (second)
        memcpy(data + first, ringBuffer, second);

    __sync_synchronize();
    ringTail = (tail + toRead) % ringCapacity;

    return toRead;
}

static void resetStats()
{
    portENTER_CRITICAL(&statsMux);
    statBytesCaptured = 0;
    statBytesDelivered = 0;
    statBytesDropped = 0;
    statBufferHighWater = 0;
    portEXIT_CRITICAL(&statsMux);
}

static void releaseBuffers()
{
    if (captureScratch) {
        free(captureScratch);
        captureScratch = nullptr;
        captureScratchBytes = 0;
    }

    if (ringBuffer) {
        free(ringBuffer);
        ringBuffer = nullptr;
        ringCapacity = 0;
    }

    ringHead = 0;
    ringTail = 0;
}

static bool allocateBuffers(
    const AudioFormat &format,
    String &error
)
{
    uint64_t bytesPerSecond =
        (uint64_t)format.sampleRate *
        (uint64_t)format.channels *
        (uint64_t)(format.bitsPerSample / 8U);

    uint64_t wanted64 = bytesPerSecond * 2ULL + 1ULL;

    size_t wanted =
        wanted64 > AUDIO_RING_MAX_BYTES
        ? AUDIO_RING_MAX_BYTES
        : (size_t)wanted64;

    if (wanted < AUDIO_RING_MIN_BYTES)
        wanted = AUDIO_RING_MIN_BYTES;

    ringBuffer =
        (uint8_t *)heap_caps_malloc(
            wanted,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (!ringBuffer) {
        error =
            "audio PSRAM ring allocation failed (" +
            String((unsigned long)wanted) +
            " bytes)";
        return false;
    }

    ringCapacity = wanted;
    ringHead = 0;
    ringTail = 0;

    captureScratch =
        (uint8_t *)heap_caps_malloc(
            AUDIO_CAPTURE_SCRATCH_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );

    if (!captureScratch) {
        error = "audio internal capture buffer allocation failed";
        releaseBuffers();
        return false;
    }

    captureScratchBytes = AUDIO_CAPTURE_SCRATCH_BYTES;
    return true;
}

static void captureTask(void *)
{
    while (!captureStopRequested) {
        size_t got =
            audioI2S.readBytes(
                (char *)captureScratch,
                captureScratchBytes
            );

        if (got == 0) {
            taskYIELD();
            continue;
        }

        portENTER_CRITICAL(&statsMux);
        statBytesCaptured += (uint64_t)got;
        portEXIT_CRITICAL(&statsMux);

        size_t stored = ringWrite(captureScratch, got);

        if (stored < got) {
            portENTER_CRITICAL(&statsMux);
            statBytesDropped += (uint64_t)(got - stored);
            portEXIT_CRITICAL(&statsMux);
        }
    }

    captureRunning = false;
    captureTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

static bool beginBackend(
    const AudioFormat &format,
    String &error
)
{
#if defined(AUDIO_INPUT_BACKEND_PDM)

    audioI2S.setPort(I2S_NUM_0);
    audioI2S.setPinsPdmRx(
        (int8_t)AUDIO_PDM_CLK_PIN,
        (int8_t)AUDIO_PDM_DATA_PIN
    );
    audioI2S.setTimeout(150);

    if (!audioI2S.begin(
            I2S_MODE_PDM_RX,
            format.sampleRate,
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO
        )) {
        error = "PDM I2S initialization failed";
        return false;
    }

    i2sStarted = true;
    return true;

#else

    (void)format;
    error = "no audio capture backend configured for this board";
    return false;

#endif
}

} // namespace


bool audioCaptureHardwareAvailable()
{
#if defined(AUDIO_INPUT_BACKEND_PDM)
    return true;
#else
    return false;
#endif
}


AudioBackendKind audioCaptureBackendKind()
{
#if defined(AUDIO_INPUT_BACKEND_PDM)
    return AUDIO_BACKEND_PDM;
#else
    return AUDIO_BACKEND_NONE;
#endif
}


const char *audioCaptureBackendName()
{
#ifdef AUDIO_INPUT_NAME
    return AUDIO_INPUT_NAME;
#elif defined(AUDIO_INPUT_BACKEND_PDM)
    return "PDM microphone";
#else
    return "No audio input";
#endif
}


AudioCaptureCapabilities audioCaptureCapabilities()
{
    AudioCaptureCapabilities caps = {};
    caps.backend = audioCaptureBackendKind();
    caps.available = audioCaptureHardwareAvailable();

#if defined(AUDIO_INPUT_BACKEND_PDM)
    caps.minSampleRate = AUDIO_INPUT_MIN_SAMPLE_RATE_HZ;
    caps.maxSampleRate = AUDIO_INPUT_MAX_SAMPLE_RATE_HZ;
    caps.recommendedSampleRate = AUDIO_INPUT_RECOMMENDED_SAMPLE_RATE_HZ;
    caps.supports16Bit = true;
    caps.supports24Bit = false;
    caps.supports32Bit = false;
    caps.supportsMono = true;
    caps.supportsStereo = false;
#endif

    return caps;
}


bool audioCaptureFormatSupported(
    const AudioFormat &format,
    String &error
)
{
    error = "";

    AudioCaptureCapabilities caps = audioCaptureCapabilities();

    if (!caps.available) {
        error = "no audio input configured for this board";
        return false;
    }

    if (
        format.sampleRate < caps.minSampleRate ||
        format.sampleRate > caps.maxSampleRate
    ) {
        error =
            "sample rate outside backend range " +
            String((unsigned long)caps.minSampleRate) +
            ".." +
            String((unsigned long)caps.maxSampleRate) +
            " Hz";
        return false;
    }

    bool bitsSupported =
        (format.bitsPerSample == 16 && caps.supports16Bit) ||
        (format.bitsPerSample == 24 && caps.supports24Bit) ||
        (format.bitsPerSample == 32 && caps.supports32Bit);

    if (!bitsSupported) {
        error = "bit depth is not supported by this audio backend";
        return false;
    }

    bool channelsSupported =
        (format.channels == 1 && caps.supportsMono) ||
        (format.channels == 2 && caps.supportsStereo);

    if (!channelsSupported) {
        error = "channel count is not supported by this audio backend";
        return false;
    }

    return true;
}


bool audioCaptureStart(
    const AudioFormat &format,
    String &error
)
{
    error = "";

    if (captureRunning || captureTaskHandle) {
        error = "audio capture is already running";
        return false;
    }

    if (!audioCaptureFormatSupported(format, error))
        return false;

    releaseBuffers();
    resetStats();

    if (!allocateBuffers(format, error))
        return false;

    if (!beginBackend(format, error)) {
        releaseBuffers();
        return false;
    }

    activeFormat = format;
    captureStopRequested = false;
    captureRunning = true;

    BaseType_t taskResult =
        xTaskCreatePinnedToCore(
            captureTask,
            "sf_audio_capture",
            4096,
            nullptr,
            3,
            &captureTaskHandle,
            0
        );

    if (taskResult != pdPASS) {
        captureRunning = false;
        captureTaskHandle = nullptr;
        captureStopRequested = true;

        if (i2sStarted) {
            audioI2S.end();
            i2sStarted = false;
        }

        releaseBuffers();
        error = "audio capture task creation failed";
        return false;
    }

    return true;
}


size_t audioCaptureRead(
    uint8_t *buffer,
    size_t maxBytes,
    uint32_t timeoutMs
)
{
    if (!buffer || maxBytes == 0)
        return 0;

    uint32_t start = millis();

    while (true) {
        size_t got = ringRead(buffer, maxBytes);

        if (got) {
            portENTER_CRITICAL(&statsMux);
            statBytesDelivered += (uint64_t)got;
            portEXIT_CRITICAL(&statsMux);
            return got;
        }

        if (!captureRunning && ringAvailableUnsafe() == 0)
            return 0;

        if (
            timeoutMs == 0 ||
            (uint32_t)(millis() - start) >= timeoutMs
        ) {
            return 0;
        }

        delay(1);
    }
}


void audioCaptureStop()
{
    captureStopRequested = true;

    uint32_t deadline = millis() + 1000UL;

    while (
        captureTaskHandle &&
        (int32_t)(deadline - millis()) > 0
    ) {
        delay(10);
    }

    if (captureTaskHandle) {
        // readBytes() uses a short Stream timeout, so reaching this fallback
        // should be exceptional. Stop the task before tearing down I2S.
        vTaskDelete(captureTaskHandle);
        captureTaskHandle = nullptr;
        captureRunning = false;
    }

    if (i2sStarted) {
        audioI2S.end();
        i2sStarted = false;
    }

    captureRunning = false;
    releaseBuffers();
}


bool audioCaptureIsRunning()
{
    return captureRunning;
}


AudioFormat audioCaptureActiveFormat()
{
    return activeFormat;
}


AudioCaptureStats audioCaptureStats()
{
    AudioCaptureStats stats = {};

    portENTER_CRITICAL(&statsMux);
    stats.bytesCaptured = statBytesCaptured;
    stats.bytesDelivered = statBytesDelivered;
    stats.bytesDropped = statBytesDropped;
    stats.bufferHighWater = statBufferHighWater;
    portEXIT_CRITICAL(&statsMux);

    stats.bufferCapacity = ringCapacity;
    return stats;
}


size_t audioCaptureBufferedBytes()
{
    return ringAvailableUnsafe();
}
