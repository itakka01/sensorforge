#include "cluster_espnow_twoway.h"
#include <cmath>

namespace ClusterEspNowTwoWay {
namespace {
struct Sample { double offset, rtt; int64_t nodeMid; };
void sortRtt(Sample* s, size_t n) {
  for(size_t i=1;i<n;++i) {
    Sample v=s[i]; size_t j=i;
    while(j>0 && s[j-1].rtt>v.rtt) {s[j]=s[j-1];--j;}
    s[j]=v;
  }
}
void sortOffsets(double* x, size_t n) {
  for(size_t i=1;i<n;++i) {
    const double v=x[i]; size_t j=i;
    while(j && x[j-1]>v) {x[j]=x[j-1];--j;}
    x[j]=v;
  }
}
}
Estimate estimate(const Exchange* samples,size_t count,int64_t maxRttUs) {
  Estimate result;
  if(!samples || !count || count>kMaxSamples || maxRttUs<=0) return result;
  Sample accepted[kMaxSamples]; size_t n=0;
  for(size_t i=0;i<count;++i) {
    const Exchange& s=samples[i];
    // Both local intervals are monotonic and nonnegative. Validate before
    // subtraction so signed 64-bit arithmetic cannot overflow on ESP32-S3.
    if (s.nodeTxUs <= 0 || s.coordinatorRxUs <= 0 ||
        s.nodeRxUs <= s.nodeTxUs ||
        s.coordinatorTxUs < s.coordinatorRxUs) {
      ++result.rejected;
      continue;
    }
    const uint64_t nodeDuration =
        static_cast<uint64_t>(s.nodeRxUs) - static_cast<uint64_t>(s.nodeTxUs);
    const uint64_t coordinatorDuration =
        static_cast<uint64_t>(s.coordinatorTxUs) - static_cast<uint64_t>(s.coordinatorRxUs);
    if (nodeDuration > 10000000ULL || coordinatorDuration > nodeDuration ||
        nodeDuration - coordinatorDuration > static_cast<uint64_t>(maxRttUs)) {
      ++result.rejected;
      continue;
    }
    const uint64_t rtt = nodeDuration - coordinatorDuration;
    // Differences are taken in long double to avoid int64 overflow.
    const long double a=(long double)s.coordinatorRxUs-(long double)s.nodeTxUs;
    const long double b=(long double)s.coordinatorTxUs-(long double)s.nodeRxUs;
    accepted[n++]={(double)((a+b)/2.0L),(double)rtt,
                    s.nodeTxUs+(int64_t)(nodeDuration/2)};
  }
  result.accepted=(uint8_t)n;
  if(n<3) return result;
  // Whole-population median is diagnostic, independent of RTT selection.
  double allOffsets[kMaxSamples];
  for (size_t i=0;i<n;++i) allOffsets[i]=accepted[i].offset;
  sortOffsets(allOffsets,n);
  result.allMedianOffsetUs=(n&1)?allOffsets[n/2]:
      (allOffsets[n/2-1]+allOffsets[n/2])/2.0;
  sortRtt(accepted,n);
  result.minRttOffsetUs=accepted[0].offset;
  size_t subset=(n+3)/4; if(subset<3)subset=3;
  result.selected=static_cast<uint8_t>(subset);
  double offsets[kMaxSamples];
  long double rttSum=0;
  for(size_t i=0;i<subset;++i) {offsets[i]=accepted[i].offset;rttSum+=accepted[i].rtt;}
  sortOffsets(offsets,subset);
  const double median=(subset&1)?offsets[subset/2]:
      (offsets[subset/2-1]+offsets[subset/2])/2.0;
  result.valid=std::isfinite(median);
  result.offsetUs=median;
  result.spreadUs=offsets[subset-1]-offsets[0];
  result.selectedRttMinUs=accepted[0].rtt;
  result.selectedRttMaxUs=accepted[subset-1].rtt;
  // Robust interior range; retain full range as the unchanged quality gate.
  const size_t lo=(subset-1)/10;
  const size_t hi=((subset-1)*9+9)/10;
  result.selectedOffsetP10Us=offsets[lo]-median;
  result.selectedOffsetP90Us=offsets[hi]-median;
  result.medianRttUs=accepted[(subset-1)/2].rtt;
  result.nodeReferenceUs=accepted[0].nodeMid;
  return result;
}
bool estimateDriftPpm(const Estimate& older,const Estimate& newer,
                      double& driftPpm,int64_t minSeparationUs) {
  if(!older.valid || !newer.valid || minSeparationUs<=0 ||
     newer.nodeReferenceUs<=older.nodeReferenceUs) return false;
  const long double elapsed=(long double)newer.nodeReferenceUs-older.nodeReferenceUs;
  if(elapsed<minSeparationUs) return false;
  const long double drift=1000000.0L*
       ((long double)newer.offsetUs-older.offsetUs)/elapsed;
  if(!std::isfinite((double)drift) || fabsl(drift)>1000.0L) return false;
  driftPpm=(double)drift;return true;
}
}
