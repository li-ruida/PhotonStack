#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack::detail {

inline float sampleBilinearStraightAlpha(
    const ImageBuffer& image,
    float x,
    float y,
    std::uint16_t channel,
    bool clampToEdge
) {
    if (image.empty() || channel >= image.channels || !std::isfinite(x) || !std::isfinite(y)) {
        return 0.0F;
    }
    if (clampToEdge) {
        x = std::clamp(x, 0.0F, static_cast<float>(image.width - 1));
        y = std::clamp(y, 0.0F, static_cast<float>(image.height - 1));
    } else if (x < 0.0F || y < 0.0F ||
               x > static_cast<float>(image.width - 1) || y > static_cast<float>(image.height - 1)) {
        return 0.0F;
    }

    const auto x0 = static_cast<std::uint32_t>(std::floor(x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(y));
    const auto x1 = std::min<std::uint32_t>(x0 + 1, image.width - 1);
    const auto y1 = std::min<std::uint32_t>(y0 + 1, image.height - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const auto pixelOffset = [&](std::uint32_t px, std::uint32_t py) {
        return (static_cast<std::size_t>(py) * image.width + px) * image.channels;
    };
    const std::array<std::size_t, 4> offsets = {
        pixelOffset(x0, y0),
        pixelOffset(x1, y0),
        pixelOffset(x0, y1),
        pixelOffset(x1, y1),
    };
    const std::array<float, 4> weights = {
        (1.0F - tx) * (1.0F - ty),
        tx * (1.0F - ty),
        (1.0F - tx) * ty,
        tx * ty,
    };

    if (image.channels >= 4) {
        std::array<float, 4> alpha = {};
        double coverage = 0.0;
        for (std::size_t sample = 0; sample < offsets.size(); ++sample) {
            const float sourceAlpha = image.pixels[offsets[sample] + 3];
            alpha[sample] = std::isfinite(sourceAlpha) ? std::clamp(sourceAlpha, 0.0F, 1.0F) : 0.0F;
            coverage += static_cast<double>(weights[sample]) * alpha[sample];
        }
        if (channel == 3) {
            return static_cast<float>(coverage);
        }
        if (coverage <= 1.0e-6) {
            return 0.0F;
        }

        double premultipliedValue = 0.0;
        double validCoverage = 0.0;
        for (std::size_t sample = 0; sample < offsets.size(); ++sample) {
            const float value = image.pixels[offsets[sample] + channel];
            if (alpha[sample] > 0.0F && std::isfinite(value)) {
                const double sampleCoverage = static_cast<double>(weights[sample]) * alpha[sample];
                premultipliedValue += sampleCoverage * value;
                validCoverage += sampleCoverage;
            }
        }
        return validCoverage > 1.0e-6
            ? static_cast<float>(premultipliedValue / validCoverage)
            : 0.0F;
    }

    double value = 0.0;
    double validWeight = 0.0;
    for (std::size_t sample = 0; sample < offsets.size(); ++sample) {
        const float source = image.pixels[offsets[sample] + channel];
        if (std::isfinite(source)) {
            value += static_cast<double>(weights[sample]) * source;
            validWeight += weights[sample];
        }
    }
    return validWeight > 1.0e-6 ? static_cast<float>(value / validWeight) : 0.0F;
}

} // namespace photonstack::detail
