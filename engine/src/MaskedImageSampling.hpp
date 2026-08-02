#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack::detail {

struct WeightedValue {
    double value = 0.0;
    double weight = 0.0;
};

inline double weightedPercentile(std::vector<WeightedValue> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end(), [](const WeightedValue& left, const WeightedValue& right) {
        return left.value < right.value;
    });
    double totalWeight = 0.0;
    for (const auto& sample : values) {
        totalWeight += sample.weight;
    }
    if (!(totalWeight > 0.0) || !std::isfinite(totalWeight)) {
        return 0.0;
    }
    const double rank = std::clamp(fraction, 0.0, 1.0) * std::max(0.0, totalWeight - 1.0);
    double cumulative = 0.0;
    for (const auto& sample : values) {
        cumulative += sample.weight;
        if (cumulative > rank) {
            return sample.value;
        }
    }
    return values.back().value;
}

inline double weightedMedian(std::vector<WeightedValue> values) {
    values.erase(
        std::remove_if(values.begin(), values.end(), [](const WeightedValue& sample) {
            return !std::isfinite(sample.value) || !std::isfinite(sample.weight) || sample.weight <= 0.0;
        }),
        values.end()
    );
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end(), [](const WeightedValue& left, const WeightedValue& right) {
        return left.value < right.value;
    });

    double totalWeight = 0.0;
    for (const auto& sample : values) {
        totalWeight += sample.weight;
    }
    if (!(totalWeight > 0.0) || !std::isfinite(totalWeight)) {
        return 0.0;
    }

    constexpr double target = 0.5;
    double cumulative = values.front().weight;
    double previousPosition = values.front().weight * 0.5 / totalWeight;
    double previousValue = values.front().value;
    if (target <= previousPosition) {
        return previousValue;
    }

    for (std::size_t index = 1; index < values.size(); ++index) {
        const auto& sample = values[index];
        const double position = (cumulative + sample.weight * 0.5) / totalWeight;
        if (target <= position) {
            const double span = position - previousPosition;
            if (!(span > 0.0)) {
                return sample.value;
            }
            const double amount = std::clamp((target - previousPosition) / span, 0.0, 1.0);
            return previousValue + (sample.value - previousValue) * amount;
        }
        cumulative += sample.weight;
        previousPosition = position;
        previousValue = sample.value;
    }
    return values.back().value;
}

inline std::uint16_t colorChannelCount(const ImageBuffer& image) {
    return std::min<std::uint16_t>(3, image.channels);
}

inline float pixelCoverage(const ImageBuffer& image, std::size_t pixel) {
    if (image.channels != 4) {
        return 1.0F;
    }
    const float alpha = image.pixels[pixel * image.channels + 3];
    return std::isfinite(alpha) ? std::clamp(alpha, 0.0F, 1.0F) : 0.0F;
}

inline bool pixelHasValidColor(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    if (pixelCoverage(image, pixel) <= 1.0e-6F) {
        return false;
    }

    const auto channels = colorChannelCount(image);
    for (std::uint16_t channel = 0; channel < channels; ++channel) {
        if (!std::isfinite(image.pixels[offset + channel])) {
            return false;
        }
    }
    return true;
}

inline bool imageHasInvalidCoveredColor(const ImageBuffer& image) {
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        if (image.channels == 4 && !std::isfinite(image.pixels[offset + 3])) {
            return true;
        }
        if (pixelCoverage(image, pixel) <= 1.0e-6F) {
            continue;
        }
        for (std::uint16_t channel = 0; channel < colorChannelCount(image); ++channel) {
            if (!std::isfinite(image.pixels[offset + channel])) {
                return true;
            }
        }
    }
    return false;
}

inline bool imageHasCoveredColor(const ImageBuffer& image) {
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (pixelCoverage(image, pixel) > 1.0e-6F) {
            return true;
        }
    }
    return false;
}

inline std::size_t clampedPixelIndex(const ImageBuffer& image, int x, int y) {
    const auto clampedX = static_cast<std::uint32_t>(std::clamp(x, 0, static_cast<int>(image.width) - 1));
    const auto clampedY = static_cast<std::uint32_t>(std::clamp(y, 0, static_cast<int>(image.height) - 1));
    return static_cast<std::size_t>(clampedY) * image.width + clampedX;
}

inline bool sampleClampedValid(
    const ImageBuffer& image,
    int x,
    int y,
    std::uint16_t channel,
    float& value
) {
    const auto pixel = clampedPixelIndex(image, x, y);
    if (!pixelHasValidColor(image, pixel)) {
        return false;
    }
    value = image.pixels[pixel * image.channels + channel];
    return true;
}

inline bool sampleClampedCovered(
    const ImageBuffer& image,
    int x,
    int y,
    std::uint16_t channel,
    float& value,
    float& coverage
) {
    const auto pixel = clampedPixelIndex(image, x, y);
    if (!pixelHasValidColor(image, pixel)) {
        return false;
    }
    value = image.pixels[pixel * image.channels + channel];
    coverage = pixelCoverage(image, pixel);
    return true;
}

inline float outputColor(float value, bool clampOutput) {
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    return clampOutput ? std::clamp(value, 0.0F, 1.0F) : value;
}

inline void clearMaskedPixel(ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    for (std::uint16_t channel = 0; channel < colorChannelCount(image); ++channel) {
        image.pixels[offset + channel] = 0.0F;
    }
    if (image.channels == 4) {
        image.pixels[offset + 3] = 0.0F;
    }
}

} // namespace photonstack::detail
