#pragma once

#include <stddef.h>
#include <stdint.h>

// Minimal baseline JPEG encoder (JFIF, 4:4:4, standard Annex K tables) for
// small overview thumbnails. All tables are constexpr (flash), working data is
// on the stack (< 1 KB), so unlike the esp32-camera encoder (jpge, ~7 KB
// static internal RAM) it costs no internal RAM while unused.
namespace JpegMini {

// rgb565le: width*height pixels, RGB565 low byte first (as written by
// jpg2rgb565). quality 1..100. Returns the JPEG length, 0 if capacity is too
// small or the input is invalid.
size_t encodeRgb565Le(const uint8_t *rgb565le, uint16_t width, uint16_t height, uint8_t quality,
                      uint8_t *out, size_t capacity);

} // namespace JpegMini
