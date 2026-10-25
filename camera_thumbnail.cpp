#include "camera_thumbnail.h"

#include "esp_heap_caps.h"
#include "img_converters.h"
#include "jpeg_mini_encoder.h"

namespace {

constexpr uint8_t kThumbQuality = 80;

} // namespace

bool cameraMakeThumbnail(const uint8_t *jpeg, size_t length, uint16_t width, uint16_t height,
                         uint16_t minWidth, uint8_t *&out, size_t &outLength)
{
    out = nullptr;
    outLength = 0;
    if (!jpeg || !length || !width || !height || !psramFound())
        return false;

    uint8_t factor = 0;
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    if (width / 8U >= minWidth) {
        factor = 8;
        scale = JPEG_IMAGE_SCALE_1_8;
    } else if (width / 4U >= minWidth) {
        factor = 4;
        scale = JPEG_IMAGE_SCALE_1_4;
    } else if (width / 2U >= minWidth) {
        factor = 2;
        scale = JPEG_IMAGE_SCALE_1_2;
    } else {
        return false;
    }

    // esp_jpeg reports scaled dimensions using integer division (image_motion.cpp).
    const uint16_t w = width / factor;
    const uint16_t h = height / factor;
    const size_t pixels = (size_t)w * h;
    uint8_t *rgb = static_cast<uint8_t *>(
        heap_caps_malloc(pixels * 2U + 512U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!rgb)
        return false;
    if (!jpg2rgb565(jpeg, length, rgb, scale)) {
        heap_caps_free(rgb);
        return false;
    }
    // jpg2rgb565 writes RGB565 low byte first, the order JpegMini reads.
    const size_t capacity = pixels * 2U + 2048U;
    uint8_t *jpegOut = static_cast<uint8_t *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jpegOut) {
        heap_caps_free(rgb);
        return false;
    }
    const size_t written = JpegMini::encodeRgb565Le(rgb, w, h, kThumbQuality, jpegOut, capacity);
    heap_caps_free(rgb);
    if (!written) {
        heap_caps_free(jpegOut);
        return false;
    }
    out = jpegOut;
    outLength = written;
    return true;
}
