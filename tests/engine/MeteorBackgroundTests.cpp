#include "../../engine/src/MeteorBackground.hpp"
#include <iostream>
#include <stdexcept>

using namespace photonstack;

namespace {
float reference(const ImageBuffer& image, unsigned x, unsigned y, unsigned radius, float fallback) {
    double sum = 0, weight = 0;
    for (unsigned yy = y > radius ? y - radius : 0; yy <= std::min(image.height - 1, y + radius); ++yy)
        for (unsigned xx = x > radius ? x - radius : 0; xx <= std::min(image.width - 1, x + radius); ++xx) {
            const auto i = (std::size_t(yy) * image.width + xx) * 4;
            const float a = image.pixels[i + 3];
            if (!std::isfinite(a) || a <= 1.e-6F || !std::isfinite(image.pixels[i]) ||
                !std::isfinite(image.pixels[i + 1]) || !std::isfinite(image.pixels[i + 2])) continue;
            const float luminance = image.pixels[i] * .2126F + image.pixels[i + 1] * .7152F + image.pixels[i + 2] * .0722F;
            if (!std::isfinite(luminance)) continue;
            const double coverage = std::min(a, 1.F);
            sum += double(luminance) * coverage;
            weight += coverage;
        }
    return weight <= 1.e-9 ? fallback : float(sum / weight);
}

detail::MeteorBackground tile(const ImageBuffer& image, unsigned x, unsigned y, unsigned radius) {
    const auto left = x / 256 * 256, top = y / 256 * 256;
    return {image, left > radius ? left - radius : 0, top > radius ? top - radius : 0,
            std::min(image.width - 1, left + 255 + radius), std::min(image.height - 1, top + 255 + radius)};
}

ImageBuffer uniform(float value, float alpha) {
    ImageBuffer image;
    image.width = 641; image.height = 289;
    image.pixels.assign(image.sampleCount(), value);
    for (std::size_t p = 0; p < image.pixelCount(); ++p) image.pixels[4 * p + 3] = alpha;
    return image;
}

void mixedCoverageAndSeams() {
    auto image = uniform(.05F, 1.F);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const float coverage[] = {0, .125F, .5F, 1, 2, nan, inf, -1, 1.e-8F};
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        for (unsigned c = 0; c < 3; ++c) image.pixels[4 * p + c] = (int((p + c * 7) % 99) - 49) * .003F;
        image.pixels[4 * p + 3] = coverage[p % 9];
        if (p % 157 == 0) image.pixels[4 * p] = nan;
        if (p % 167 == 0) image.pixels[4 * p + 1] = inf;
    }
    for (unsigned radius : {0U, 3U, 11U, 132U})
        for (unsigned y : {0U, 1U, 128U, 255U, 256U, 288U})
            for (unsigned x : {0U, 1U, 120U, 248U, 255U, 256U, 257U, 510U, 511U, 512U, 640U}) {
                const auto actual = tile(image, x, y, radius).at(image, x, y, radius, -7.F);
                const auto expected = reference(image, x, y, radius, -7.F);
                if (std::abs(actual - expected) > 2.e-8F)
                    throw std::runtime_error("Meteor background differs at a tile seam or invalid coverage");
            }
}

void extremePrefixDoesNotPolluteNeighborhood() {
    for (float extreme : {1.e20F, -1.e20F, std::numeric_limits<float>::max() / 4}) {
        auto image = uniform(.05F, .35F);
        for (unsigned c = 0; c < 3; ++c) image.pixels[(2 * image.width + 2) * 4 + c] = extreme;
        // The bright sample is in the integral prefix, outside this local box.
        const auto actual = tile(image, 160, 150, 7).at(image, 160, 150, 7, -7.F);
        if (actual != reference(image, 160, 150, 7, -7.F))
            throw std::runtime_error("HDR prefix destroyed a small local background");
        // Opposite HDR signs can cancel the signed prefix while its error stays large.
        for (unsigned c = 0; c < 3; ++c) image.pixels[(4 * image.width + 4) * 4 + c] = -extreme;
        if (tile(image, 160, 150, 7).at(image, 160, 150, 7, -7.F) != reference(image, 160, 150, 7, -7.F))
            throw std::runtime_error("Signed HDR cancellation bypassed the fallback");
    }
}

void tinyAndAbsentCoverage() {
    auto image = uniform(.123F, 1.F);
    for (unsigned y = 130; y <= 170; ++y) for (unsigned x = 140; x <= 180; ++x)
        image.pixels[(y * image.width + x) * 4 + 3] = 2.e-6F;
    if (tile(image, 160, 150, 7).at(image, 160, 150, 7, -7.F) != reference(image, 160, 150, 7, -7.F))
        throw std::runtime_error("Large coverage prefix destroyed a faint local sample");
    for (unsigned y = 130; y <= 170; ++y) for (unsigned x = 140; x <= 180; ++x)
        image.pixels[(y * image.width + x) * 4 + 3] = std::numeric_limits<float>::quiet_NaN();
    if (tile(image, 160, 150, 7).at(image, 160, 150, 7, -7.F) != -7.F)
        throw std::runtime_error("An uncovered local box must retain its fallback");
}
} // namespace

int main() {
    try {
        mixedCoverageAndSeams();
        extremePrefixDoesNotPolluteNeighborhood();
        tinyAndAbsentCoverage();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
