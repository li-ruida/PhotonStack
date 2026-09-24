#include "photonstack/MultiscaleDenoiser.hpp"
#include "photonstack/BackgroundExtractor.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <source_location>
#include <stdexcept>
using namespace photonstack;
void require(bool ok, const std::source_location at=std::source_location::current()) {
    if(!ok) throw std::runtime_error("Multiscale check at line "+std::to_string(at.line()));
}
void independentNoiseReferenceTest() {
    // Structure filling the field must not be treated as a noise realization.
    ImageBuffer textured; textured.width=160; textured.height=160;
    textured.pixels.resize(textured.sampleCount());
    auto referenceNoise=textured;
    ImageBuffer weights; weights.width=160; weights.height=160; weights.channels=1;
    weights.pixels.assign(weights.pixelCount(),1);
    std::mt19937 imageRng(71), noiseRng(113);
    std::normal_distribution<float> imageGaussian(0,.5F), referenceGaussian(0,.5F);
    std::vector<double> truth(weights.pixelCount());
    for(unsigned y=0;y<160;++y)for(unsigned x=0;x<160;++x) {
        auto p=std::size_t(y)*160+x;
        truth[p]=-4+5*std::sin(x*2*3.141592653589793/10)+2*std::cos(y*2*3.141592653589793/17);
        const auto v=imageGaussian(imageRng),n=referenceGaussian(noiseRng);
        for(unsigned c=0;c<3;++c) {
            textured.pixels[p*4+c]=float(truth[p]+v);
            referenceNoise.pixels[p*4+c]=n;
        }
        textured.pixels[p*4+3]=referenceNoise.pixels[p*4+3]=1;
        if(x>=70 && x<80 && y>=70 && y<80) weights.pixels[p]=0;
    }
    const MultiscaleDenoiseOptions strong{.luminance=1,.chroma=0,.scales=3};
    auto sceneEstimated=MultiscaleDenoiser().apply(textured,weights,strong);
    auto independentlyEstimated=MultiscaleDenoiser().apply(textured,weights,strong,&referenceNoise);
    require(sceneEstimated.ok && independentlyEstimated.ok);
    double sceneError=0,independentError=0;
    for(unsigned y=24;y<136;++y)for(unsigned x=24;x<136;++x) {
        auto p=std::size_t(y)*160+x;
        sceneError+=std::pow(sceneEstimated.image.pixels[p*4]-truth[p],2);
        independentError+=std::pow(independentlyEstimated.image.pixels[p*4]-truth[p],2);
        require(independentlyEstimated.image.pixels[p*4+3]==1);
        if(weights.pixels[p]==0)require(independentlyEstimated.image.pixels[p*4]==textured.pixels[p*4]);
    }
    std::cerr<<"Independent noise reference scene error ratio "<<independentError/sceneError<<"\n";
    require(independentError<sceneError*.5);
    auto zeroNoise=referenceNoise;
    for(std::size_t p=0;p<zeroNoise.pixelCount();++p)for(unsigned c=0;c<3;++c)zeroNoise.pixels[p*4+c]=0;
    auto exact=MultiscaleDenoiser().apply(textured,weights,strong,&zeroNoise);
    require(exact.ok && exact.image.pixels==textured.pixels);
    auto coloredNoise=referenceNoise;
    auto coloredInput=textured;
    for(std::size_t p=0;p<coloredNoise.pixelCount();++p)
        for(unsigned c=0;c<3;++c) {
            coloredNoise.pixels[p*4+c]=referenceGaussian(noiseRng);
            coloredInput.pixels[p*4+c]+=imageGaussian(imageRng);
        }
    auto chromaOnly=MultiscaleDenoiser().apply(coloredInput,weights,{.luminance=0,.chroma=1,.scales=3},&coloredNoise);
    require(chromaOnly.ok);
    double chromaChange=0;
    for(std::size_t p=0;p<textured.pixelCount();++p) {
        double dy=0;const double rgbWeights[]{.2126,.7152,.0722};
        for(unsigned c=0;c<3;++c) {
            const auto change=chromaOnly.image.pixels[p*4+c]-coloredInput.pixels[p*4+c];
            dy+=rgbWeights[c]*change;chromaChange+=std::abs(change);
        }
        require(std::abs(dy)<.00003);
    }
    require(chromaChange>1);
    // A luminance-only reference must preserve the scene-estimated chroma
    // solution, not merely its chroma sigma. The shared luminance support mask
    // is part of that solution and must remain scene-derived for chroma.
    MultiscaleDenoiseOptions both{.luminance=.6,.chroma=.8,.scales=3};
    const auto original=MultiscaleDenoiser().apply(coloredInput,weights,both);
    const auto allReference=MultiscaleDenoiser().apply(coloredInput,weights,both,&coloredNoise);
    both.noiseReferenceScope=MultiscaleNoiseReferenceScope::LuminanceOnly;
    const auto luminanceReference=MultiscaleDenoiser().apply(coloredInput,weights,both,&coloredNoise);
    const auto noReference=MultiscaleDenoiser().apply(coloredInput,weights,both);
    const auto zeroLuminanceNoise=MultiscaleDenoiser().apply(coloredInput,weights,both,&zeroNoise);
    require(original.ok && allReference.ok && luminanceReference.ok && noReference.ok && zeroLuminanceNoise.ok);
    require(original.image.pixels==noReference.image.pixels);
    double changedLuminance=0;
    const auto luma=[](const ImageBuffer& a,std::size_t i) {
        return .2126*a.pixels[i]+.7152*a.pixels[i+1]+.0722*a.pixels[i+2];
    };
    for(std::size_t p=0;p<coloredInput.pixelCount();++p) {
        const auto i=p*4;
        require(luminanceReference.image.pixels[i+3]==coloredInput.pixels[i+3]);
        require(std::abs(luma(luminanceReference.image,i)-luma(allReference.image,i))<.00003);
        require(std::abs(luma(zeroLuminanceNoise.image,i)-luma(coloredInput,i))<.00003);
        changedLuminance+=std::abs(luma(luminanceReference.image,i)-luma(original.image,i));
        for(unsigned c:{0U,2U}) {
            const double before=double(original.image.pixels[i+c])-original.image.pixels[i+1];
            const double after=double(luminanceReference.image.pixels[i+c])-luminanceReference.image.pixels[i+1];
            const double zero=double(zeroLuminanceNoise.image.pixels[i+c])-zeroLuminanceNoise.image.pixels[i+1];
            require(std::abs(after-before)<.00003 && std::abs(zero-before)<.00003);
        }
        if(weights.pixels[p]==0)
            for(unsigned c=0;c<4;++c) require(luminanceReference.image.pixels[i+c]==coloredInput.pixels[i+c]);
    }
    require(changedLuminance>1);
    both.noiseReferenceScope=static_cast<MultiscaleNoiseReferenceScope>(99);
    require(!MultiscaleDenoiser().apply(coloredInput,weights,both,&coloredNoise).ok);
    auto invalidNoise=referenceNoise;invalidNoise.width-=1;
    require(!MultiscaleDenoiser().apply(textured,weights,strong,&invalidNoise).ok);
    invalidNoise=referenceNoise;invalidNoise.pixels[0]=std::numeric_limits<float>::quiet_NaN();
    require(!MultiscaleDenoiser().apply(textured,weights,strong,&invalidNoise).ok);
    invalidNoise=referenceNoise;invalidNoise.pixels[3]=.5;
    require(!MultiscaleDenoiser().apply(textured,weights,strong,&invalidNoise).ok);
    invalidNoise=referenceNoise;invalidNoise.colorEncoding=ColorEncoding::SRGB;
    require(!MultiscaleDenoiser().apply(textured,weights,strong,&invalidNoise).ok);
}
int main() {
    independentNoiseReferenceTest();
    ImageBuffer image; image.width=256; image.height=192; image.channels=4;
    image.pixels.resize(image.sampleCount());
    ImageBuffer blend; blend.width=image.width; blend.height=image.height; blend.channels=1;
    blend.pixels.assign(image.pixelCount(),1);
    std::mt19937 rng(42); std::normal_distribution<float> normal(0,3);
    std::vector<float> noise(image.pixelCount()*3);
    for(auto& v:noise)v=normal(rng);
    for(unsigned y=0;y<image.height;++y) for(unsigned x=0;x<image.width;++x) {
        const auto p=std::size_t(y)*image.width+x;
        const double star=160*std::exp(-((x-100.)*(x-100.)+(y-96.)*(y-96.))/8);
        const double nebula=30*std::exp(-((x-160.)*(x-160.)+(y-96.)*(y-96.))/1200);
        for(unsigned c=0;c<3;++c) {
            double correlated=0;
            for(unsigned yy=y>1?y-1:0;yy<std::min(image.height,y+2);++yy)
                for(unsigned xx=x>1?x-1:0;xx<std::min(image.width,x+2);++xx) correlated+=noise[(std::size_t(yy)*image.width+xx)*3+c]/3;
            image.pixels[p*4+c]=float(-5+nebula*(c==0?1.3:1)+star+correlated);
        }
        image.pixels[p*4+3]=1;
        if((x-100.)*(x-100.)+(y-96.)*(y-96.)<225)blend.pixels[p]=0;
    }
    const auto bypass=MultiscaleDenoiser().apply(image,blend);
    require(bypass.ok && bypass.image.pixels==image.pixels);
    auto result=MultiscaleDenoiser().apply(image,blend,{.luminance=.9,.chroma=1,.scales=4});
    require(result.ok && result.image.width==image.width && result.image.height==image.height);
    double before=0,after=0,nebulaBefore=0,nebulaAfter=0;
    for(unsigned y=0;y<image.height;++y) for(unsigned x=0;x<image.width;++x) {
        const auto p=std::size_t(y)*image.width+x;
        require(result.image.pixels[p*4+3]==1);
        for(unsigned c=0;c<3;++c) {
            if(blend.pixels[p]==0)require(result.image.pixels[p*4+c]==image.pixels[p*4+c]);
            if(x>15 && x<65 && y>15 && y<65) {
                before+=std::pow(image.pixels[p*4+c]+5,2);after+=std::pow(result.image.pixels[p*4+c]+5,2);
            }
            if(x>130 && x<190 && y>65 && y<125) {
                nebulaBefore+=image.pixels[p*4+c]+5;nebulaAfter+=result.image.pixels[p*4+c]+5;
            }
        }
    }
    std::cerr << "Noise energy ratio " << after/before << "; nebula aperture ratio " << nebulaAfter/nebulaBefore << "\n";
    require(after<before*.35);
    require(nebulaAfter/nebulaBefore>.97 && nebulaAfter/nebulaBefore<1.03);
    auto color=MultiscaleDenoiser().apply(image,blend,{.luminance=0,.chroma=1,.scales=4});require(color.ok);
    for(std::size_t p=0;p<image.pixelCount();++p) {
        double change=.2126*(color.image.pixels[p*4]-image.pixels[p*4])+.7152*(color.image.pixels[p*4+1]-image.pixels[p*4+1])+.0722*(color.image.pixels[p*4+2]-image.pixels[p*4+2]);require(std::abs(change)<.00003);
    }
    require(!MultiscaleDenoiser().apply(image,blend,{.scales=0}).ok);
    blend.pixels[0]=std::numeric_limits<float>::quiet_NaN();require(!MultiscaleDenoiser().apply(image,blend).ok);
    // Background samples live at cell centers. Continue a planar model to the
    // actual sensor edges instead of leaving an uncorrected half-cell border.
    for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x)for(unsigned c=0;c<3;++c)
        image.pixels[(std::size_t(y)*image.width+x)*4+c]=float(100+.2*x+.3*y+c*7);
    BackgroundGridOptions grid;grid.columns=8;grid.rows=6;grid.protectBrightTargets=false;
    grid.extraction.preserveBrightness=false;grid.extraction.clampOutput=false;grid.extrapolateEdges=true;
    auto flat=BackgroundExtractor().extractGrid(image,grid);require(flat.ok);
    for(unsigned c=0;c<3;++c) {
        const double center=flat.image.pixels[(96*256+128)*4+c];
        require(std::abs(flat.image.pixels[c]-center)<.001);
        require(std::abs(flat.image.pixels[(image.pixelCount()-1)*4+c]-center)<.001);
    }
    // Explicit target exclusions apply to the grid as well as the polynomial.
    auto starImage=image;
    for(unsigned y=72;y<120;++y)for(unsigned x=104;x<152;++x)for(unsigned c=0;c<3;++c)
        starImage.pixels[(std::size_t(y)*image.width+x)*4+c]+=100;
    grid.exclusions={{128,96,40,40,0}};grid.protectBrightTargets=true;
    auto protectedBackground=BackgroundExtractor().extractGrid(starImage,grid);require(protectedBackground.ok);
    require(protectedBackground.image.pixels[(96*256+128)*4]>90);
    grid.exclusions={{128,96,10000,10000,0}};
    require(!BackgroundExtractor().extractGrid(image,grid).ok);

}
