#include "photonstack/DisplayGridReducer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

using namespace photonstack;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static ImageBuffer image(int width, int height, int channels = 3) {
    ImageBuffer a; a.width = width; a.height = height; a.channels = channels;
    a.colorEncoding = ColorEncoding::SRGB; a.sourceBitsPerChannel = 16;
    if (channels == 1) a.format = PixelFormat::Float32Gray;
    a.pixels.resize(a.sampleCount(), .5F);
    if (channels == 4) for (std::size_t p = 0; p < a.pixelCount(); ++p) a.pixels[4 * p + 3] = 1;
    return a;
}

// Independently evaluate the reflected-domain frequency response using a DCT,
// without the production stencil, its coefficients or reflection indexing.
static std::vector<double> cosineReference(const ImageBuffer& a) {
    const int w = a.width, h = a.height;
    const double pi = std::numbers::pi;
    std::vector<double> out(a.pixelCount());
    for (int v = 0; v < h; ++v) for (int u = 0; u < w; ++u) {
        double coefficient = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            coefficient += a.pixels[y * w + x] * std::cos(pi * (x + .5) * u / w) * std::cos(pi * (y + .5) * v / h);
        coefficient *= (u ? 2.0 : 1.0) / w * (v ? 2.0 : 1.0) / h;
        coefficient *= (1 - std::pow(std::sin(pi * u / (2 * w)), 128)) *
                       (1 - std::pow(std::sin(pi * v / (2 * h)), 128));
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            out[y * w + x] += coefficient * std::cos(pi * (x + .5) * u / w) * std::cos(pi * (y + .5) * v / h);
    }
    return out;
}

int main() { try {
    const DisplayGridReducer reducer;
    for (auto dimensions : {std::pair{1,1}, {1,9}, {11,1}, {7,9}}) {
        auto a = image(dimensions.first, dimensions.second, 1);
        const auto constant = reducer.apply(a);
        require(constant.ok && constant.image.pixels == a.pixels, "constant changed");
        for (std::size_t i = 0; i < a.pixelCount(); ++i) a.pixels[i] += float(.0005 * std::sin(i * 3.17));
        const auto result = reducer.apply(a); require(result.ok, "small gray failed");
        const auto reference = cosineReference(a);
        for (std::size_t i = 0; i < a.pixelCount(); ++i)
            require(std::abs(result.image.pixels[i] - reference[i]) < 6e-8, "DCT response or reflected edge incorrect");
    }
    auto a = image(193, 151, 4);
    for (int y = 0; y < 151; ++y) for (int x = 0; x < 193; ++x) for (int c = 0; c < 3; ++c)
        a.pixels[(y * 193 + x) * 4 + c] = float(.1 + .3 * c + .08 * std::cos(x * 2.93 + y * 3.07 + c));
    const auto original = a.pixels;
    const auto result = reducer.apply(a); require(result.ok, "RGB filter failed");
    require(a.pixels == original, "input mutated");
    require(result.image.width == a.width && result.image.height == a.height &&
            result.image.format == a.format && result.image.sourceBitsPerChannel == 16 &&
            result.image.colorEncoding == a.colorEncoding, "metadata lost");
    require(reducer.apply(a, {.amount=0}).image.pixels == original, "zero amount changed pixels");
    // Channel permutations must keep one common gamut factor.
    auto swapped = a;
    for (std::size_t p = 0; p < a.pixelCount(); ++p) std::swap(swapped.pixels[p*4], swapped.pixels[p*4+2]);
    const auto swapResult = reducer.apply(swapped); require(swapResult.ok, "swapped failed");
    for (std::size_t p = 0; p < a.pixelCount(); ++p) {
        for (int c = 0; c < 3; ++c) require(swapResult.image.pixels[p*4+c] == result.image.pixels[p*4+2-c], "RGB channels asymmetric");
        require(result.image.pixels[p*4+3] == 1, "alpha filtered");
    }
    // An isolated hole or fractional coverage must not leak into the stencil;
    // everything outside its exact square of influence equals the full image.
    for (float coverage : {0.F, .4F}) {
        auto masked = a;
        const int hx=75, hy=70; const auto hole=(hy*193+hx)*4;
        masked.pixels[hole+3]=coverage;
        if (coverage==0) masked.pixels[hole]=std::numeric_limits<float>::quiet_NaN();
        const auto safe = reducer.apply(masked); require(safe.ok, "masked image failed");
        for (int y=0; y<151; ++y) for (int x=0; x<193; ++x) for(int c=0;c<4;++c) {
            const auto offset=(y*193+x)*4+c;
            if(x==hx && y==hy && coverage==0) require(safe.image.pixels[offset]==0,"hole not cleared");
            else if(std::abs(x-hx)<=64 && std::abs(y-hy)<=64)
                require(safe.image.pixels[offset]==masked.pixels[offset],"missing support modified");
            else require(safe.image.pixels[offset]==result.image.pixels[offset],"hole leaked beyond support");
        }
    }
    // High-amplitude impulses exercise the gamut guard, including exact ends.
    auto spikes=image(137,5,3);
    for(std::size_t i=0;i<spikes.pixels.size();++i) spikes.pixels[i]=float((i*311%1001)/1000.0);
    spikes.pixels[0]=0;spikes.pixels[1]=1;
    const auto bounded=reducer.apply(spikes);require(bounded.ok,"spikes failed");
    for(float v:bounded.image.pixels) require(std::isfinite(v)&&v>=0&&v<=1,"gamut guard failed");
    auto nan=a;nan.pixels[0]=std::numeric_limits<float>::quiet_NaN();
    require(!reducer.apply(nan).ok,"covered NaN accepted");
    nan=a;nan.pixels[3]=std::numeric_limits<float>::infinity();
    require(!reducer.apply(nan).ok,"invalid alpha accepted");
    nan=a;nan.pixels[3]=-1;
    require(!reducer.apply(nan).ok,"negative alpha accepted");
    nan=a;nan.pixels[0]=1.01F;
    require(!reducer.apply(nan).ok,"unbounded color accepted");
    for(double amount:{-.1,1.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        require(!reducer.apply(a,{.amount=amount}).ok,"invalid amount accepted");
    for(auto encoding:{ColorEncoding::Linear,ColorEncoding::Unknown}) {
        auto wrong=a;wrong.colorEncoding=encoding;require(!reducer.apply(wrong).ok,"scientific encoding accepted");
    }
    auto wrong=a;wrong.pixels.pop_back();require(!reducer.apply(wrong).ok,"short buffer accepted");
    wrong=image(2,2,2);require(!reducer.apply(wrong).ok,"unsupported channels accepted");
    require(!reducer.apply({}).ok,"empty buffer accepted");
    std::cout << "DCT response, reflected edges, constants, masks, RGB symmetry, gamut and input validation passed\n";
} catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
