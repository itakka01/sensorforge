#pragma once

#include <Arduino.h>
#include <stdint.h>

// Generic SensorForge audio-capture interface.
//
// The recorder/output layer must not depend on a concrete microphone type.
// Backends translate their native bus format into packed interleaved PCM bytes
// described by AudioFormat. Board-integrated and user-wired external inputs are
// resolved into the same runtime settings so recorder/container code remains
// independent of microphone transport and GPIO wiring.

enum AudioBackendKind : uint8_t {
    AUDIO_BACKEND_NONE = 0,
    AUDIO_BACKEND_PDM,
    AUDIO_BACKEND_I2S_STD
};

enum AudioSourceKind : uint8_t {
    AUDIO_SOURCE_BOARD_DEFAULT = 0,
    AUDIO_SOURCE_EXTERNAL
};

enum AudioI2SSlotKind : uint8_t {
    AUDIO_I2S_SLOT_LEFT = 0,
    AUDIO_I2S_SLOT_RIGHT,
    AUDIO_I2S_SLOT_STEREO
};

struct AudioInputSettings {
    AudioSourceKind source;
    AudioBackendKind backend;

    int8_t pdmClkPin;
    int8_t pdmDataPin;

    int8_t i2sBclkPin;
    int8_t i2sWsPin;
    int8_t i2sDataPin;
    int8_t i2sMclkPin;
    AudioI2SSlotKind i2sSlot;
};

struct AudioFormat {
    uint32_t sampleRate;
    uint16_t bitsPerSample;
    uint8_t channels;
};

struct AudioCaptureCapabilities {
    AudioBackendKind backend;
    bool available;
    uint32_t minSampleRate;
    uint32_t maxSampleRate;
    uint32_t recommendedSampleRate;
    bool supports16Bit;
    bool supports24Bit;
    bool supports32Bit;
    bool supportsMono;
    bool supportsStereo;
};

struct AudioCaptureStats {
    uint64_t bytesCaptured;
    uint64_t bytesDelivered;
    uint64_t bytesDropped;
    size_t bufferCapacity;
    size_t bufferHighWater;
};

// Resolve the persisted runtime config into one concrete audio input.
// Fixed board hardware comes from board_config.h. External wiring comes only
// from config.txt/WebConfig Expert mode.
bool audioCaptureConfiguredInput(
    AudioInputSettings &settings,
    String &error
);

bool audioCaptureHardwareAvailable();
AudioBackendKind audioCaptureBackendKind();
const char *audioCaptureBackendName();
AudioCaptureCapabilities audioCaptureCapabilities();

const char *audioCaptureBackendName(
    const AudioInputSettings &settings
);

AudioCaptureCapabilities audioCaptureCapabilities(
    const AudioInputSettings &settings
);

bool audioCaptureFormatSupported(
    const AudioInputSettings &settings,
    const AudioFormat &format,
    String &error
);

bool audioCaptureFormatSupported(
    const AudioFormat &format,
    String &error
);

// Starts a background capture task. Audio is buffered in PSRAM so temporary
// SD/filesystem latency does not directly stall the microphone DMA path.
bool audioCaptureStart(
    const AudioInputSettings &settings,
    const AudioFormat &format,
    String &error
);

bool audioCaptureStart(
    const AudioFormat &format,
    String &error
);

// Reads packed PCM bytes from the PSRAM ring buffer. Returns 0 on timeout.
size_t audioCaptureRead(
    uint8_t *buffer,
    size_t maxBytes,
    uint32_t timeoutMs
);

void audioCaptureStop();
bool audioCaptureIsRunning();
AudioFormat audioCaptureActiveFormat();
AudioCaptureStats audioCaptureStats();
size_t audioCaptureBufferedBytes();
