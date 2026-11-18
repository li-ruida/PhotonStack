#include "photonstack/LinearMatch.hpp"
#include "photonstack/WorkerPool.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace photonstack {
namespace {
struct Point { double x, y; unsigned region; bool check; };
double quantile(std::vector<double> values, double q) {
    std::sort(values.begin(), values.end());
    const double p = q * (values.size() - 1);
    const auto i = static_cast<std::size_t>(p);
    return values[i] + (p-i) * (values[std::min(i+1, values.size()-1)]-values[i]);
}
void validate(const ImageBuffer& image) {
    if (image.empty() || image.channels != 1 || image.colorEncoding != ColorEncoding::Linear || image.pixels.size() != image.pixelCount())
        throw std::invalid_argument("Intensity matching requires linear mono images");
}
void validate(const LinearMatchCoefficients& c) {
    if (!std::isfinite(c.gain) || c.gain <= 0 || !std::isfinite(c.offset))
        throw std::invalid_argument("Intensity match needs finite positive gain and finite offset");
}
LinearMatchCoefficients fit(const std::vector<Point>& points, std::size_t& downweighted) {
    std::vector<double> xs, ys;
    for (const auto& p:points) if (!p.check) { xs.push_back(p.x); ys.push_back(p.y); }
    const double cx=quantile(xs,.5), cy=quantile(ys,.5);
    const double sx=quantile(xs,.9)-quantile(xs,.1), sy=quantile(ys,.9)-quantile(ys,.1);
    if (!(sx > std::max(1e-30, std::abs(cx)*1e-12)) || !(sy > std::max(1e-30, std::abs(cy)*1e-12)))
        throw std::invalid_argument("Insufficient intensity range for gain fitting");
    // Robust marginal initialization, followed by centered Tukey bisquare IRLS. All
    // operations use double, with coordinates normalized to avoid cancellation.
    double a=1, b=0;
    for (unsigned iteration=0; iteration<100; ++iteration) {
        std::vector<double> residual;
        for (std::size_t i=0;i<xs.size();++i) residual.push_back((ys[i]-cy)/sy-a*(xs[i]-cx)/sx-b);
        const double center=quantile(residual,.5);
        std::vector<double> deviation; for (double r:residual) deviation.push_back(std::abs(r-center));
        const double scale=std::max(1e-10,1.4826*quantile(deviation,.5));
        double sw=0, mx=0, my=0;
        std::vector<double> weights; downweighted=0;
        for (std::size_t i=0;i<xs.size();++i) {
            const double u=std::abs(residual[i]-center)/(4.685*scale);
            const double w=u<1 ? (1-u*u)*(1-u*u) : 0;
            weights.push_back(w); if(w<.5)++downweighted;
            sw+=w;mx+=w*(xs[i]-cx)/sx;my+=w*(ys[i]-cy)/sy;
        }
        if(sw<64)throw std::invalid_argument("Too few effective samples after robust intensity rejection");
        mx/=sw;my/=sw;
        double xx=0,xy=0;
        for (std::size_t i=0;i<xs.size();++i) {
            const double x=(xs[i]-cx)/sx-mx,y=(ys[i]-cy)/sy-my;
            xx+=weights[i]*x*x;xy+=weights[i]*x*y;
        }
        if (!(xx>1e-12*sw)) throw std::invalid_argument("Degenerate intensity fit");
        const double nextA=xy/xx,nextB=my-nextA*mx;
        const bool converged=std::abs(nextA-a)+std::abs(nextB-b)<1e-10;
        a=nextA;b=nextB;
        if(converged)break;
        if(iteration==99)throw std::runtime_error("Robust intensity fit did not converge");
    }
    LinearMatchCoefficients c{a*sy/sx,cy+b*sy-a*sy/sx*cx};validate(c);return c;
}
}
LinearMatchResult matchLinearMaster(const ImageBuffer& reference, const ImageBuffer& target,
    LinearMatchOptions options, const LinearMatchCoefficients* replay) {
    validate(reference);validate(target);
    if(reference.width!=target.width||reference.height!=target.height)
        throw std::invalid_argument("Register masters onto the same grid before intensity matching");
    if(!std::isfinite(options.lowQuantile)||!std::isfinite(options.highQuantile)||options.lowQuantile<0||options.highQuantile>1||options.highQuantile-options.lowQuantile<.1)
        throw std::invalid_argument("Choose quantiles 0 <= low < high <= 1 spanning at least 0.1");
    if(replay)validate(*replay);
    LinearMatchResult result;auto& m=result.measurement;
    std::vector<Point> candidates;std::vector<double> xs,ys;
    // One deterministic jittered position per tile, at most 256x256 samples.
    // This keeps fitting bounded independently of full image dimensions.
    const unsigned nx=std::min(256U,target.width),ny=std::min(256U,target.height);
    for(unsigned j=0;j<ny;++j)for(unsigned i=0;i<nx;++i){
        const unsigned x0=static_cast<unsigned>(std::uint64_t(i)*target.width/nx),x1=static_cast<unsigned>(std::uint64_t(i+1)*target.width/nx);
        const unsigned y0=static_cast<unsigned>(std::uint64_t(j)*target.height/ny),y1=static_cast<unsigned>(std::uint64_t(j+1)*target.height/ny);
        const std::uint32_t hash=(i+1)*73856093U^(j+1)*19349663U;
        const unsigned x=x0+hash%(x1-x0),y=y0+(hash>>16)%(y1-y0);
        const auto index=std::size_t(y)*target.width+x;
        const double a=target.pixels[index],b=reference.pixels[index];
        if(!std::isfinite(a)||!std::isfinite(b))continue;
        const unsigned bx=static_cast<unsigned>(std::uint64_t(x)*8/target.width),by=static_cast<unsigned>(std::uint64_t(y)*8/target.height);
        candidates.push_back({a,b,(by/2)*4+bx/2,bool((bx+by)%2)});xs.push_back(a);ys.push_back(b);
    }
    m.candidates=candidates.size();
    if(candidates.size()<128)throw std::invalid_argument("Need at least 128 common finite samples for intensity matching");
    const double xlo=quantile(xs,options.lowQuantile),xhi=quantile(xs,options.highQuantile);
    const double ylo=quantile(ys,options.lowQuantile),yhi=quantile(ys,options.highQuantile);
    m.referenceSpan=yhi-ylo;
    std::vector<Point> selected;
    std::array<bool,16> fitRegions{},checkRegions{};
    for(const auto& p:candidates)if(p.x>=xlo&&p.x<=xhi&&p.y>=ylo&&p.y<=yhi){
        selected.push_back(p);
        if(p.check){++m.checkSamples;checkRegions[p.region]=true;}else{++m.fitSamples;fitRegions[p.region]=true;}
    }
    if(m.fitSamples<64||m.checkSamples<64||std::count(fitRegions.begin(),fitRegions.end(),true)<8||std::count(checkRegions.begin(),checkRegions.end(),true)<8)
        throw std::invalid_argument("Intensity samples must cover at least eight regions in both fitting and held-out sets");
    m.coefficients=replay?*replay:fit(selected,m.downweighted);
    // Replay uses saved coefficients, without fitting. Recompute diagnostics
    // against immutable input data, including residuals in every held-out tile.
    std::vector<double> residuals,absolute;
    for(const auto& p:selected)if(p.check){
        const double before=p.x-p.y,after=m.coefficients.gain*p.x+m.coefficients.offset-p.y;
        m.beforeRms+=before*before;m.afterRms+=after*after;residuals.push_back(after);absolute.push_back(std::abs(after));
        auto& r=m.regions[p.region];++r.count;r.beforeMean+=before;r.afterMean+=after;r.afterRms+=after*after;
    }
    m.beforeRms=std::sqrt(m.beforeRms/m.checkSamples);m.afterRms=std::sqrt(m.afterRms/m.checkSamples);
    m.medianResidual=quantile(residuals,.5);m.p95AbsoluteResidual=quantile(absolute,.95);
    for(auto& r:m.regions)if(r.count){r.beforeMean/=r.count;r.afterMean/=r.count;r.afterRms=std::sqrt(r.afterRms/r.count);}
    if(!std::isfinite(m.afterRms)||!std::isfinite(m.p95AbsoluteResidual))throw std::invalid_argument("Intensity residual overflow");
    result.image=target;result.validity=target;result.residual=target;
    const auto workers=std::min<unsigned>(target.height,std::clamp(std::thread::hardware_concurrency(),1U,8U));
    detail::runWorkers(workers,[&](std::size_t worker){
        for(std::size_t i=target.pixelCount()*worker/workers;i<target.pixelCount()*(worker+1)/workers;++i){
            const double value=m.coefficients.gain*double(target.pixels[i])+m.coefficients.offset;
            const bool valid=std::isfinite(value)&&std::abs(value)<=std::numeric_limits<float>::max();
            result.image.pixels[i]=valid?float(value):std::numeric_limits<float>::quiet_NaN();
            result.validity.pixels[i]=valid?1:0;
            const double residual=double(result.image.pixels[i])-reference.pixels[i];
            result.residual.pixels[i]=std::isfinite(residual)&&std::abs(residual)<=std::numeric_limits<float>::max()?float(residual):std::numeric_limits<float>::quiet_NaN();
        }
    });
    m.validFraction=double(std::count(result.validity.pixels.begin(),result.validity.pixels.end(),1.0F))/target.pixelCount();
    if(m.validFraction==0)throw std::invalid_argument("Intensity matching has no finite output");
    return result;
}
} // namespace photonstack
