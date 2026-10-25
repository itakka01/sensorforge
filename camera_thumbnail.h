#pragma once

#include <Arduino.h>

// Downscaled JPEG copy of a camera JPEG for overview pages. esp_jpeg decodes
// directly at 1/2, 1/4 or 1/8 (the full image is never decoded); the largest
// factor that keeps at least minWidth pixels is used; jpeg_mini_encoder.h
// re-encodes it (no static internal RAM). Buffers live in PSRAM.
// On success out is a PSRAM buffer the caller frees with heap_caps_free().
// False when no factor fits (image already small), PSRAM is missing or the
// decode/encode fails. Runs in the caller's task (~0.1-0.2 s).
bool cameraMakeThumbnail(const uint8_t *jpeg, size_t length, uint16_t width, uint16_t height,
                         uint16_t minWidth, uint8_t *&out, size_t &outLength);
