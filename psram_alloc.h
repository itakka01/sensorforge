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
#include <new>
#include <stddef.h>
#include <stdint.h>

inline void *sfPsramCalloc(size_t count, size_t size)
{
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Fixed-capacity table in PSRAM, allocated once and never freed. Until
// allocate() succeeds it is empty (size() == 0, begin() == end()), so
// range-for loops and size()-bounded loops simply do nothing. Converts to T*
// for indexing and pointer arithmetic. Every element is constructed
// (placement new), so types with member initializers or vtables are valid.
template <typename T>
class SfPsramArray {
public:
    bool allocate(size_t count)
    {
        if (data_)
            return true;
        T *table = static_cast<T *>(sfPsramCalloc(count, sizeof(T)));
        if (!table)
            return false;
        for (size_t i = 0; i < count; ++i)
            new (&table[i]) T();
        data_ = table;
        size_ = count;
        return true;
    }
    size_t size() const { return size_; }
    T *begin() { return data_; }
    T *end() { return data_ + size_; }
    const T *begin() const { return data_; }
    const T *end() const { return data_ + size_; }
    operator T *() { return data_; }
    operator const T *() const { return data_; }

private:
    T *data_ = nullptr;
    size_t size_ = 0;
};

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
