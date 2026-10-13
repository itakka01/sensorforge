#pragma once
#include <stdint.h>
#include <stddef.h>
namespace ClusterEspNowRegression {
constexpr size_t kHistory=24;
struct Point { int64_t nodeTimeUs=0; double offsetUs=0; double spreadUs=0; };
struct Fit { bool valid=false; double ppm=0; double residualUs=0; size_t count=0; int64_t spanUs=0; };
// Recent authenticated estimates only; no persistent time offset. Time centered
// to avoid catastrophic cancellation of ESP timer values.
Fit fit(const Point *points,size_t count,int64_t minSpanUs=180000000);
}
