#pragma once
#include <stddef.h>
#include <stdint.h>

// Coordinator time minus node time; timestamps must originate from the
// same exchange. This estimator does not assert on-air timestamp accuracy.
namespace ClusterEspNowTwoWay {
constexpr size_t kMaxSamples = 64;
struct Exchange {
  int64_t nodeTxUs = 0;        // t1, node monotonic clock
  int64_t coordinatorRxUs = 0; // t2, coordinator monotonic clock
  int64_t coordinatorTxUs = 0; // t3, coordinator monotonic clock
  int64_t nodeRxUs = 0;        // t4, node monotonic clock
};
struct Estimate {
  bool valid = false;
  uint8_t accepted = 0;
  uint8_t rejected = 0;
  double minRttOffsetUs = 0;    // diagnostic, not automatically best clock
  double allMedianOffsetUs = 0; // diagnostic
  uint8_t selected = 0;        // lowest-RTT group
  double offsetUs = 0;          // coordinator clock - node clock
  double medianRttUs = 0;
  double spreadUs = 0;          // range of retained offsets, not an accuracy bound
  // Diagnostics for the selected lowest-RTT quartile, not quality thresholds.
  double selectedRttMinUs = 0;
  double selectedRttMaxUs = 0;
  double selectedOffsetP10Us = 0; // relative to the selected median
  double selectedOffsetP90Us = 0; // relative to the selected median
  int64_t nodeReferenceUs = 0;
};

// Rejects noncausal or excessive RTT samples, selects the lowest-RTT
// quartile (minimum three samples) and takes its median offset.
Estimate estimate(const Exchange* samples, size_t count,
                  int64_t maxRttUs = 50000);

// Drift (coordinator relative to node) inferred from two estimates separated
// by elapsed local time. Not safe for use before estimates were validated.
bool estimateDriftPpm(const Estimate& older, const Estimate& newer,
                      double& driftPpm, int64_t minSeparationUs = 30000000);
}
