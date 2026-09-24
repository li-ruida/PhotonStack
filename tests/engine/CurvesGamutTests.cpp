#include "photonstack/Curves.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace photonstack;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static double luma(const ImageBuffer& image, std::size_t pixel) {
    auto o = pixel * image.channels;
    return .2126 * image.pixels[o] + .7152 * image.pixels[o + 1] + .0722 * image.pixels[o + 2];
}
int main() { try {
    ImageBuffer image; image.width = 4; image.height = 1; image.channels = 4;
    image.pixels = {.99F,.25F,.12F,1, .2F,.4F,.8F,.5F, 0,0,0,1,
                    std::numeric_limits<float>::quiet_NaN(),0,0,0};
    CurvesOptions options; options.points = {{0,.2F},{1,1}};
    options.channel = CurveChannel::Luminance;
    const auto legacy = Curves{}.apply(image, options);
    require(legacy.ok && legacy.image.pixels[0] == 1, "legacy highlight fixture must clip");
    options.preserveLuminanceGamut = true;
    const auto safe = Curves{}.apply(image, options); require(safe.ok, "gamut transfer failed");
    auto gray = image;
    for (std::size_t p = 0; p < 3; ++p) {
        auto o=p*4; float y=.2126F*image.pixels[o]+.7152F*image.pixels[o+1]+.0722F*image.pixels[o+2];
        for(int c=0;c<3;++c) gray.pixels[o+c]=y;
    }
    auto referenceOptions=options; referenceOptions.channel=CurveChannel::RGB; referenceOptions.preserveLuminanceGamut=false;
    const auto reference=Curves{}.apply(gray,referenceOptions); require(reference.ok,"gray reference failed");
    for (std::size_t p=0;p<3;++p) {
        require(std::abs(luma(safe.image,p)-reference.image.pixels[4*p])<2e-7,"mapped luminance lost");
        require(safe.image.pixels[4*p+3]==image.pixels[4*p+3],"coverage changed");
        for(int c=0;c<3;++c) require(safe.image.pixels[4*p+c]>=0 && safe.image.pixels[4*p+c]<=1,"output outside gamut");
    }
    require(safe.image.pixels[0]<1,"gamut transfer introduced clipped red");
    require(safe.image.pixels[8]>.19F,"lifted black must follow gray curve");
    for(int c=0;c<4;++c)require(safe.image.pixels[12+c]==0,"invalid masked pixel not cleared");
    for(std::size_t p=0;p<2;++p) {
        auto o=p*4;
        double before=(image.pixels[o]-image.pixels[o+1])/(image.pixels[o+1]-image.pixels[o+2]);
        double after=(safe.image.pixels[o]-safe.image.pixels[o+1])/(safe.image.pixels[o+1]-safe.image.pixels[o+2]);
        require(std::abs(before-after)<2e-5,"chroma difference direction changed");
    }
    auto invalid=image;invalid.pixels[0]=1.01F;
    require(Curves{}.apply(invalid,options).errorCode=="ImageValueInvalid","unbounded values accepted");
    auto wrongChannel=options;wrongChannel.channel=CurveChannel::RGB;
    require(Curves{}.apply(image,wrongChannel).errorCode=="ArgumentInvalid","gamut option accepted for RGB curve");
    options.points={{0,0},{1,1}};
    const auto identity=Curves{}.apply(image,options);require(identity.ok,"identity failed");
    for(std::size_t i=0;i<12;++i) require(std::abs(identity.image.pixels[i]-image.pixels[i])<2e-7,"identity changed visible pixel");
    std::cout<<"Gamut-preserving luminance, clipping regression, chroma, lifted black, mask and validation passed\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;} }
