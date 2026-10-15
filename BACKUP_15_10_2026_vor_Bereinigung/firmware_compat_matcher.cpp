#include "firmware_compat_matcher.h"
#include <string.h>

FirmwareCompatibilityMatcher::FirmwareCompatibilityMatcher() { reset(); }
void FirmwareCompatibilityMatcher::reset() {
    marker_ = nullptr;
    length_ = 0;
    matched_ = 0;
    memset(prefix_, 0, sizeof(prefix_));
    found_ = false;
}
bool FirmwareCompatibilityMatcher::begin(const char *marker) {
    reset();
    if (!marker || !marker[0]) return false;
    const size_t n = strnlen(marker, MAX_MARKER_SIZE + 1U);
    if (!n || n > MAX_MARKER_SIZE) return false;
    marker_ = marker;
    length_ = n;
    for (size_t i=1, j=0; i<n; ++i) {
        while (j && marker_[i] != marker_[j]) j = prefix_[j-1];
        if (marker_[i] == marker_[j]) ++j;
        prefix_[i] = j;
    }
    return true;
}
void FirmwareCompatibilityMatcher::feed(const uint8_t *chunk, size_t length) {
    if (found_ || !marker_ || !chunk || !length) return;
    for (size_t i=0; i<length; ++i) {
        const char c = static_cast<char>(chunk[i]);
        while (matched_ && c != marker_[matched_]) matched_ = prefix_[matched_-1];
        if (c == marker_[matched_]) ++matched_;
        if (matched_ == length_) { found_ = true; return; }
    }
}
