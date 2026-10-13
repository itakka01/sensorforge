#include "cluster_espnow_regression.h"
#include <cmath>
namespace ClusterEspNowRegression {
Fit fit(const Point *p,size_t n,int64_t minSpanUs) {
 Fit r;
 if(!p || n<4 || n>kHistory || minSpanUs<=0) return r;
 const int64_t origin=p[0].nodeTimeUs;
 if (origin<=0) return r;
 long double sw=0,sx=0,sy=0,sxx=0,sxy=0;
 for(size_t i=0;i<n;++i){
  if(p[i].nodeTimeUs<origin || !std::isfinite(p[i].offsetUs) ||
     !std::isfinite(p[i].spreadUs) || p[i].spreadUs<0) return r;
  const long double x=(long double)(p[i].nodeTimeUs-origin)/1000000.0L;
  const long double y=p[i].offsetUs;
  const long double w=1.0L/(25.0L+p[i].spreadUs*p[i].spreadUs);
  sw+=w;sx+=w*x;sy+=w*y;sxx+=w*x*x;sxy+=w*x*y;
 }
 r.spanUs=p[n-1].nodeTimeUs-origin;
 if(r.spanUs<minSpanUs) return r;
 const long double denom=sw*sxx-sx*sx;
 if(denom<=0) return r;
 const long double slope=(sw*sxy-sx*sy)/denom;
 const long double intercept=(sy-slope*sx)/sw;
 if(!std::isfinite((double)slope) || fabsl(slope)>500.0L) return r;
 long double err=0;
 for(size_t i=0;i<n;++i){
  const long double x=(long double)(p[i].nodeTimeUs-origin)/1000000.0L;
  const long double d=p[i].offsetUs-(intercept+slope*x);
  err+=d*d;
 }
 r.count=n;r.ppm=(double)slope;r.residualUs=(double)sqrtl(err/n);
 r.valid=std::isfinite(r.residualUs) && r.residualUs<=250.0;
 return r;
}
}
