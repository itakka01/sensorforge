#pragma once

#include <Arduino.h>
#include <stdint.h>

// Generic SensorForge audio-capture interface.
//
// The recorder/output layer must not depend on a concrete microphone type.
// Backends translate their native bus format into packed interleaved PCM bytes
// described by AudioFormat. v47 implements the XIAO onboard PDM microphone;
// additional I2S/codec backends can be added behind this same API later.

enum AudioBackendKind : uint8_t {
    AUDIO_BACKEND_NONE = 0,
    AUDIO_BACKEND_PDM,
    AUDIO_BACKEND_I2S_STD
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

bool audioCaptureHardwareAvailable();
AudioBackendKind audioCaptureBackendKind();
const char *audioCaptureBackendName();
AudioCaptureCapabilities audioCaptureCapabilities();

bool audioCaptureFormatSupported(
    const AudioFormat &format,
    String &error
);

// Starts a background capture task. Audio is buffered in PSRAM so temporary
// SD/filesystem latency does not directly stall the microphone DMA path.
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
