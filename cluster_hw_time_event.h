#pragma once
// Passive GPIO meter: groups the first rising edge per input channel into one
// physical event. All timestamps come from ONE local esp_timer clock of the
// measuring device; no cluster time, no coordinator reference is involved.
// Pure helpers without Arduino/ESP-IDF dependencies (host test:
// test_cluster_capture_pulse.cpp.test).
#include <stdint.h>

namespace ClusterHwTimeEvent {

constexpr int kChannels = 3;

struct Observed {
    int64_t firstRiseUs[kChannels];
    bool received[kChannels];
    uint32_t rises[kChannels];
};

struct Event {
    int64_t firstEdgeUs;              // earliest edge of the event (reference)
    int64_t edgeUs[kChannels];
    bool present[kChannels];          // edge inside the event window
    uint32_t rises[kChannels];        // all rising edges seen while armed
    uint8_t lateEdges;                // first edges outside the window (not attributed)
};

inline bool earliest(const Observed &o, int64_t &firstUs) {
    bool found = false;
    for (int i = 0; i < kChannels; ++i) {
        if (!o.received[i]) continue;
        if (!found || o.firstRiseUs[i] < firstUs) firstUs = o.firstRiseUs[i];
        found = true;
    }
    return found;
}

// True once the window after the earliest edge has elapsed, so the event can be
// closed. Edges of one shot must arrive within windowUs of each other.
inline bool ready(const Observed &o, int64_t nowUs, int64_t windowUs) {
    int64_t firstUs = 0;
    return earliest(o, firstUs) && nowUs - firstUs >= windowUs;
}

// Builds the event from a final snapshot taken together with the re-arm.
inline bool close(const Observed &o, int64_t windowUs, Event &e) {
    e = Event{};
    if (!earliest(o, e.firstEdgeUs)) return false;
    for (int i = 0; i < kChannels; ++i) {
        e.rises[i] = o.rises[i];
        if (!o.received[i]) continue;
        if (o.firstRiseUs[i] - e.firstEdgeUs <= windowUs) {
            e.present[i] = true;
            e.edgeUs[i] = o.firstRiseUs[i];
        } else {
            ++e.lateEdges;
        }
    }
    return true;
}

// A channel counts for a precision result only with exactly one rising edge
// inside the window and no unattributed (late) edge anywhere in the event.
// Missing, doubled or late edges never produce a valid difference.
inline bool qualified(const Event &e, int channel) {
    return channel >= 0 && channel < kChannels && e.present[channel] &&
           e.rises[channel] == 1 && e.lateEdges == 0;
}

}  // namespace ClusterHwTimeEvent
