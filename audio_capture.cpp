#include "audio_capture.h"

#include "board_config.h"
#include "config.h"

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
    const AudioInputSettings &settings,
    const AudioFormat &format,
    String &error
)
{
    audioI2S.setPort(I2S_NUM_0);
    audioI2S.setTimeout(150);

    if (settings.backend == AUDIO_BACKEND_PDM) {
        audioI2S.setPinsPdmRx(
            settings.pdmClkPin,
            settings.pdmDataPin
        );

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
    }

    if (settings.backend == AUDIO_BACKEND_I2S_STD) {
        audioI2S.setPins(
            settings.i2sBclkPin,
            settings.i2sWsPin,
            -1,
            settings.i2sDataPin,
            settings.i2sMclkPin
        );

        i2s_slot_mode_t slotMode =
            format.channels == 2
            ? I2S_SLOT_MODE_STEREO
            : I2S_SLOT_MODE_MONO;

        int8_t slotMask =
            settings.i2sSlot == AUDIO_I2S_SLOT_RIGHT
            ? (int8_t)I2S_STD_SLOT_RIGHT
            : settings.i2sSlot == AUDIO_I2S_SLOT_STEREO
              ? (int8_t)I2S_STD_SLOT_BOTH
              : (int8_t)I2S_STD_SLOT_LEFT;

        if (!audioI2S.begin(
                I2S_MODE_STD,
                format.sampleRate,
                I2S_DATA_BIT_WIDTH_16BIT,
                slotMode,
                slotMask
            )) {
            error = "standard I2S initialization failed";
            return false;
        }

        i2sStarted = true;
        return true;
    }

    error = "no audio capture backend configured";
    return false;
}


} // namespace


bool audioCaptureConfiguredInput(
    AudioInputSettings &settings,
    String &error
)
{
    error = "";
    settings = {};
    settings.source = AUDIO_SOURCE_BOARD_DEFAULT;
    settings.backend = AUDIO_BACKEND_NONE;
    settings.pdmClkPin = -1;
    settings.pdmDataPin = -1;
    settings.i2sBclkPin = -1;
    settings.i2sWsPin = -1;
    settings.i2sDataPin = -1;
    settings.i2sMclkPin = -1;
    settings.i2sSlot = AUDIO_I2S_SLOT_LEFT;

    if (cfg_audio_source == "external") {
        if (!cfg_audio_expert_mode) {
            error = "external audio source requires expert mode";
            return false;
        }

        settings.source = AUDIO_SOURCE_EXTERNAL;

        if (cfg_audio_backend == "pdm") {
            settings.backend = AUDIO_BACKEND_PDM;
            settings.pdmClkPin = (int8_t)cfg_audio_pdm_clk_pin;
            settings.pdmDataPin = (int8_t)cfg_audio_pdm_data_pin;
            return true;
        }

        if (cfg_audio_backend == "i2s") {
            settings.backend = AUDIO_BACKEND_I2S_STD;
            settings.i2sBclkPin = (int8_t)cfg_audio_i2s_bclk_pin;
            settings.i2sWsPin = (int8_t)cfg_audio_i2s_ws_pin;
            settings.i2sDataPin = (int8_t)cfg_audio_i2s_data_pin;
            settings.i2sMclkPin = (int8_t)cfg_audio_i2s_mclk_pin;

            if (cfg_audio_i2s_slot == "right") {
                settings.i2sSlot = AUDIO_I2S_SLOT_RIGHT;
            } else if (cfg_audio_i2s_slot == "stereo") {
                settings.i2sSlot = AUDIO_I2S_SLOT_STEREO;
            } else {
                settings.i2sSlot = AUDIO_I2S_SLOT_LEFT;
            }

            return true;
        }

        error = "unsupported external audio backend";
        return false;
    }

#if BOARD_HAS_INTEGRATED_MIC
#if defined(BOARD_INTEGRATED_MIC_BACKEND_PDM)
    settings.source = AUDIO_SOURCE_BOARD_DEFAULT;
    settings.backend = AUDIO_BACKEND_PDM;
    settings.pdmClkPin =
        (int8_t)BOARD_INTEGRATED_MIC_PDM_CLK_PIN;
    settings.pdmDataPin =
        (int8_t)BOARD_INTEGRATED_MIC_PDM_DATA_PIN;
    return true;
#else
#error "BOARD_HAS_INTEGRATED_MIC requires a supported integrated mic backend"
#endif
#else
    error = "this board has no integrated microphone";
    return false;
#endif
}


const char *audioCaptureBackendName(
    const AudioInputSettings &settings
)
{
    if (settings.source == AUDIO_SOURCE_BOARD_DEFAULT) {
#if BOARD_HAS_INTEGRATED_MIC
        return BOARD_INTEGRATED_MIC_NAME;
#else
        return "No integrated microphone";
#endif
    }

    if (settings.backend == AUDIO_BACKEND_PDM)
        return "External PDM microphone";

    if (settings.backend == AUDIO_BACKEND_I2S_STD)
        return "External I2S microphone";

    return "No audio input";
}


AudioCaptureCapabilities audioCaptureCapabilities(
    const AudioInputSettings &settings
)
{
    AudioCaptureCapabilities caps = {};
    caps.backend = settings.backend;
    caps.available = settings.backend != AUDIO_BACKEND_NONE;

    if (!caps.available)
        return caps;

    if (
        settings.source == AUDIO_SOURCE_BOARD_DEFAULT &&
        settings.backend == AUDIO_BACKEND_PDM
    ) {
#if BOARD_HAS_INTEGRATED_MIC && defined(BOARD_INTEGRATED_MIC_BACKEND_PDM)
        caps.minSampleRate =
            BOARD_INTEGRATED_MIC_MIN_SAMPLE_RATE_HZ;
        caps.maxSampleRate =
            BOARD_INTEGRATED_MIC_MAX_SAMPLE_RATE_HZ;
        caps.recommendedSampleRate =
            BOARD_INTEGRATED_MIC_RECOMMENDED_SAMPLE_RATE_HZ;
        caps.supports16Bit = true;
        caps.supports24Bit = false;
        caps.supports32Bit = false;
        caps.supportsMono = true;
        caps.supportsStereo = false;
#else
        caps.available = false;
        caps.backend = AUDIO_BACKEND_NONE;
#endif
        return caps;
    }

    if (settings.backend == AUDIO_BACKEND_PDM) {
        caps.minSampleRate = 8000UL;
        caps.maxSampleRate = 48000UL;
        caps.recommendedSampleRate = 16000UL;
        caps.supports16Bit = true;
        caps.supports24Bit = false;
        caps.supports32Bit = false;
        caps.supportsMono = true;
        caps.supportsStereo = false;
        return caps;
    }

    if (settings.backend == AUDIO_BACKEND_I2S_STD) {
        caps.minSampleRate = 8000UL;
        caps.maxSampleRate = 96000UL;
        caps.recommendedSampleRate = 16000UL;
        caps.supports16Bit = true;
        caps.supports24Bit = false;
        caps.supports32Bit = false;
        caps.supportsMono = true;
        caps.supportsStereo = true;
        return caps;
    }

    caps.available = false;
    caps.backend = AUDIO_BACKEND_NONE;
    return caps;
}


bool audioCaptureHardwareAvailable()
{
    AudioInputSettings settings;
    String error;

    return
        audioCaptureConfiguredInput(settings, error) &&
        audioCaptureCapabilities(settings).available;
}


AudioBackendKind audioCaptureBackendKind()
{
    AudioInputSettings settings;
    String error;

    if (!audioCaptureConfiguredInput(settings, error))
        return AUDIO_BACKEND_NONE;

    return settings.backend;
}


const char *audioCaptureBackendName()
{
    AudioInputSettings settings;
    String error;

    if (!audioCaptureConfiguredInput(settings, error))
        return "No audio input";

    return audioCaptureBackendName(settings);
}


AudioCaptureCapabilities audioCaptureCapabilities()
{
    AudioInputSettings settings;
    String error;

    if (!audioCaptureConfiguredInput(settings, error)) {
        AudioCaptureCapabilities caps = {};
        caps.backend = AUDIO_BACKEND_NONE;
        caps.available = false;
        return caps;
    }

    return audioCaptureCapabilities(settings);
}


bool audioCaptureFormatSupported(
    const AudioInputSettings &settings,
    const AudioFormat &format,
    String &error
)
{
    error = "";

    AudioCaptureCapabilities caps =
        audioCaptureCapabilities(settings);

    if (!caps.available) {
        error = "no audio input configured";
        return false;
    }

    if (settings.backend == AUDIO_BACKEND_PDM) {
        if (
            settings.pdmClkPin < 0 ||
            settings.pdmDataPin < 0 ||
            settings.pdmClkPin == settings.pdmDataPin
        ) {
            error = "invalid PDM pin configuration";
            return false;
        }
    }

    if (settings.backend == AUDIO_BACKEND_I2S_STD) {
        if (
            settings.i2sBclkPin < 0 ||
            settings.i2sWsPin < 0 ||
            settings.i2sDataPin < 0 ||
            settings.i2sBclkPin == settings.i2sWsPin ||
            settings.i2sBclkPin == settings.i2sDataPin ||
            settings.i2sWsPin == settings.i2sDataPin ||
            (
                settings.i2sMclkPin >= 0 &&
                (
                    settings.i2sMclkPin == settings.i2sBclkPin ||
                    settings.i2sMclkPin == settings.i2sWsPin ||
                    settings.i2sMclkPin == settings.i2sDataPin
                )
            )
        ) {
            error = "invalid I2S pin configuration";
            return false;
        }

        if (
            format.channels == 2 &&
            settings.i2sSlot != AUDIO_I2S_SLOT_STEREO
        ) {
            error = "stereo I2S requires stereo slot selection";
            return false;
        }

        if (
            format.channels == 1 &&
            settings.i2sSlot == AUDIO_I2S_SLOT_STEREO
        ) {
            error = "mono I2S requires left or right slot selection";
            return false;
        }
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


bool audioCaptureFormatSupported(
    const AudioFormat &format,
    String &error
)
{
    AudioInputSettings settings;

    if (!audioCaptureConfiguredInput(settings, error))
        return false;

    return audioCaptureFormatSupported(
        settings,
        format,
        error
    );
}


bool audioCaptureStart(
    const AudioInputSettings &settings,
    const AudioFormat &format,
    String &error
)
{
    error = "";

    if (captureRunning || captureTaskHandle) {
        error = "audio capture is already running";
        return false;
    }

    if (!audioCaptureFormatSupported(
            settings,
            format,
            error
        )) {
        return false;
    }

    releaseBuffers();
    resetStats();

    if (!allocateBuffers(format, error))
        return false;

    if (!beginBackend(
            settings,
            format,
            error
        )) {
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



bool audioCaptureStart(
    const AudioFormat &format,
    String &error
)
{
    AudioInputSettings settings;

    if (!audioCaptureConfiguredInput(settings, error))
        return false;

    return audioCaptureStart(
        settings,
        format,
        error
    );
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
