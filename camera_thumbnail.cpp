#include "camera_thumbnail.h"

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "jpeg_mini_encoder.h"

namespace {

constexpr uint8_t kThumbQuality = 80;

uint32_t elapsedUs(int64_t since)
{
    return (uint32_t)(esp_timer_get_time() - since);
}

// esp_new_jpeg: decode scaled to w x h (multiples of 8) as RGB888, encode
// 4:2:0. Its buffers must be 16-byte aligned (jpeg_calloc_align).
bool makeWithNewJpeg(const uint8_t *jpeg, size_t length, uint16_t w, uint16_t h, uint8_t *&out,
                     size_t &outLength, CameraThumbStats &stats)
{
    int64_t t0 = esp_timer_get_time();
    jpeg_dec_config_t dc = DEFAULT_JPEG_DEC_CONFIG();
    dc.output_type = JPEG_PIXEL_FORMAT_RGB888;
    dc.scale.width = w;
    dc.scale.height = h;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&dc, &dec) != JPEG_ERR_OK || !dec)
        return false;
    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t *>(jpeg);
    io.inbuf_len = (int)length;
    jpeg_dec_header_info_t info = {};
    int rgbLength = 0;
    uint8_t *rgb = nullptr;
    bool ok = jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK &&
              jpeg_dec_get_outbuf_len(dec, &rgbLength) == JPEG_ERR_OK &&
              rgbLength == (int)w * h * 3;
    if (ok) {
        rgb = static_cast<uint8_t *>(jpeg_calloc_align(rgbLength, 16));
        ok = rgb != nullptr;
    }
    if (ok) {
        io.outbuf = rgb;
        ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
    }
    jpeg_dec_close(dec);
    stats.decodeUs = elapsedUs(t0);
    if (!ok) {
        if (rgb)
            jpeg_free_align(rgb);
        return false;
    }

    t0 = esp_timer_get_time();
    jpeg_enc_config_t ec = DEFAULT_JPEG_ENC_CONFIG();
    ec.width = w;
    ec.height = h;
    ec.src_type = JPEG_PIXEL_FORMAT_RGB888;
    ec.subsampling = JPEG_SUBSAMPLE_420;
    ec.quality = kThumbQuality;
    jpeg_enc_handle_t enc = nullptr;
    const int capacity = (int)w * h + 4096; // ~1 byte/pixel is far above q80 4:2:0
    uint8_t *jpegOut = static_cast<uint8_t *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int written = 0;
    ok = jpegOut && jpeg_enc_open(&ec, &enc) == JPEG_ERR_OK && enc;
    if (ok)
        ok = jpeg_enc_process(enc, rgb, rgbLength, jpegOut, capacity, &written) == JPEG_ERR_OK && written > 0;
    if (enc)
        jpeg_enc_close(enc);
    jpeg_free_align(rgb);
    stats.encodeUs = elapsedUs(t0);
    if (!ok) {
        if (jpegOut)
            heap_caps_free(jpegOut);
        return false;
    }
    out = jpegOut;
    outLength = (size_t)written;
    return true;
}

// Fallback: esp_jpeg (tjpgd) scaled decode to RGB565 + JpegMini (4:4:4).
bool makeWithMini(const uint8_t *jpeg, size_t length, uint16_t w, uint16_t h, esp_jpeg_image_scale_t scale,
                  uint8_t *&out, size_t &outLength, CameraThumbStats &stats)
{
    int64_t t0 = esp_timer_get_time();
    const size_t pixels = (size_t)w * h;
    uint8_t *rgb = static_cast<uint8_t *>(
        heap_caps_malloc(pixels * 2U + 512U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!rgb)
        return false;
    if (!jpg2rgb565(jpeg, length, rgb, scale)) {
        heap_caps_free(rgb);
        return false;
    }
    stats.decodeUs = elapsedUs(t0);
    t0 = esp_timer_get_time();
    // jpg2rgb565 writes RGB565 low byte first, the order JpegMini reads.
    const size_t capacity = pixels * 2U + 2048U;
    uint8_t *jpegOut = static_cast<uint8_t *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jpegOut) {
        heap_caps_free(rgb);
        return false;
    }
    const size_t written = JpegMini::encodeRgb565Le(rgb, w, h, kThumbQuality, jpegOut, capacity);
    heap_caps_free(rgb);
    stats.encodeUs = elapsedUs(t0);
    if (!written) {
        heap_caps_free(jpegOut);
        return false;
    }
    out = jpegOut;
    outLength = written;
    return true;
}

} // namespace

bool cameraMakeThumbnail(const uint8_t *jpeg, size_t length, uint16_t width, uint16_t height,
                         uint16_t minWidth, uint8_t *&out, size_t &outLength, CameraThumbStats *stats)
{
    out = nullptr;
    outLength = 0;
    CameraThumbStats local;
    CameraThumbStats &st = stats ? *stats : local;
    st = CameraThumbStats();
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

    // esp_new_jpeg needs multiples of 8 and at most 1/8; rounding down could
    // exceed 1/8, then only the fallback fits.
    const uint16_t w8 = (uint16_t)((width / factor) & ~7U);
    const uint16_t h8 = (uint16_t)((height / factor) & ~7U);
    if (w8 && h8 && (uint32_t)w8 * 8U >= width && (uint32_t)h8 * 8U >= height &&
        makeWithNewJpeg(jpeg, length, w8, h8, out, outLength, st)) {
        st.path = 1;
        st.width = w8;
        st.height = h8;
        return true;
    }

    // esp_jpeg reports scaled dimensions using integer division (image_motion.cpp).
    const uint16_t w = width / factor;
    const uint16_t h = height / factor;
    if (!makeWithMini(jpeg, length, w, h, scale, out, outLength, st))
        return false;
    st.path = 2;
    st.width = w;
    st.height = h;
    return true;
}
