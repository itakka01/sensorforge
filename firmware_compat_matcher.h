#pragma once
#include <stddef.h>
#include <stdint.h>

// Chunk-independent compatibility marker matcher. Not a signature verifier.
// Marker storage is owned by the caller and must remain valid until reset().
class FirmwareCompatibilityMatcher {
public:
    static constexpr size_t MAX_MARKER_SIZE = 96U;
    FirmwareCompatibilityMatcher();
    bool begin(const char *marker);
    void reset();
    void feed(const uint8_t *chunk, size_t length);
    bool found() const { return found_; }
private:
    const char *marker_;
    size_t length_;
    size_t matched_;
    size_t prefix_[MAX_MARKER_SIZE];
    bool found_;
};
