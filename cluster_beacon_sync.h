#pragma once
// One-way broadcast time synchronization (node side), beta147.
//
// The coordinator broadcasts a signed beacon about once per second and right
// after it a follow-up with the coordinator monotonic time taken immediately
// before esp_now_send() of that beacon and the measured TX path (stamp ->
// TX-done callback). For every beacon a node gets one sample:
//
//   y = coordinatorStamp - nodeRx  (coordinator minus node clock, minus delay)
//
// Physically nodeRx happens after the stamp by queueing + airtime + receive
// callback latency. All of these are delays, never advances, so the samples
// with the LARGEST y are the clean ones. The estimator fits a line (offset +
// drift) through the upper half of a sliding window ("upper envelope") and
// rejects late samples. The remaining constant path delay is not known from
// one-way samples; it is calibrated rarely by a two-way series
// (delay = two-way offset - envelope offset, see ESPNOW_TIME_ARCHITECTURE.md).
//
//   coordinatorTime(node) = node + envelopeOffset(node) + delay
//
// Pure code without Arduino/ESP-IDF dependencies; host test:
// test_beacon_sync.cpp.test.
#include <stddef.h>
#include <stdint.h>

namespace ClusterBeaconSync {

constexpr size_t kWindow = 64;              // samples in the model (about 64 s)
constexpr int64_t kMaxSampleAgeUs = 120000000LL;
constexpr size_t kMinSamples = 8;
constexpr int64_t kMinSpanUs = 4000000LL;
constexpr double kMaxJitterUs = 150.0;      // RMS of the selected upper half
constexpr double kMaxDriftPpm = 500.0;
// A sample whose TX path (stamp -> TX-done) is longer than the shortest one in
// the recent history by more than this left the coordinator late (busy
// channel, queue): its stamp is too early, the sample is skipped.
constexpr int32_t kTxPathSlackUs = 150;
constexpr size_t kTxPathHistory = 32;
// Above the envelope by more than this = implausible (no delay can be
// negative); several in a row = model no longer fits, start over.
constexpr double kEarlyRejectUs = 150.0;
constexpr uint8_t kEarlyResetCount = 5;

enum class AddResult : uint8_t {
    Accepted = 0,
    TxPathLate,     // coordinator sent late, sample skipped
    Early,          // above the envelope, sample skipped
    NotMonotonic,   // node time not after the previous sample
    Reset,          // model restarted with this sample (repeated early samples)
};

enum class Invalid : uint8_t {
    None = 0,
    TooFewSamples,
    SpanTooShort,
    JitterTooHigh,
    DriftOutOfRange,
};

struct Sample {
    int64_t nodeUs = 0;    // node monotonic time of the beacon receive callback
    int64_t coordUs = 0;   // coordinator monotonic time immediately before send
    int32_t txPathUs = -1; // coordinator stamp -> TX-done callback, -1 = unknown
};

struct Model {
    bool valid = false;
    Invalid invalid = Invalid::TooFewSamples;
    uint8_t samples = 0;     // samples in the window
    uint8_t selected = 0;    // samples in the upper half used for the line
    int64_t refNodeUs = 0;   // node time of the newest sample (line origin)
    int64_t baseUs = 0;      // integer part of the offset at refNodeUs
    double offsetUs = 0;     // fractional offset at refNodeUs (envelope)
    double driftPpm = 0;     // coordinator clock rate minus node clock rate
    double jitterUs = 0;     // RMS residual of the selected samples
    int64_t spanUs = 0;
};

struct Stats {
    uint32_t added = 0, accepted = 0, txPathLate = 0, early = 0;
    uint32_t notMonotonic = 0, resets = 0;
};

class Estimator {
public:
    void reset();                       // window and model; lifetime stats stay
    void clearStats() { stats_ = Stats{}; }
    AddResult add(const Sample &s);
    const Model &model() const { return model_; }
    const Stats &stats() const { return stats_; }
    // Envelope offset (coordinator - node, without delay) at node time t.
    // Only meaningful while model().valid.
    double envelopeOffsetUs(int64_t nodeUs) const;
    // Residual of a sample against the current model (y - envelope); 0 if the
    // model is invalid.
    double residualUs(const Sample &s) const;
    // Shortest TX path in the recent history, -1 if none known.
    int32_t txPathMinUs() const;
    size_t count() const { return count_; }
    bool sampleAt(size_t index, Sample &out) const;  // 0 = oldest

private:
    void refit();
    void dropOld(int64_t newestNodeUs);

    Sample ring_[kWindow];
    size_t first_ = 0, count_ = 0;
    int32_t txPaths_[kTxPathHistory] = {};
    size_t txPathWrite_ = 0, txPathCount_ = 0;
    uint8_t earlyInRow_ = 0;
    Model model_;
    Stats stats_;
};

// Median of up to kMedianMax values (copies, does not reorder the input).
// 0 if n == 0.
constexpr size_t kMedianMax = 128;
double median(const double *values, size_t n);

// Path delay of one link (coordinator stamp -> node receive callback, as seen
// by the envelope): median of the last calibrations by two-way series.
struct DelayCalibration {
    static constexpr size_t kHistory = 5;
    double values[kHistory] = {};
    size_t count = 0, write = 0;
    int64_t lastNodeUs = 0;  // node time of the newest calibration
    void reset() { count = write = 0; lastNodeUs = 0; }
    void add(double delayUs, int64_t nodeUs) {
        values[write] = delayUs;
        write = (write + 1) % kHistory;
        if (count < kHistory) ++count;
        lastNodeUs = nodeUs;
    }
    bool known() const { return count > 0; }
    double value() const { return median(values, count); }
};
// A delay calibration needs a settled envelope: with only kMinSamples the
// upper half is 4 points and the offset was off by up to 150 us on hardware
// (beta147, first calibration of each node).
constexpr size_t kMinCalibrationSamples = 32;
// Plausible range of a calibrated delay (airtime alone is about 1 ms).
constexpr double kDelayMinUs = 0.0;
constexpr double kDelayMaxUs = 10000.0;

}  // namespace ClusterBeaconSync
