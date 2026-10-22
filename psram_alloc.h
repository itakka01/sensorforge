#pragma once
// Explicit external-RAM (PSRAM) allocation for large buffers that are neither
// DMA targets, ISR/callback data nor time critical. Moving such buffers out of
// .bss frees the same amount of internal DRAM for the internal heap.
//
// Never falls back to internal RAM: if PSRAM is missing or exhausted the caller
// gets nullptr and must report the error instead of draining the last internal
// reserve. (Plain malloc() would place requests <= 4096 bytes internally,
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL.)
#include <esp_heap_caps.h>
#include <stddef.h>
#include <stdint.h>

inline void *sfPsramCalloc(size_t count, size_t size)
{
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Scoped temporary PSRAM buffer, freed when it leaves scope. Not copyable.
class SfPsramBuffer {
public:
    explicit SfPsramBuffer(size_t size)
        : data_(static_cast<uint8_t *>(sfPsramCalloc(1, size))), size_(data_ ? size : 0) {}
    ~SfPsramBuffer() { heap_caps_free(data_); }
    SfPsramBuffer(const SfPsramBuffer &) = delete;
    SfPsramBuffer &operator=(const SfPsramBuffer &) = delete;
    explicit operator bool() const { return data_ != nullptr; }
    uint8_t *data() const { return data_; }
    size_t size() const { return size_; }
private:
    uint8_t *data_;
    size_t size_;
};
