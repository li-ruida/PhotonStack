#include "photonstack/MasterRegistration.hpp"
#include "photonstack/WorkerPool.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

namespace photonstack {
namespace {
ImageBuffer rgba(const ImageBuffer& mono, unsigned width, unsigned height) {
    ImageBuffer result; result.width=width; result.height=height;
    result.pixels.resize(result.sampleCount(),0);
    for (unsigned y=0;y<mono.height;++y) for(unsigned x=0;x<mono.width;++x) {
        const float value=mono.pixels[std::size_t(y)*mono.width+x];
        if (!std::isfinite(value)) continue;
        const auto i=(std::size_t(y)*width+x)*4;
        result.pixels[i]=result.pixels[i+1]=result.pixels[i+2]=value; result.pixels[i+3]=1;
    }
    return result;
}
float sample(const ImageBuffer& mono, double x, double y) {
    constexpr float missing=std::numeric_limits<float>::quiet_NaN();
    if (!std::isfinite(x)||!std::isfinite(y)) return missing;
    // Coordinate roundoff in identity WCS must not erase the last row/column.
    if(std::abs(x-std::round(x))<1e-7)x=std::round(x);
    if(std::abs(y-std::round(y))<1e-7)y=std::round(y);
    if(x<0||y<0||x>mono.width-1.0||y>mono.height-1.0)return missing;
    const auto ix=static_cast<unsigned>(std::floor(x)),iy=static_cast<unsigned>(std::floor(y));
    const double fx=x-ix,fy=y-iy;
    double value=0;
    for(unsigned j=0;j<2;++j)for(unsigned i=0;i<2;++i){
        const double weight=(i?fx:1-fx)*(j?fy:1-fy);
        if(weight==0)continue;
        if(ix+i>=mono.width||iy+j>=mono.height)return missing;
        const auto v=mono.pixels[std::size_t(iy+j)*mono.width+ix+i];
        if(!std::isfinite(v))return missing;
        value+=weight*v;
    }
    return std::isfinite(value)&&std::abs(value)<=std::numeric_limits<float>::max()?static_cast<float>(value):missing;
}
bool sameSky(const SolvedPanel& a, const SolvedPanel& b) {
    if(a.grid.x!=b.grid.x||a.grid.y!=b.grid.y||a.grid.ra!=b.grid.ra||a.grid.dec!=b.grid.dec||a.grid.cd!=b.grid.cd||a.distortion.size()!=b.distortion.size())return false;
    for(std::size_t i=0;i<a.distortion.size();++i){const auto& x=a.distortion[i];const auto& y=b.distortion[i];if(x.i!=y.i||x.j!=y.j||x.a!=y.a||x.b!=y.b)return false;}
    return true;
}
void validate(const ImageBuffer& p) {
    if(p.empty()||p.channels!=1||p.pixels.size()!=p.pixelCount()||p.colorEncoding!=ColorEncoding::Linear)
        throw std::invalid_argument("Registration requires linear mono masters");
}
void validateTransform(const SimilarityTransform& t) {
    if(!std::isfinite(t.scale)||t.scale<=0||!std::isfinite(t.rotationRadians)||!std::isfinite(t.dx)||!std::isfinite(t.dy))
        throw std::invalid_argument("Invalid saved master transform");
}
}
RegisteredMaster registerMonoMaster(const LinearMonoMaster& reference, const LinearMonoMaster& moving,
    MasterRegistrationMode mode, const MasterRegistrationMeasurement* replay, bool isReference) {
    validate(reference.image);validate(moving.image);
    if(mode!=MasterRegistrationMode::Stars&&mode!=MasterRegistrationMode::Wcs)throw std::invalid_argument("Unknown master registration mode");
    if(mode==MasterRegistrationMode::Wcs&&(!reference.sky||!moving.sky))throw std::invalid_argument("WCS registration requires solved masters");
    RegisteredMaster result;
    if (replay) result.measurement=*replay;
    auto& m=result.measurement;
    if (isReference) {
        result.image=reference.image; m={};
    } else {
        std::optional<SkyProjection> referenceSky,movingSky;
        bool identitySky=false;
        if(mode==MasterRegistrationMode::Wcs){
            if(!reference.sky||!moving.sky)throw std::invalid_argument("WCS registration requires solved masters");
            identitySky=sameSky(*reference.sky,*moving.sky);
            referenceSky.emplace(*reference.sky,reference.sky->grid);
            movingSky.emplace(*moving.sky,reference.sky->grid);
        } else if(!replay) {
            const unsigned w=std::max(reference.image.width,moving.image.width),h=std::max(reference.image.height,moving.image.height);
            auto a=rgba(reference.image,w,h),b=rgba(moving.image,w,h);
            RegistrationOptions options;options.minimumMatches=12;options.matchTolerance=1.5F;
            options.similarityFallbackToTranslation=false;options.refineSimilarityCentroids=true;
            options.starDetection.minPeak=0;options.starDetection.sigmaThreshold=5;
            options.starDetection.robustStatistics=true;options.starDetection.maxStars=768;
            const auto fit=Registration().estimateSimilarity(a,b,options);
            if(!fit.ok||!fit.usedCentroidRefinement||fit.matches<12)
                throw std::runtime_error("Insufficient distributed PSF star matches for master registration: "+fit.message);
            m.transform=fit.transform;m.fitStars=fit.matches;m.fitRms=fit.centroidRefinementRms;
        }
        validateTransform(m.transform);
        result.image.width=reference.image.width;result.image.height=reference.image.height;
        result.image.channels=1;result.image.format=PixelFormat::Float32Gray;
        result.image.pixels.resize(result.image.pixelCount());
        const double c=std::cos(double(m.transform.rotationRadians)),s=std::sin(double(m.transform.rotationRadians));
        const auto workers=std::min<unsigned>(reference.image.height,std::clamp(std::thread::hardware_concurrency(),1U,8U));
        detail::runWorkers(workers,[&](std::size_t worker){
            for(std::size_t y=result.image.height*worker/workers;y<result.image.height*(worker+1)/workers;++y)
                for(unsigned x=0;x<result.image.width;++x){
                    SkyPoint p;
                    if(identitySky)p={double(x),double(y)};
                    else if(referenceSky){const auto sky=referenceSky->toCanvas(x,y);p=movingSky->toPanel(sky.x,sky.y);}
                    else {const double dx=double(x)-m.transform.dx,dy=double(y)-m.transform.dy;p={(c*dx+s*dy)/m.transform.scale,(-s*dx+c*dy)/m.transform.scale};}
                    result.image.pixels[y*result.image.width+x]=sample(moving.image,p.x,p.y);
                }
        });
        // Review the actual resampled result, not only the stars used to fit.
        // These grid-selected pairs may overlap the fit set; not held-out data.
        auto a=rgba(reference.image,reference.image.width,reference.image.height);
        auto b=rgba(result.image,result.image.width,result.image.height);
        const auto pairs=Registration::overlapCentroids(a,b,{});
        std::vector<double> distances;
        for(const auto& p:pairs)distances.push_back(std::hypot(double(p[0])-p[2],double(p[1])-p[3]));
        std::sort(distances.begin(),distances.end());m.checkStars=distances.size();m.median=m.p95=0;
        if(!distances.empty()){m.median=distances[distances.size()/2];m.p95=distances[std::min(distances.size()-1,static_cast<std::size_t>(std::ceil(.95*distances.size())-1))];}
        if(m.checkStars>=12&&(m.median>.5||m.p95>1))
            throw std::runtime_error("Resampled masters disagree at common stars (median > 0.5 or P95 > 1 pixel)");
        if(mode==MasterRegistrationMode::Stars&&m.checkStars<12)
            throw std::runtime_error("Fewer than 12 common PSF stars after resampling; registration is unverified");
    }
    result.validity.width=result.image.width;result.validity.height=result.image.height;
    result.validity.channels=1;result.validity.format=PixelFormat::Float32Gray;
    result.validity.pixels.resize(result.image.pixelCount());std::size_t valid=0;
    for(std::size_t i=0;i<result.image.pixelCount();++i){const bool finite=std::isfinite(result.image.pixels[i]);result.validity.pixels[i]=finite?1:0;valid+=finite;}
    if(!valid)throw std::runtime_error("Registered master has no valid overlap with the reference canvas");
    m.validFraction=double(valid)/result.image.pixelCount();
    return result;
}
} // namespace photonstack
