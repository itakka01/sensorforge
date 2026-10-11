#pragma once
#include <Arduino.h>
#include "cluster_hw_time_capture.h"

// Coordinator-only, temporary ownership of the three measurement inputs.
// No automatic activation; owner must call release() on EVERY exit path.
namespace ClusterHwTimeGpioLease {
bool acquire(String &reason);
void release();
bool active();
void snapshot(ClusterHwTimeCapture::Edges &out);
}
