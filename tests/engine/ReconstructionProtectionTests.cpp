#include "photonstack/ReconstructionProtection.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace photonstack;
namespace {
void require(bool value, const char* text) {
    if (!value) { std::cerr << text << '\n'; std::exit(1); }
}
ImageBuffer gray(unsigned width, unsigned height, float value) {
    ImageBuffer image;
    image.width = width; image.height = height; image.channels = 1;
    image.format = PixelFormat::Float32Gray;
    image.pixels.assign(image.sampleCount(), value);
    return image;
}
}
int main() {
    const ReconstructionProtection protect;
    auto truth = gray(129, 129, 0), reconstructed = truth, mask = truth;
    for (unsigned y=0; y<129; ++y) for (unsigned x=0; x<129; ++x) {
        const auto p = y*129+x;
        const float dx = static_cast<float>(x)-64, dy = static_cast<float>(y)-64;
        truth.pixels[p] = -50 + .5F*x + 4000*std::exp(-(dx*dx+dy*dy)/200);
        reconstructed.pixels[p] = truth.pixels[p] + (x%2 ? 40 : -40);
        mask.pixels[p] = x>=40 && x<=88 && y>=40 && y<=88 ? 1 : 0;
    }
    auto result = protect.apply(reconstructed, truth, mask);
    require(result.ok && result.protectedPixels==49*49, "valid guard reports its active pixel count");
    double before=0, after=0;
    for (std::size_t p=0; p<mask.pixelCount(); ++p) {
        if (mask.pixels[p]==0) require(result.image.pixels[p]==reconstructed.pixels[p], "outside guard stays bit-exact");
        else { before+=std::abs(reconstructed.pixels[p]-truth.pixels[p]); after+=std::abs(result.image.pixels[p]-truth.pixels[p]); }
    }
    require(after < before*.001, "guard removes more than 99.9 percent of a known fine-scale error");
    require(result.image.pixels[0]<0 && result.image.pixels[64*129+64]>1000, "scientific negative and HDR samples are not display-clamped");
    auto doubled = reconstructed, doubledReference = truth;
    for (auto& v:doubled.pixels) v*=2;
    for (auto& v:doubledReference.pixels) v*=2;
    const auto scaled = protect.apply(doubled, doubledReference, mask);
    require(scaled.ok, "scaled scientific input succeeds");
    for (std::size_t p=0; p<mask.pixelCount(); ++p)
        require(scaled.image.pixels[p]==2*result.image.pixels[p], "guard respects linear photometric scaling");
    ReconstructionProtectionOptions options;
    options.amount=0;
    require(protect.apply(reconstructed,truth,mask,options).image.pixels==reconstructed.pixels, "zero amount is exact identity");
    auto emptyMask=gray(129,129,0);
    require(protect.apply(reconstructed,truth,emptyMask).image.pixels==reconstructed.pixels, "zero mask is exact identity");
    auto fractional=mask;
    for(auto&v:fractional.pixels)v*=.5F;
    auto half=protect.apply(reconstructed,truth,fractional);
    require(half.ok && std::abs(half.image.pixels[64*129+64]-(reconstructed.pixels[64*129+64]+result.image.pixels[64*129+64])*.5)<.001,
            "fractional weight gives continuous protection");

    auto original = gray(9,7,1234), pedestal = gray(9,7,1371), all = gray(9,7,1);
    options = {}; options.backgroundSigma=64;
    auto constant = protect.apply(original,pedestal,all,options);
    require(constant.ok && constant.image.pixels==original.pixels, "constant reference pedestal never changes the large-scale image, even with wide reflected support");
    auto one=protect.apply(gray(1,1,-20),gray(1,1,100),gray(1,1,1));
    require(one.ok && one.image.pixels[0]==-20, "reflection handles a single-pixel image");

    ImageBuffer rgba;
    rgba.width=9;rgba.height=7;rgba.pixels.resize(rgba.sampleCount());
    for(std::size_t p=0;p<rgba.pixelCount();++p){rgba.pixels[p*4]=1200;rgba.pixels[p*4+1]=-60;rgba.pixels[p*4+2]=13000;rgba.pixels[p*4+3]=.75F;}
    auto reference=rgba;
    for(std::size_t p=0;p<rgba.pixelCount();++p)for(unsigned c=0;c<3;++c)reference.pixels[p*4+c]+=80;
    rgba.pixels[4*4+3]=reference.pixels[4*4+3]=0;
    rgba.pixels[4*4]=reference.pixels[4*4]=std::numeric_limits<float>::quiet_NaN();
    auto covered=protect.apply(rgba,reference,all);
    require(covered.ok, "uncovered nonfinite values do not contaminate neighboring pixels");
    for(std::size_t p=0;p<rgba.pixelCount();++p){
        require(covered.image.pixels[p*4+3]==rgba.pixels[p*4+3], "coverage is preserved exactly");
        if(p!=4)for(unsigned c=0;c<3;++c)require(covered.image.pixels[p*4+c]==rgba.pixels[p*4+c], "normalized blur does not create seams at a coverage hole");
    }
    require(std::isnan(covered.image.pixels[4*4]), "uncovered source payload is not silently rewritten");
    reference.pixels[5*4+3]=0;
    require(protect.apply(rgba,reference,all).errorCode=="ReferenceCoverageMissing", "active guard cannot manufacture missing reference coverage");
    all.pixels[5]=0;
    require(protect.apply(rgba,reference,all).ok, "unused reference hole is allowed");

    auto badMask=mask;badMask.pixels[0]=1.01F;
    require(protect.apply(reconstructed,truth,badMask).errorCode=="MaskValueInvalid", "out-of-range weights are rejected");
    auto colored=rgba;for(auto&v:colored.pixels)v=1;colored.pixels[1]=.5;
    require(protect.apply(rgba,rgba,colored).errorCode=="MaskValueInvalid", "colored masks are not silently interpreted as weights");
    options={};options.amount=std::numeric_limits<double>::quiet_NaN();
    require(protect.apply(reconstructed,truth,mask,options).errorCode=="ArgumentInvalid", "nonfinite parameters are rejected");
    auto display=truth;display.colorEncoding=ColorEncoding::SRGB;
    require(protect.apply(reconstructed,display,mask).errorCode=="ImageEncodingInvalid", "nonlinear reference cannot masquerade as scientific input");
    require(protect.apply(reconstructed,gray(3,3,0),mask).errorCode=="ImageBufferInvalid", "mismatched reference grids are rejected");
    auto invalid=truth;invalid.pixels[0]=std::numeric_limits<float>::infinity();
    require(protect.apply(reconstructed,invalid,mask).errorCode=="ImageValueInvalid", "covered nonfinite scientific data are rejected");
    auto huge=gray(9,9,std::numeric_limits<float>::max()), opposite=gray(9,9,-std::numeric_limits<float>::max());
    opposite.pixels[40]=std::numeric_limits<float>::max();
    require(protect.apply(huge,opposite,gray(9,9,1)).errorCode=="ImageValueInvalid", "float32 overflow fails instead of clipping scientific data");
    return 0;
}
