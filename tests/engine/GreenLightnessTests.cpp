#include "photonstack/ColorAdjuster.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace photonstack;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
double decode(double x) {
    return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4);
}
double y(const float* p) {
    return .2126 * decode(p[0]) + .7152 * decode(p[1]) + .0722 * decode(p[2]);
}
int main() {
    ImageBuffer image;
    image.width = 17 * 17;
    image.height = 17;
    image.channels = 4;
    image.colorEncoding = ColorEncoding::SRGB;
    image.pixels.resize(image.sampleCount());
    for (int r = 0; r < 17; ++r)
        for (int g = 0; g < 17; ++g)
            for (int b = 0; b < 17; ++b) {
                const auto p = ((r * 17 + g) * 17 + b) * 4;
                image.pixels[p] = r / 16.F;
                image.pixels[p + 1] = g / 16.F;
                image.pixels[p + 2] = b / 16.F;
                image.pixels[p + 3] = .5F;
            }
    GreenCastSuppressionOptions options;
    options.amount = 1;
    options.greenExcessThreshold = 0;
    options.averageNeutral = true;
    options.preserveLightness = true;
    ColorAdjuster adjuster;
    const auto result = adjuster.suppressGreenCast(image, options);
    require(result.ok && result.affectedPixels > 0, "style executes");
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        const auto i = p * 4;
        require(std::abs(y(&image.pixels[i]) - y(&result.image.pixels[i])) < 1.5e-7,
                "linear Y preserved across color cube");
        require(result.image.pixels[i + 3] == .5F, "fractional alpha unchanged");
        for (int c = 0; c < 3; ++c) {
            const auto v = result.image.pixels[i + c];
            require(std::isfinite(v) && v >= 0 && v <= 1, "finite bounded gamut");
            if (image.pixels[i + 1] <= .5F * (image.pixels[i] + image.pixels[i + 2]))
                require(v == image.pixels[i + c], "neutral and non-green colors exact");
        }
    }
    // Pixel permutations commute with this operation: no neighborhood mixing,
    // blur, source detection or position-dependent change can occur.
    auto reversed = image;
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (int c = 0; c < 4; ++c)
            reversed.pixels[p * 4 + c] = image.pixels[(image.pixelCount() - 1 - p) * 4 + c];
    const auto reversedResult = adjuster.suppressGreenCast(reversed, options);
    require(reversedResult.ok, "permuted colors accepted");
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (int c = 0; c < 4; ++c)
            require(reversedResult.image.pixels[p * 4 + c] == result.image.pixels[(image.pixelCount() - 1 - p) * 4 + c],
                    "pointwise processing");
    const auto green = (16 * 17) * 4;
    require(std::abs(result.image.pixels[green] - result.image.pixels[green + 1]) < 1e-7 &&
                std::abs(result.image.pixels[green + 1] - result.image.pixels[green + 2]) < 1e-7,
            "pure green maps to neutral at original lightness");
    options.amount = 0;
    require(adjuster.suppressGreenCast(image, options).image.pixels == image.pixels, "zero amount identity");
    options.amount = .65F;
    auto rgb = image;
    rgb.channels = 3;
    rgb.pixels.resize(rgb.sampleCount());
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c)
            rgb.pixels[p * 3 + c] = image.pixels[p * 4 + c];
    require(adjuster.suppressGreenCast(rgb, options).ok, "RGB supported");
    image.colorEncoding = ColorEncoding::Linear;
    require(!adjuster.suppressGreenCast(image, options).ok, "scientific linear domain rejected");
    image.colorEncoding = ColorEncoding::SRGB;
    image.pixels[0] = 2;
    require(!adjuster.suppressGreenCast(image, options).ok, "unbounded display rejected");
    image.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    require(!adjuster.suppressGreenCast(image, options).ok, "covered NaN rejected");
    image.pixels[3] = 0;
    const auto masked = adjuster.suppressGreenCast(image, options);
    require(masked.ok && masked.image.pixels[0] == 0 && masked.image.pixels[3] == 0, "masked invalid color cleared");
    options.amount = std::numeric_limits<float>::infinity();
    require(!adjuster.suppressGreenCast(image, options).ok, "invalid strength rejected");
}
