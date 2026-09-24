#include "photonstack/LocalContrast.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <source_location>
#include <stdexcept>
using namespace photonstack;
void require(bool condition, const std::source_location where = std::source_location::current()) {
    if (!condition) throw std::runtime_error("Continuum cooling check at line " + std::to_string(where.line()));
}
ImageBuffer fixture(bool star) {
    ImageBuffer im;
    im.width = im.height = 96;
    im.channels = 4;
    im.format = PixelFormat::Float32RGBA;
    im.colorEncoding = ColorEncoding::SRGB;
    im.pixels.resize(im.sampleCount());
    for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x) {
        double source = star ? std::exp(-((x-48.)*(x-48.)+(y-48.)*(y-48.))/3.2) : 0;
        auto p = (y*96+x)*4;
        im.pixels[p] = .36 + .3*source;
        im.pixels[p+1] = .33 + .4*source;
        im.pixels[p+2] = .3 + .6*source;
        im.pixels[p+3] = .75;
    }
    return im;
}
void checkEstimator(CoolingContinuumEstimator estimator) {
    LocalContrast effect;
    auto image = fixture(true), pedestal = fixture(false);
    ContinuumCoolingOptions options;
    options.estimator=estimator;
    auto result = effect.coolContinuum(image, options), background = effect.coolContinuum(pedestal, options);
    require(result.ok && background.ok);
    for (std::size_t p=0; p<image.pixelCount(); ++p) {
        for (int c=0; c<3; ++c) {
            auto i=p*4+c;
            require(result.image.pixels[i]>0 && result.image.pixels[i]<=image.pixels[i]);
            // Known injected RGB source on a flat continuum. A uniform
            // foreground subtraction should retain its signal, not rescale it.
            require(std::abs((result.image.pixels[i]-background.image.pixels[i]) -
                             (image.pixels[i]-pedestal.pixels[i]))<2e-5);
        }
        require(result.image.pixels[p*4+2]==image.pixels[p*4+2]);
        require(result.image.pixels[p*4+3]==image.pixels[p*4+3]);
    }
    options.amount=0;
    require(effect.coolContinuum(image,options).image.pixels==image.pixels);
    options.amount=.06;
    for (std::size_t p=0;p<pedestal.pixelCount();++p)
        for(int c=0;c<3;++c) pedestal.pixels[p*4+c]*=.1F;
    require(effect.coolContinuum(pedestal,options).image.pixels==pedestal.pixels);
    // Very low red samples inside a bright continuum exercise the floor guard.
    image.pixels[(48*96+48)*4]=1e-7;
    result=effect.coolContinuum(image,options);
    require(result.ok && result.image.pixels[(48*96+48)*4]>0);
    image.pixels[0]=std::numeric_limits<float>::quiet_NaN();
    require(!effect.coolContinuum(image,options).ok);
    image.pixels[3]=0;
    result=effect.coolContinuum(image,options);
    require(result.ok && result.image.pixels[0]==0 && result.image.pixels[3]==0);
    image=fixture(false);image.pixels[0]=1.01F;
    require(!effect.coolContinuum(image,options).ok);
    image=fixture(false);image.colorEncoding=ColorEncoding::Linear;
    require(!effect.coolContinuum(image,options).ok);
    image=fixture(false);
    for(double amount:{-.01,.121,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        options.amount=amount;require(!effect.coolContinuum(image,options).ok);
    }
    options.amount=.06;
    for(unsigned radius:{0,3,65}) {options.radius=radius;require(!effect.coolContinuum(image,options).ok);}
}

int main() {
    checkEstimator(CoolingContinuumEstimator::SourceExcluded);
    checkEstimator(CoolingContinuumEstimator::Opening);
    ContinuumCoolingOptions options;
    options.estimator=static_cast<CoolingContinuumEstimator>(999);
    require(!LocalContrast{}.coolContinuum(fixture(false),options).ok);
}
