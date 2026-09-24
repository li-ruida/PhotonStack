#include "photonstack/NoiseReducer.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace photonstack;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
double luminance(const ImageBuffer& image, std::size_t p) {
    return image.pixels[p * 4] * .2126 + image.pixels[p * 4 + 1] * .7152 + image.pixels[p * 4 + 2] * .0722;
}
}
int main() {
    for (bool shadows : {false, true}) {
        ImageBuffer image;
        image.width = image.height = 5;
        image.channels = 4;
        image.pixels.resize(image.sampleCount());
        for (std::size_t p = 0; p < image.pixelCount(); ++p) {
            const float rgb[] = {.995F, .93F, .995F};
            for (int c = 0; c < 3; ++c)
                image.pixels[p * 4 + c] = shadows ? 1 - rgb[c] : rgb[c];
            image.pixels[p * 4 + 3] = 1;
        }
        for (int c = 0; c < 3; ++c)
            image.pixels[12 * 4 + c] = shadows ? .005F : .995F;
        NoiseReductionOptions options;
        options.amount = 0;
        options.chromaAmount = 1;
        options.edgeThreshold = .1F;
        const auto result = NoiseReducer{}.reduce(image, options);
        require(result.ok, "colored highlight/shadow denoises");
        require(result.image.pixels != image.pixels, "gamut protection still allows useful chroma reduction");
        for (std::size_t p = 0; p < image.pixelCount(); ++p) {
            require(std::abs(luminance(result.image, p) - luminance(image, p)) < 2e-7,
                    "chroma-only denoising preserves luminance at gamut boundaries");
            for (int c = 0; c < 3; ++c) {
                const float v = result.image.pixels[p * 4 + c];
                require(v > .5F / 65535 && v < 1 - .5F / 65535,
                        "denoising does not create 16-bit black or white endpoints");
            }
            require(result.image.pixels[p * 4 + 3] == 1, "denoising preserves coverage");
        }
        options.chromaAmount = 0;
        const auto unchanged = NoiseReducer{}.reduce(image, options);
        require(unchanged.ok && unchanged.image.pixels == image.pixels, "zero amounts are an exact no-op");
        options.chromaAmount = 1;
        options.clampOutput = false;
        const auto scientific = NoiseReducer{}.reduce(image, options);
        require(scientific.ok, "unclamped denoising remains available");
        bool outside = false;
        for (std::size_t p = 0; p < image.pixelCount(); ++p) {
            require(std::abs(luminance(scientific.image, p) - luminance(image, p)) < 2e-7,
                    "unclamped chroma-only denoising also preserves luminance");
            for (int c = 0; c < 3; ++c)
                outside = outside || scientific.image.pixels[p * 4 + c] < 0 || scientific.image.pixels[p * 4 + c] > 1;
        }
        require(outside, "fixture actually exercises an out-of-gamut chroma proposal");
    }
    return 0;
}
