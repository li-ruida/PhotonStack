#include "photonstack/LocalContrast.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <source_location>
#include <stdexcept>
using namespace photonstack;
void require(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok)
        throw std::runtime_error("Protected structure check at line " + std::to_string(where.line()));
}
int main() {
    ImageBuffer image;
    image.width = image.height = 128;
    image.channels = 4;
    image.pixels.resize(image.sampleCount());
    auto fill = [&](bool structure, bool star) {
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                const double noise = ((x + y) % 2 ? .01 : -.01);
                const double broad = structure ? .06 * std::cos((x - 64.) * 6.283185307 / 48) : 0;
                const double point = star ? .5 * std::exp(-((x - 64.) * (x - 64.) + (y - 64.) * (y - 64.)) / 2) : 0;
                const float value = .35 + noise + broad + point;
                for (int c = 0; c < 3; ++c)
                    image.pixels[(y * 128 + x) * 4 + c] = value + c * .015F;
                image.pixels[(y * 128 + x) * 4 + 3] = 1;
            }
    };
    LocalContrastOptions options;
    options.protectStructure = true;
    options.radius = 32;
    options.fineRadius = 6;
    options.amount = .8;
    const LocalContrast effect;
    fill(true, true);
    const auto result = effect.apply(image, options);
    require(result.ok);
    const auto peak = (64 * 128 + 64) * 4;
    // The smooth foreground/background correction may shift this pixel, but
    // must not enhance the compact star itself like an unsharp mask would.
    require(std::abs(result.image.pixels[peak] - image.pixels[peak]) < .03);
    // The broad cosine gains contrast away from the star; high-frequency
    // checkerboard detail and RGB differences are retained, not amplified.
    const auto maximum = (32 * 128 + 64) * 4, minimum = (32 * 128 + 88) * 4;
    require(result.image.pixels[maximum] - result.image.pixels[minimum] >
            (image.pixels[maximum] - image.pixels[minimum]) * 1.3);
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        require(std::abs((result.image.pixels[p * 4 + 1] - result.image.pixels[p * 4]) -
                         (image.pixels[p * 4 + 1] - image.pixels[p * 4])) < 1e-6);
        require(result.image.pixels[p * 4 + 3] == image.pixels[p * 4 + 3]);
    }
    fill(false, false);
    const auto noise = effect.apply(image, options);
    require(noise.ok);
    for (int y = 32; y < 96; ++y)
        for (int x = 32; x < 96; ++x)
            require(std::abs(noise.image.pixels[(y * 128 + x) * 4] - image.pixels[(y * 128 + x) * 4]) < 1e-5);
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c)
            image.pixels[p * 4 + c] *= .2F;
    const auto sky = effect.apply(image, options);
    require(sky.ok && sky.image.pixels == image.pixels);
    image.pixels[peak] = std::numeric_limits<float>::quiet_NaN();
    image.pixels[peak + 3] = 0;
    const auto masked = effect.apply(image, options);
    require(masked.ok && masked.image.pixels[peak] == 0);
    image.pixels[peak + 3] = 1;
    require(!effect.apply(image, options).ok);
    image.pixels[peak] = 2;
    require(!effect.apply(image, options).ok);
    image.pixels[peak] = .1;
    // A bright colored star on a flat non-dark background must not produce a
    // ring at the transition from full protection to unprotected structure.
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const double star = std::exp(-((x - 64.) * (x - 64.) + (y - 64.) * (y - 64.)) / 8);
            for (int c = 0; c < 3; ++c)
                image.pixels[(y * 128 + x) * 4 + c] = .35F + star * (c == 2 ? .64 : .2);
            image.pixels[(y * 128 + x) * 4 + 3] = 1;
        }
    const auto isolated = effect.apply(image, options);
    require(isolated.ok);
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        require(std::abs(isolated.image.pixels[p * 4] - image.pixels[p * 4]) < 1e-4);
    // Include a flat-topped, wider core: its center is absent from a median
    // residual detector and previously acquired a spurious bright ring.
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const double star = 6 * std::exp(-((x - 64.) * (x - 64.) + (y - 64.) * (y - 64.)) / 18);
            for (int c = 0; c < 3; ++c)
                image.pixels[(y * 128 + x) * 4 + c] = std::min(.995, .35 + star * (c == 2 ? .64 : .2));
        }
    const auto saturated = effect.apply(image, options);
    require(saturated.ok);
    for (int y = 60; y <= 68; ++y)
        for (int x = 60; x <= 68; ++x)
            require(std::abs(saturated.image.pixels[(y * 128 + x) * 4] - image.pixels[(y * 128 + x) * 4]) < 1e-4);
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        require(saturated.image.pixels[p * 4] - image.pixels[p * 4] < .002);
    ImageBuffer rgb = image;
    rgb.channels = 3;
    rgb.pixels.resize(rgb.sampleCount());
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c)
            rgb.pixels[p * 3 + c] = image.pixels[p * 4 + c];
    const auto noAlpha = effect.apply(rgb, options);
    require(noAlpha.ok);
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c)
            require(std::abs(noAlpha.image.pixels[p * 3 + c] - saturated.image.pixels[p * 4 + c]) < 1e-6);
    options.clampOutput = false;
    require(!effect.apply(image, options).ok);
    options.clampOutput = true;
    options.fineRadius = options.radius;
    require(!effect.apply(image, options).ok);
    options.fineRadius = 6;
    options.amount = 0;
    require(effect.apply(image, options).image.pixels == image.pixels);
    options.starChroma = .7F;
    const auto color = effect.apply(image, options);
    require(color.ok);
    auto luma = [](const float* pixel) { return .2126 * pixel[0] + .7152 * pixel[1] + .0722 * pixel[2]; };
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        require(std::abs(luma(&image.pixels[p * 4]) - luma(&color.image.pixels[p * 4])) < 1e-6);
    const auto wing = (70 * 128 + 64) * 4;
    require(color.image.pixels[wing + 2] - color.image.pixels[wing] < image.pixels[wing + 2] - image.pixels[wing]);
    require(color.image.pixels[0] == image.pixels[0]);
    options.starChroma = -1;
    require(!effect.apply(image, options).ok);
    options.starChroma = .7;
    options.protectStructure = false;
    require(!effect.apply(image, options).ok);
    // A smooth extended galaxy with foreground stars must not acquire large
    // nonstellar spots from differently normalized holes at the two scales.
    options.protectStructure = true;
    options.starChroma = 1;
    options.amount = .8;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const float galaxy = .12 + .55 * std::exp(-(x - 64.) * (x - 64.) / 500 - (y - 64.) * (y - 64.) / 1600);
            for (int c = 0; c < 3; ++c)
                image.pixels[(y * 128 + x) * 4 + c] = galaxy;
        }
    const auto smooth = effect.apply(image, options);
    require(smooth.ok);
    const std::array<std::array<int, 2>, 3> positions{{{52, 45}, {72, 60}, {61, 82}}};
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            double stars = 0;
            for (auto xy : positions)
                stars += .4 * std::exp(-((x - xy[0]) * (x - xy[0]) + (y - xy[1]) * (y - xy[1])) / 2.);
            for (int c = 0; c < 3; ++c)
                image.pixels[(y * 128 + x) * 4 + c] =
                    std::min(.995F, image.pixels[(y * 128 + x) * 4 + c] + static_cast<float>(stars));
        }
    const auto foreground = effect.apply(image, options);
    require(foreground.ok);
    for (int y = 16; y < 112; ++y)
        for (int x = 16; x < 112; ++x) {
            bool outside = true;
            for (auto xy : positions)
                outside = outside && (x - xy[0]) * (x - xy[0]) + (y - xy[1]) * (y - xy[1]) > 16 * 16;
            if (outside)
                require(std::abs(foreground.image.pixels[(y * 128 + x) * 4] - smooth.image.pixels[(y * 128 + x) * 4]) <
                        .02);
        }
    // A nearly saturated color channel must retain headroom while structure
    // in the other channels is enhanced; hard clipping made flat color peaks.
    options.amount = 2;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const float wave = .1 * std::cos((x-64.)*6.283185307/48);
            image.pixels[(y*128+x)*4] = .99F;
            image.pixels[(y*128+x)*4+1] = .3F+wave;
            image.pixels[(y*128+x)*4+2] = .2F+wave;
            image.pixels[(y*128+x)*4+3] = 1;
        }
    const auto highlight = effect.apply(image, options);
    require(highlight.ok);
    require(highlight.image.pixels[(32*128+64)*4] > .99F);
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        require(highlight.image.pixels[p*4] < 1 - .5F / 65535);

    // A continuum-only tone change is affine in an isolated star's residual,
    // unlike applying a nonlinear curve to the star and continuum together.
    options.amount = 0;
    options.starChroma = 1;
    options.continuumCurvePoints = {{0, 0}, {.25F, .4F}, {1, 1}};
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const double star = std::exp(-((x-64.)*(x-64.)+(y-64.)*(y-64.))/8);
            for (int c = 0; c < 3; ++c)
                image.pixels[(y*128+x)*4+c] = .25F + star * (c == 2 ? .7 : .4);
            image.pixels[(y*128+x)*4+3] = .6F;
        }
    const auto continuumTone = effect.apply(image, options);
    require(continuumTone.ok);
    double originalCore = 0, originalTotal = 0, mappedCore = 0, mappedTotal = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const auto p = (y*128+x)*4;
            for (int c = 0; c < 3; ++c) {
                const double expected = .4 + .8 * (image.pixels[p+c] - .25);
                require(std::abs(continuumTone.image.pixels[p+c] - expected) < .0001);
                require(continuumTone.image.pixels[p+c] >= 0 && continuumTone.image.pixels[p+c] <= 1);
            }
            require(continuumTone.image.pixels[p+3] == image.pixels[p+3]);
            const int distance = (x-64)*(x-64)+(y-64)*(y-64);
            if (distance <= 64) {
                originalTotal += image.pixels[p]-.25;
                mappedTotal += continuumTone.image.pixels[p]-.4;
                if (distance <= 4) {
                    originalCore += image.pixels[p]-.25;
                    mappedCore += continuumTone.image.pixels[p]-.4;
                }
            }
        }
    require(std::abs(originalCore/originalTotal - mappedCore/mappedTotal) < .0001);
    ImageBuffer toneRGB = image;
    toneRGB.channels = 3;
    toneRGB.pixels.resize(toneRGB.sampleCount());
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c)
            toneRGB.pixels[p*3+c] = image.pixels[p*4+c];
    const auto toneWithoutAlpha = effect.apply(toneRGB, options);
    require(toneWithoutAlpha.ok);
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c)
            require(std::abs(toneWithoutAlpha.image.pixels[p*3+c] - continuumTone.image.pixels[p*4+c]) < 1e-5);
    auto toneMaskedInput = image;
    toneMaskedInput.pixels[peak] = std::numeric_limits<float>::quiet_NaN();
    toneMaskedInput.pixels[peak+3] = 0;
    const auto toneMasked = effect.apply(toneMaskedInput, options);
    require(toneMasked.ok && toneMasked.image.pixels[peak] == 0 && toneMasked.image.pixels[peak+3] == 0);
    for (const auto value : toneMasked.image.pixels)
        require(std::isfinite(value));
    options.continuumCurvePoints = {{0,0},{1,1}};
    const auto identity = effect.apply(image, options);
    require(identity.ok && identity.image.pixels == image.pixels);
    options.continuumCurvePoints = {{0,0},{.25F,.1F},{1,1}};
    const auto darkerContinuum = effect.apply(image, options);
    require(darkerContinuum.ok);
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c)
            require(std::abs(darkerContinuum.image.pixels[p*4+c] - (.1 + .4*(image.pixels[p*4+c]-.25))) < .0001);
    options.continuumCurvePoints = {{0,0},{.5F,.9F},{.75F,.8F},{1,1}};
    require(!effect.apply(image, options).ok);
    options.continuumCurvePoints = {{0,.1F},{1,1}};
    require(!effect.apply(image, options).ok);
    options.continuumCurvePoints = {{0,0},{.5F,.5F},{.5F,.6F},{1,1}};
    require(!effect.apply(image, options).ok);
    options.continuumCurvePoints = {{0,0},{std::numeric_limits<float>::quiet_NaN(),.5F},{1,1}};
    require(!effect.apply(image, options).ok);
    options.continuumCurvePoints = {{0,0},{1,1}};
    options.protectStructure = false;
    require(!effect.apply(image, options).ok);
}
