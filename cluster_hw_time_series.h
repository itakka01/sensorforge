#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cluster_hw_time_math.h"

// Coordinator-only bounded RAM history for the optional GPIO time test.
// The caller must supply all edges from a single coordinator monotonic timebase.
// No GPIO ownership, transport or drone-state transitions are performed here.
namespace ClusterHwTimeSeries {
static constexpr size_t kCapacity = 256;
struct Row {
    uint32_t sequence;
    int64_t referenceUs;
    ClusterHwTimeMath::Result offsets;
};
class History {
public:
    void clear() { size_ = 0; next_ = 0; }
    size_t size() const { return size_; }
    void add(uint32_t sequence, const ClusterHwTimeMath::Sample &sample) {
        Row &r = rows_[next_];
        r.sequence = sequence;
        r.referenceUs = sample.referenceUs;
        r.offsets = ClusterHwTimeMath::compare(sample);
        next_ = (next_ + 1) % kCapacity;
        if (size_ < kCapacity) ++size_;
    }
    // Chronological index: zero is oldest retained sample.
    bool get(size_t index, Row &out) const {
        if (index >= size_) return false;
        out = rows_[(next_ + kCapacity - size_ + index) % kCapacity];
        return true;
    }
    // First and last valid measurement for a channel, ignoring missing edges.
    bool drift(size_t channel, double &ppm) const {
        if (channel >= 3) return false;
        Row first = {}, last = {}, row = {};
        bool found = false;
        for (size_t i = 0; i < size_; ++i) {
            get(i, row);
            if (!row.offsets.valid[channel]) continue;
            if (!found) { first = row; found = true; }
            last = row;
        }
        return found && ClusterHwTimeMath::driftPpm(first.referenceUs,
            first.offsets.offsetMs[channel], last.referenceUs,
            last.offsets.offsetMs[channel], ppm);
    }
private:
    Row rows_[kCapacity] = {};
    size_t size_ = 0;
    size_t next_ = 0;
};
}
