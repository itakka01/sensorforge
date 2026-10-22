#pragma once
// Host-only stand-in for ESP-IDF esp_heap_caps.h (used by test_psram_alloc.cpp.test).
// Records every request so tests can prove the requested capabilities and the
// absence of any internal-RAM fallback; failures can be injected.
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_DMA      (1u << 3)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
struct HostHeapStub { bool failSpiram = false; unsigned calls = 0, frees = 0, live = 0; uint32_t lastCaps = 0; };
inline HostHeapStub &hostHeapStub() { static HostHeapStub s; return s; }
inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    HostHeapStub &s = hostHeapStub(); ++s.calls; s.lastCaps = caps;
    if ((caps & MALLOC_CAP_SPIRAM) && s.failSpiram) return nullptr;
    void *p = calloc(n, size); if (p) ++s.live; return p;
}
inline void heap_caps_free(void *p) { HostHeapStub &s = hostHeapStub(); if (p) { ++s.frees; --s.live; } free(p); }
