#include "cluster_beacon_sync.h"
#include <cmath>

namespace ClusterBeaconSync {
namespace {

void sortValues(double *v, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        const double x = v[i];
        size_t j = i;
        while (j && v[j - 1] > x) { v[j] = v[j - 1]; --j; }
        v[j] = x;
    }
}

double medianSorted(const double *v, size_t n) {
    if (!n) return 0;
    return (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

struct Line { bool ok = false; double a = 0, b = 0; };  // y = a + b*x (x in s, y in us)

Line fitLine(const double *x, const double *y, const bool *use, size_t n) {
    Line l;
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!use[i]) continue;
        sw += 1; sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i];
    }
    if (sw < 2) return l;
    const double den = sw * sxx - sx * sx;
    if (!(den > 1e-9)) {
        // All samples at (nearly) the same time: offset only.
        l.ok = true; l.a = sy / sw; l.b = 0; return l;
    }
    l.b = (sw * sxy - sx * sy) / den;
    l.a = (sy - l.b * sx) / sw;
    l.ok = std::isfinite(l.a) && std::isfinite(l.b);
    return l;
}

}  // namespace

double median(const double *values, size_t n) {
    if (!values || !n) return 0;
    double tmp[kMedianMax];
    if (n > kMedianMax) n = kMedianMax;
    for (size_t i = 0; i < n; ++i) tmp[i] = values[i];
    sortValues(tmp, n);
    return medianSorted(tmp, n);
}

void Estimator::reset() {
    first_ = count_ = 0;
    txPathWrite_ = txPathCount_ = 0;
    earlyInRow_ = 0;
    model_ = Model{};
}

int32_t Estimator::txPathMinUs() const {
    if (!txPathCount_) return -1;
    int32_t m = txPaths_[0];
    for (size_t i = 1; i < txPathCount_; ++i) if (txPaths_[i] < m) m = txPaths_[i];
    return m;
}

bool Estimator::sampleAt(size_t index, Sample &out) const {
    if (index >= count_) return false;
    out = ring_[(first_ + index) % kWindow];
    return true;
}

double Estimator::envelopeOffsetUs(int64_t nodeUs) const {
    const double x = static_cast<double>(nodeUs - model_.refNodeUs) / 1e6;
    return static_cast<double>(model_.baseUs) + model_.offsetUs + model_.driftPpm * x;
}

double Estimator::residualUs(const Sample &s) const {
    if (!model_.valid) return 0;
    const int64_t y = s.coordUs - s.nodeUs;
    const double x = static_cast<double>(s.nodeUs - model_.refNodeUs) / 1e6;
    return static_cast<double>(y - model_.baseUs) - (model_.offsetUs + model_.driftPpm * x);
}

void Estimator::dropOld(int64_t newestNodeUs) {
    while (count_ && newestNodeUs - ring_[first_].nodeUs > kMaxSampleAgeUs) {
        first_ = (first_ + 1) % kWindow;
        --count_;
    }
}

AddResult Estimator::add(const Sample &s) {
    ++stats_.added;
    if (count_) {
        const Sample &last = ring_[(first_ + count_ - 1) % kWindow];
        if (s.nodeUs <= last.nodeUs) { ++stats_.notMonotonic; return AddResult::NotMonotonic; }
    }
    // TX path: compare against the history before adding this value.
    if (s.txPathUs >= 0) {
        const int32_t minPath = txPathMinUs();
        txPaths_[txPathWrite_] = s.txPathUs;
        txPathWrite_ = (txPathWrite_ + 1) % kTxPathHistory;
        if (txPathCount_ < kTxPathHistory) ++txPathCount_;
        if (minPath >= 0 && txPathCount_ >= 4 && s.txPathUs > minPath + kTxPathSlackUs) {
            ++stats_.txPathLate;
            return AddResult::TxPathLate;
        }
    }
    AddResult result = AddResult::Accepted;
    dropOld(s.nodeUs);
    if (model_.valid && count_ && residualUs(s) > kEarlyRejectUs) {
        if (++earlyInRow_ < kEarlyResetCount) { ++stats_.early; return AddResult::Early; }
        // The model no longer describes the link (e.g. the coordinator clock
        // was stepped): keep the TX path history, rebuild the window.
        first_ = count_ = 0;
        model_ = Model{};
        ++stats_.resets;
        result = AddResult::Reset;
    }
    earlyInRow_ = 0;
    if (count_ == kWindow) { first_ = (first_ + 1) % kWindow; --count_; }
    ring_[(first_ + count_) % kWindow] = s;
    ++count_;
    ++stats_.accepted;
    refit();
    return result;
}

void Estimator::refit() {
    Model m;
    m.samples = static_cast<uint8_t>(count_);
    if (!count_) { model_ = m; return; }
    const Sample &newest = ring_[(first_ + count_ - 1) % kWindow];
    const Sample &oldest = ring_[first_];
    m.refNodeUs = newest.nodeUs;
    m.baseUs = newest.coordUs - newest.nodeUs;
    m.spanUs = newest.nodeUs - oldest.nodeUs;

    double x[kWindow], y[kWindow], r[kWindow], sorted[kWindow];
    bool use[kWindow];
    for (size_t i = 0; i < count_; ++i) {
        const Sample &s = ring_[(first_ + i) % kWindow];
        x[i] = static_cast<double>(s.nodeUs - m.refNodeUs) / 1e6;
        y[i] = static_cast<double>((s.coordUs - s.nodeUs) - m.baseUs);
        use[i] = true;
    }
    Line line = fitLine(x, y, use, count_);
    if (!line.ok) { m.invalid = Invalid::TooFewSamples; model_ = m; return; }
    size_t selected = count_;
    // Upper half: late samples (negative residual) drop out step by step.
    for (int pass = 0; pass < 3 && count_ >= 4; ++pass) {
        for (size_t i = 0; i < count_; ++i) {
            r[i] = y[i] - (line.a + line.b * x[i]);
            sorted[i] = r[i];
        }
        sortValues(sorted, count_);
        const double cut = medianSorted(sorted, count_);
        selected = 0;
        for (size_t i = 0; i < count_; ++i) { use[i] = r[i] >= cut; selected += use[i]; }
        if (selected < 2) break;
        const Line next = fitLine(x, y, use, count_);
        if (!next.ok) break;
        line = next;
    }
    double sq = 0;
    size_t used = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (!use[i]) continue;
        const double d = y[i] - (line.a + line.b * x[i]);
        sq += d * d;
        ++used;
    }
    m.selected = static_cast<uint8_t>(used);
    m.offsetUs = line.a;
    m.driftPpm = line.b;  // us per s == ppm
    m.jitterUs = used ? std::sqrt(sq / static_cast<double>(used)) : 0;
    if (count_ < kMinSamples) m.invalid = Invalid::TooFewSamples;
    else if (m.spanUs < kMinSpanUs) m.invalid = Invalid::SpanTooShort;
    else if (!std::isfinite(m.driftPpm) || std::fabs(m.driftPpm) > kMaxDriftPpm) m.invalid = Invalid::DriftOutOfRange;
    else if (!std::isfinite(m.jitterUs) || m.jitterUs > kMaxJitterUs) m.invalid = Invalid::JitterTooHigh;
    else m.invalid = Invalid::None;
    m.valid = m.invalid == Invalid::None;
    model_ = m;
}

}  // namespace ClusterBeaconSync
