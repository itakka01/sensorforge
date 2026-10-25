#pragma once

#include <Arduino.h>

// Where the time of the last cameraMakeThumbnail went (for /camera_frame).
struct CameraThumbStats {
    uint32_t decodeUs = 0;
    uint32_t encodeUs = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t path = 0; // 0 none, 1 esp_new_jpeg, 2 esp_jpeg + jpeg_mini_encoder
};

// Downscaled JPEG copy of a camera JPEG for overview pages, reduced by 1/8,
// 1/4 or 1/2: the largest factor that keeps at least minWidth pixels.
// Main path: esp_new_jpeg (S3 assembler-optimized) decodes directly to the
// target size (multiples of 8) and re-encodes 4:2:0. Fallback when that
// fails: esp_jpeg scaled decode + jpeg_mini_encoder.h (no static internal
// RAM). Buffers live in PSRAM.
// On success out is a PSRAM buffer the caller frees with heap_caps_free().
// False when no factor fits (image already small), PSRAM is missing or the
// decode/encode fails. Runs in the caller's task.
bool cameraMakeThumbnail(const uint8_t *jpeg, size_t length, uint16_t width, uint16_t height,
                         uint16_t minWidth, uint8_t *&out, size_t &outLength,
                         CameraThumbStats *stats = nullptr);
