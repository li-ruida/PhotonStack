#include "photonstack/PsfMeasurement.hpp"
#include "photonstack/StarCentroid.hpp"
#include "photonstack/StarDetector.hpp"
#include "photonstack/WorkerPool.hpp"
#include "MaskedImageSampling.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <thread>

namespace photonstack {
namespace {
constexpr double fwhmFactor=2.3548200450309493;
double quantile(std::vector<double> values,double fraction){
    std::sort(values.begin(),values.end());
    const double rank=fraction*(values.size()-1);const auto i=std::size_t(rank);
    return values[i]+(rank-i)*(values[std::min(i+1,values.size()-1)]-values[i]);
}
double equivalentFwhm(const PsfStarMeasurement& s){return fwhmFactor*std::pow(s.xx*s.yy-s.xy*s.xy,.25);}
unsigned region(const PsfStarMeasurement& s,unsigned width,unsigned height){
    return std::min(3U,unsigned(s.y*4/height))*4+std::min(3U,unsigned(s.x*4/width));
}
}
ImageBuffer gaussianPsfKernel(double xx,double yy,double xy){
    const double center=(xx+yy)/2,spread=std::hypot((xx-yy)/2,xy),det=xx*yy-xy*xy;
    if(!std::isfinite(xx)||!std::isfinite(yy)||!std::isfinite(xy)||center-spread<.55*.55||center+spread>3.8*3.8||det<=0)
        throw std::invalid_argument("PSF covariance lies outside supported positive-definite bounds");
    const int radius=int(std::ceil(4*std::sqrt(center+spread)));
    ImageBuffer result;result.width=result.height=unsigned(2*radius+1);result.channels=1;result.format=PixelFormat::Float32Gray;
    result.pixels.resize(result.pixelCount());double sum=0;
    std::vector<double> kernel;
    for(int y=-radius;y<=radius;++y)for(int x=-radius;x<=radius;++x){
        const double v=std::exp(-.5*(yy*x*x-2*xy*x*y+xx*y*y)/det);kernel.push_back(v);sum+=v;
    }
    for(std::size_t i=0;i<kernel.size();++i)result.pixels[i]=float(kernel[i]/sum);
    return result;
}
PsfMeasurement measurePsf(const ImageBuffer& image,const PsfMeasureOptions& options){
    if(image.empty()||(image.channels!=1&&image.channels!=3&&image.channels!=4)||image.colorEncoding!=ColorEncoding::Linear||image.pixels.size()!=image.sampleCount())
        throw std::invalid_argument("PSF measurement requires a linear mono/RGB image");
    if(!std::isfinite(options.sigmaThreshold)||options.sigmaThreshold<2||options.sigmaThreshold>30||!std::isfinite(options.saturation)||options.saturation<0)
        throw std::invalid_argument("PSF threshold must be 2..30 sigma and saturation nonnegative");
    const std::array<std::string,4> channels{"luminance","red","green","blue"};
    const auto chosen=std::find(channels.begin(),channels.end(),options.channel);
    if(chosen==channels.end()||(image.channels==1&&options.channel!="luminance"))throw std::invalid_argument("Select luminance, red, green or blue; mono uses luminance");
    const int channel=int(chosen-channels.begin())-1;
    std::set<std::size_t> excluded(options.excluded.begin(),options.excluded.end());
    if(excluded.size()!=options.excluded.size()||excluded.size()>2048)throw std::invalid_argument("Invalid excluded PSF star IDs");
    // Detection uses a finite luminance placeholder plus explicit coverage, so
    // NaN holes and partial RGB coverage are never treated as black sky.
    PixelStorage luminance(image.pixelCount());
    std::vector<float> coverage(image.pixelCount());
    for(std::size_t i=0;i<image.pixelCount();++i){
        if(!detail::pixelHasValidColor(image,i)||detail::pixelCoverage(image,i)<.999999F)continue;
        const auto* p=&image.pixels[i*image.channels];
        const double value=image.channels==1?p[0]:channel>=0?p[channel]:.2126*double(p[0])+.7152*double(p[1])+.0722*double(p[2]);
        if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())continue;
        luminance[i]=float(value);coverage[i]=1;
    }
    StarDetectionOptions detect;detect.robustStatistics=true;detect.sigmaThreshold=float(options.sigmaThreshold);
    detect.minPeak=std::numeric_limits<float>::lowest();detect.maxStars=2048;detect.countAllDetections=true;
    const auto candidates=StarDetector().detectLuminanceWithCoverage(image.width,image.height,luminance,coverage,detect);
    if(!candidates.ok)throw std::runtime_error(candidates.message);
    if(!excluded.empty()&&*excluded.rbegin()>=candidates.stars.size())throw std::invalid_argument("Excluded PSF star ID does not exist in this detection");
    ImageBuffer fitImage;fitImage.width=image.width;fitImage.height=image.height;fitImage.channels=1;fitImage.format=PixelFormat::Float32Gray;fitImage.pixels=std::move(luminance);
    PsfMeasurement result;result.width=image.width;result.height=image.height;result.detected=candidates.detectedCount;
    result.stars.resize(candidates.stars.size());
    const auto workers=std::min<std::size_t>(std::max<std::size_t>(1,candidates.stars.size()),std::clamp(std::thread::hardware_concurrency(),1U,8U));
    detail::runWorkers(workers,[&](std::size_t worker){
        for(std::size_t index=worker;index<candidates.stars.size();index+=workers){
            const auto& star=candidates.stars[index];auto& s=result.stars[index];s.id=index;s.x=star.x;s.y=star.y;s.sampleX=unsigned(std::round(star.x));s.sampleY=unsigned(std::round(star.y));
            if(excluded.contains(index)){s.status="excluded";continue;}
            const int x=int(std::round(star.x)),y=int(std::round(star.y));
            if(x<8||y<8||x>=int(image.width)-8||y>=int(image.height)-8){s.status="edge";continue;}
            bool missing=false,saturated=false;
            for(int dy=-7;dy<=7;++dy)for(int dx=-7;dx<=7;++dx){
                const auto p=std::size_t(y+dy)*image.width+unsigned(x+dx);
                if(coverage[p]<1){missing=true;continue;}
                if(options.saturation>0)for(unsigned c=0;c<(image.channels==1?1U:3U);++c)
                    if(channel<0||c==unsigned(channel))saturated|=image.pixels[p*image.channels+c]>=options.saturation;
            }
            if(missing){s.status="masked";continue;}
            if(saturated){s.status="saturated";continue;}
            bool crowded=false;
            for(std::size_t j=0;j<candidates.stars.size();++j)if(j!=index){
                const auto& other=candidates.stars[j];
                if(std::hypot(double(star.x)-other.x,double(star.y)-other.y)<12){crowded=true;break;}
            }
            if(crowded){s.status="crowded";continue;}
            const auto fit=fitStarCentroid(fitImage,star.x,star.y);
            if(!fit.ok){s.status="fit-failed";continue;}
            s.x=fit.x;s.y=fit.y;s.xx=fit.covarianceXX;s.yy=fit.covarianceYY;s.xy=fit.covarianceXY;s.residual=fit.relativeResidual;s.status="accepted";
        }
    });
    std::vector<double> widths;
    for(const auto& s:result.stars)if(s.status=="accepted")widths.push_back(std::log(equivalentFwhm(s)));
    if(widths.size()<12)throw std::runtime_error("PSF needs at least 12 isolated, fully covered, well-fitted stars; accepted "+std::to_string(widths.size())+" of "+std::to_string(result.stars.size()));
    const double middle=quantile(widths,.5);
    std::vector<double> deviations;for(double w:widths)deviations.push_back(std::abs(w-middle));
    // A minimum 25% width tolerance retains measurable field variation instead
    // of forcing identical stars in an unrealistically noiseless fixture.
    const double limit=std::max(std::log(1.25),4*1.4826*quantile(deviations,.5));
    for(auto& s:result.stars)if(s.status=="accepted"&&std::abs(std::log(equivalentFwhm(s))-middle)>limit)s.status="shape-outlier";
    std::vector<double> finalWidths,residuals;std::array<std::vector<double>,16> regional;
    double minX=image.width,maxX=0,minY=image.height,maxY=0;
    for(const auto& s:result.stars)if(s.status=="accepted"){
        ++result.accepted;result.xx+=s.xx;result.yy+=s.yy;result.xy+=s.xy;
        const double width=equivalentFwhm(s);finalWidths.push_back(width);residuals.push_back(s.residual);
        const auto cell=region(s,image.width,image.height);regional[cell].push_back(width);
        result.regions[cell].xx+=s.xx;result.regions[cell].yy+=s.yy;result.regions[cell].xy+=s.xy;
        minX=std::min(minX,s.x);maxX=std::max(maxX,s.x);minY=std::min(minY,s.y);maxY=std::max(maxY,s.y);
    }
    unsigned populated=0;
    for(std::size_t i=0;i<16;++i)if(!regional[i].empty()){
        ++populated;auto& r=result.regions[i];r.count=regional[i].size();r.fwhm=quantile(regional[i],.5);
        r.xx/=r.count;r.yy/=r.count;r.xy/=r.count;
    }
    if(result.accepted<12||populated<4||maxX-minX<.4*image.width||maxY-minY<.4*image.height)
        throw std::runtime_error("PSF stars lack field coverage: require 12 stars, four regions and 40% span in both axes");
    // Equal-weight mean of accepted positive-definite covariance matrices stays
    // positive definite and avoids brightness weighting toward the brightest star.
    result.xx/=result.accepted;result.yy/=result.accepted;result.xy/=result.accepted;
    const double center=(result.xx+result.yy)/2,spread=std::hypot((result.xx-result.yy)/2,result.xy);
    result.majorFwhm=fwhmFactor*std::sqrt(center+spread);result.minorFwhm=fwhmFactor*std::sqrt(center-spread);
    result.angleDegrees=.5*std::atan2(2*result.xy,result.xx-result.yy)*180/std::numbers::pi;
    result.fwhmP10=quantile(finalWidths,.1);result.fwhmP50=quantile(finalWidths,.5);result.fwhmP90=quantile(finalWidths,.9);
    result.medianResidual=quantile(residuals,.5);
    std::vector<double> medians;for(const auto& r:result.regions)if(r.count>=3)medians.push_back(r.fwhm);
    result.spatiallyVariable=std::exp(quantile(widths,.9)-quantile(widths,.1))>1.3||result.fwhmP90/result.fwhmP10>1.3||
        (medians.size()>=4&&*std::max_element(medians.begin(),medians.end())/ *std::min_element(medians.begin(),medians.end())>1.2);
    for(const auto& r:result.regions)if(r.count>=3){
        const double dx=r.xx-result.xx,dy=r.yy-result.yy,dxy=r.xy-result.xy;
        if(std::sqrt(dx*dx+dy*dy+2*dxy*dxy)/(result.xx+result.yy)>.15)result.spatiallyVariable=true;
    }
    return result;
}
} // namespace photonstack
