#include "photonstack/GeneralizedStretch.hpp"
#include "photonstack/WorkerPool.hpp"
#include "MaskedImageSampling.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace photonstack {
GeneralizedStretchCurve::GeneralizedStretchCurve(const GeneralizedStretchOptions& o):options_(o),d_(std::expm1(o.logStretch)) {
    for(double v:{o.logStretch,o.localIntensity,o.symmetry,o.shadowProtection,o.highlightProtection,o.black,o.white})
        if(!std::isfinite(v))throw std::invalid_argument("GHS parameters must be finite");
    if(o.logStretch<0||o.logStretch>10||o.localIntensity< -5||o.localIntensity>15||o.shadowProtection<0||o.shadowProtection>o.symmetry||o.symmetry>o.highlightProtection||o.highlightProtection>1||o.black>=o.white||!std::isfinite(o.white-o.black))
        throw std::invalid_argument("GHS requires D 0...10, b -5...15, 0 <= LP <= SP <= HP <= 1 and black < white");
    if(d_==0){q0_=0;span_=1;lowValue_=highValue_=0;lowSlope_=highSlope_=1;return;}
    lowValue_=signedBase(o.shadowProtection);highValue_=signedBase(o.highlightProtection);
    lowSlope_=derivative(o.symmetry-o.shadowProtection);highSlope_=derivative(o.highlightProtection-o.symmetry);
    q0_=lowValue_-o.shadowProtection*lowSlope_;
    span_=highValue_+(1-o.highlightProtection)*highSlope_-q0_;
    if(!(span_>0)||!std::isfinite(span_))throw std::invalid_argument("GHS normalization is numerically degenerate");
}
double GeneralizedStretchCurve::base(double t) const {
    const double b=options_.localIntensity;
    if(std::abs(b)<1e-12)return -std::expm1(-d_*t);
    if(b>0)return -std::expm1(-std::log1p(b*d_*t)/b);
    const double l=std::log1p(-b*d_*t),z=(b+1)/b*l;
    // expm1(z)/z avoids subtractive cancellation at b=-1 and t=0.
    return (z==0?1:std::expm1(z)/z)*l/(-b);
}
double GeneralizedStretchCurve::derivative(double t) const {
    const double b=options_.localIntensity;
    if(std::abs(b)<1e-12)return d_*std::exp(-d_*t);
    return d_*std::exp((b>0?(-1-1/b):1/b)*std::log1p(std::abs(b)*d_*t));
}
double GeneralizedStretchCurve::signedBase(double x) const {
    return x<options_.symmetry?-base(options_.symmetry-x):base(x-options_.symmetry);
}
double GeneralizedStretchCurve::forward(double x) const {
    if(!std::isfinite(x)||x<0||x>1)throw std::invalid_argument("GHS curve input must be in [0,1]");
    if(d_==0||x==0||x==1)return x;
    const auto& o=options_;
    const double q=x<o.shadowProtection?lowValue_+(x-o.shadowProtection)*lowSlope_:
        x>o.highlightProtection?highValue_+(x-o.highlightProtection)*highSlope_:signedBase(x);
    return std::clamp((q-q0_)/span_,0.,1.);
}
double GeneralizedStretchCurve::evaluate(double x) const {
    if(!options_.inverse)return forward(x);
    if(!std::isfinite(x)||x<0||x>1)throw std::invalid_argument("GHS curve input must be in [0,1]");
    if(d_==0||x==0||x==1)return x;
    const auto& o=options_;const double q=x*span_+q0_;
    if(q<lowValue_&&lowSlope_>0)return std::clamp(o.shadowProtection+(q-lowValue_)/lowSlope_,0.,1.);
    if(q>highValue_&&highSlope_>0)return std::clamp(o.highlightProtection+(q-highValue_)/highSlope_,0.,1.);
    const double v=std::abs(q),b=o.localIntensity;
    double distance;
    if(std::abs(b)<1e-12)distance=-std::log1p(-v)/d_;
    else if(b>0)distance=std::expm1(-b*std::log1p(-v))/(b*d_);
    else if(std::abs(b+1)<1e-12)distance=std::expm1(v)/d_;
    else distance=std::expm1(b/(b+1)*std::log1p(-(b+1)*v))/(-b*d_);
    const double candidate=o.symmetry+std::copysign(distance,q);
    if(std::isfinite(candidate)&&candidate>=0&&candidate<=1&&std::abs(forward(candidate)-x)<1e-12)return candidate;
    // Degenerate near-flat floating-point branches have no reliable analytic
    // inverse. Return the monotone generalized inverse, without claiming that
    // clipping/quantization or RGB gamut blending can be undone.
    double lo=0,hi=1;
    for(unsigned i=0;i<60;++i){const double mid=(lo+hi)*.5;if(forward(mid)<x)lo=mid;else hi=mid;}
    return (lo+hi)*.5;
}
GeneralizedStretchResult generalizedStretch(const ImageBuffer& source,const GeneralizedStretchOptions& o) {
    GeneralizedStretchCurve curve(o);
    if(source.empty()||source.pixels.size()!=source.sampleCount()||(source.channels!=1&&source.channels!=3&&source.channels!=4)||
       detail::imageHasInvalidCoveredColor(source))throw std::invalid_argument("GHS requires finite covered mono or RGB pixels");
    GeneralizedStretchResult result;result.image=source;result.normalized=source;
    result.image.colorEncoding=result.normalized.colorEncoding=ColorEncoding::SRGB;
    struct Counts{std::size_t covered=0,low=0,high=0,blend=0;std::array<std::size_t,256> before{},after{};};
    const auto n=source.pixelCount();const unsigned colors=detail::colorChannelCount(source);
    const auto workers=n<262144?1U:std::min(8U,std::max(1U,std::thread::hardware_concurrency()));std::vector<Counts> counts(workers);
    detail::runWorkers(workers,[&](std::size_t worker){auto& stats=counts[worker];
        for(std::size_t p=n*worker/workers;p<n*(worker+1)/workers;++p){
            if(!detail::pixelHasValidColor(source,p)){detail::clearMaskedPixel(result.image,p);detail::clearMaskedPixel(result.normalized,p);continue;}
            if(source.channels==4) result.image.pixels[p*4+3]=result.normalized.pixels[p*4+3]=detail::pixelCoverage(source,p);
            ++stats.covered;double values[3]={},mapped[3]={};bool low=false,high=false;
            for(unsigned c=0;c<colors;++c){const double raw=source.pixels[p*source.channels+c];low|=raw<o.black;high|=raw>o.white;
                values[c]=std::clamp((raw-o.black)/(o.white-o.black),0.,1.);result.normalized.pixels[p*source.channels+c]=float(values[c]);}
            stats.low+=low;stats.high+=high;
            const double luminance=colors==1?values[0]:.2126*values[0]+.7152*values[1]+.0722*values[2];
            if(o.saturation){
                if(colors==1)mapped[0]=values[0];
                else {
                    const double high=*std::max_element(values,values+3),low=*std::min_element(values,values+3);
                    const double saturation=high>0?(high-low)/high:0;
                    const double factor=saturation>0?curve.evaluate(saturation)/saturation:1;
                    for(unsigned c=0;c<3;++c)mapped[c]=std::clamp(high-(high-values[c])*factor,0.,1.);
                }
            }else if(o.luminance&&colors==3&&luminance>0){
                const double factor = curve.evaluate(luminance) / luminance;
                if (*std::max_element(values, values + 3) * factor > 1) {
                    double blend = 1;
                    for (unsigned c = 0; c < 3; ++c) {
                        mapped[c] = curve.evaluate(values[c]);
                        const double scaled = values[c] * factor;
                        if (scaled > 1) blend = std::min(blend, (1 - mapped[c]) / (scaled - mapped[c]));
                    }
                    if (blend < 1) ++stats.blend;
                    for (unsigned c = 0; c < 3; ++c)
                        mapped[c] = std::clamp(blend * values[c] * factor + (1 - blend) * mapped[c], 0., 1.);
                } else {
                    for (unsigned c = 0; c < 3; ++c) mapped[c] = values[c] * factor;
                }
            } else {
                for (unsigned c = 0; c < colors; ++c) mapped[c] = curve.evaluate(values[c]);
            }
            for(unsigned c=0;c<colors;++c)result.image.pixels[p*source.channels+c]=float(mapped[c]);
            const double after=colors==1?mapped[0]:.2126*mapped[0]+.7152*mapped[1]+.0722*mapped[2];
            ++stats.before[std::min(255U,unsigned(luminance*256))];++stats.after[std::min(255U,unsigned(after*256))];
        }
    });
    for(const auto& c:counts){result.coveredPixels+=c.covered;result.belowBlack+=c.low;result.aboveWhite+=c.high;result.gamutBlendedPixels+=c.blend;
        for(std::size_t i=0;i<256;++i){result.beforeHistogram[i]+=c.before[i];result.afterHistogram[i]+=c.after[i];}}
    if(!result.coveredPixels)throw std::invalid_argument("GHS has no covered pixels");return result;
}
}
