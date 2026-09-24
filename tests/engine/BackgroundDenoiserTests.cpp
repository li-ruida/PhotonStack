#include "photonstack/BackgroundDenoiser.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <source_location>
#include <stdexcept>
using namespace photonstack;
void require(bool ok,const std::source_location at=std::source_location::current()) {
    if(!ok)throw std::runtime_error("Background denoise check at line "+std::to_string(at.line()));
}
int main() {
    ImageBuffer a;a.width=256;a.height=192;a.channels=4;a.pixels.resize(a.sampleCount());
    ImageBuffer mask;mask.width=a.width;mask.height=a.height;mask.channels=1;mask.pixels.assign(a.pixelCount(),1);
    std::mt19937 rng(921);std::normal_distribution<float> noise(0,1);
    std::vector<float> random(a.pixelCount());for(auto& v:random)v=noise(rng);
    double before=0;
    for(unsigned y=0;y<a.height;++y)for(unsigned x=0;x<a.width;++x) {
        const auto p=std::size_t(y)*a.width+x;
        double n=0;
        for(unsigned yy=y>4?y-4:0;yy<std::min(a.height,y+5);++yy)
            for(unsigned xx=x>4?x-4:0;xx<std::min(a.width,x+5);++xx)n+=random[std::size_t(yy)*a.width+xx]/9;
        double star=100*std::exp(-((x-110.)*(x-110.)+(y-100.)*(y-100.))/8);
        const double faint=12*std::exp(-((x-65.)*(x-65.)+(y-100.)*(y-100.))/8);
        double nebula=12*std::exp(-((x-185.)*(x-185.)+(y-100.)*(y-100.))/250);
        if(std::hypot(x-110.,y-100.)<12 || std::hypot(x-185.,y-100.)<38)mask.pixels[p]=0;
        for(unsigned c=0;c<3;++c)a.pixels[p*4+c]=float(-3+.002*x+.003*y+n*(1+c*.3)+star+faint+nebula);
        a.pixels[p*4+3]=1;
        if(x>20 && x<45 && y>20 && y<160)before+=n*n;
    }
    auto bypass=BackgroundDenoiser().apply(a,mask,0);require(bypass.ok && bypass.image.pixels==a.pixels);
    auto result=BackgroundDenoiser().apply(a,mask,.8);require(result.ok);
    double after=0;
    for(unsigned y=0;y<a.height;++y)for(unsigned x=0;x<a.width;++x) {
        const auto p=std::size_t(y)*a.width+x;
        for(unsigned c=0;c<4;++c)if(mask.pixels[p]==0 || c==3)require(result.image.pixels[p*4+c]==a.pixels[p*4+c]);
        if(x>20 && x<45 && y>20 && y<160)after+=std::pow(result.image.pixels[p*4]+3-.002*x-.003*y,2);
        if(std::hypot(x-65.,y-100.)<3)for(unsigned c=0;c<3;++c)
            require(result.image.pixels[p*4+c]==a.pixels[p*4+c]);
    }
    std::cerr<<"Correlated background noise energy ratio: "<<after/before<<"\n";require(after<before*.5);
    auto scaled=a;for(std::size_t p=0;p<a.pixelCount();++p)for(unsigned c=0;c<3;++c)scaled.pixels[p*4+c]*=100;
    const auto scaledResult=BackgroundDenoiser().apply(scaled,mask,.8);require(scaledResult.ok);
    for(std::size_t p=0;p<a.pixelCount();++p)for(unsigned c=0;c<3;++c)
        require(std::abs(scaledResult.image.pixels[p*4+c]/100-result.image.pixels[p*4+c])<.001);
    // A constant negative background and a masked bright source must not acquire
    // a halo or a clipped floor. A fully protected image is an exact bypass.
    for(std::size_t p=0;p<a.pixelCount();++p)for(unsigned c=0;c<3;++c)a.pixels[p*4+c]=mask.pixels[p]==0?100.f:-3.f;
    result=BackgroundDenoiser().apply(a,mask,1);require(result.ok);
    for(std::size_t p=0;p<a.pixelCount();++p)for(unsigned c=0;c<4;++c)require(std::abs(result.image.pixels[p*4+c]-a.pixels[p*4+c])<.0001);
    mask.pixels.assign(a.pixelCount(),0);result=BackgroundDenoiser().apply(a,mask,1);require(result.ok && result.image.pixels==a.pixels);
    require(!BackgroundDenoiser().apply(a,mask,-.1).ok);
    require(!BackgroundDenoiser().apply(a,mask,std::numeric_limits<double>::quiet_NaN()).ok);
    mask.pixels[0]=2;require(!BackgroundDenoiser().apply(a,mask,1).ok);
    mask.pixels[0]=1;a.pixels[3]=.5;require(!BackgroundDenoiser().apply(a,mask,1).ok);
}
