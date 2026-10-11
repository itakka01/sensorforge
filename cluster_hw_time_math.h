#pragma once
#include <stdint.h>

// Pure arithmetic for hardware time-diagnostics. No GPIO, tasks, heap or
// runtime mode changes. All timestamp arguments use a single coordinator
// monotonic timebase, measured in microseconds.
namespace ClusterHwTimeMath {
struct Sample {
    int64_t referenceUs;
    int64_t nodeUs[3];
    bool present[3];
};
struct Result {
    bool valid[3];
    double offsetMs[3];          // node rising edge - coordinator reference
    bool pairValid[3];
    double pairOffsetMs[3];      // 0 = node1-node2, 1 = node1-node3, 2 = node2-node3
};
inline Result compare(const Sample &s) {
    Result result = {};
    for (int i = 0; i < 3; ++i) {
        result.valid[i] = s.present[i];
        if (s.present[i]) result.offsetMs[i] = (double)(s.nodeUs[i] - s.referenceUs) / 1000.0;
    }
    const int left[3] = {0,0,1}, right[3] = {1,2,2};
    for (int i = 0; i < 3; ++i) {
        result.pairValid[i] = s.present[left[i]] && s.present[right[i]];
        if (result.pairValid[i])
            result.pairOffsetMs[i] = (double)(s.nodeUs[left[i]] - s.nodeUs[right[i]]) / 1000.0;
    }
    return result;
}
// Two-point relative drift. Returns false for missing samples, reversed or
// equal timestamps. Positive ppm means the measured offset grows over time.
inline bool driftPpm(int64_t firstReferenceUs, double firstOffsetMs,
                     int64_t lastReferenceUs, double lastOffsetMs,
                     double &ppm) {
    if (lastReferenceUs <= firstReferenceUs) return false;
    const double elapsedMs = (double)(lastReferenceUs - firstReferenceUs) / 1000.0;
    ppm = ((lastOffsetMs - firstOffsetMs) / elapsedMs) * 1000000.0;
    return true;
}
}
