#include "photonstack/Stretch.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "MaskedImageSampling.hpp"
#include "photonstack/ParallelRanges.hpp"

namespace photonstack {
namespace {

StretchResult stretchError(std::string code, std::string message) {
    StretchResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

float stretchSample(float value, const StretchOptions& options, double gamma) {
    const double normalized = std::clamp(
        (static_cast<double>(value) - options.blackPoint) /
            (static_cast<double>(options.whitePoint) - options.blackPoint),
        0.0, 1.0
    );

    double stretched = std::pow(normalized, gamma);

    if (options.arcsinhStrength > 0.0F) {
        const double strength = options.arcsinhStrength;
        stretched = std::asinh(stretched * strength) / std::asinh(strength);
    }
    return static_cast<float>(std::clamp(stretched, 0.0, 1.0));
}

double luminanceAt(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    if (image.channels >= 3) {
        return static_cast<double>(image.pixels[offset]) * 0.2126 +
               static_cast<double>(image.pixels[offset + 1]) * 0.7152 +
               static_cast<double>(image.pixels[offset + 2]) * 0.0722;
    }
    return image.pixels[offset];
}

struct WeightedSample {
    double value = 0.0;
    double weight = 0.0;
};

double weightedMedian(std::vector<WeightedSample>& values) {
    if (values.empty()) {
        return 0.0;
    }
    // O(n) exact order statistics for fully covered images. Fractional alpha
    // still uses weighted statistics; no sampling or loss of faint pixels.
    if (std::all_of(values.begin(), values.end(), [](const auto& sample) { return sample.weight == 1.0; })) {
        const auto middle = values.begin() + values.size() / 2;
        const auto less = [](const auto& left, const auto& right) { return left.value < right.value; };
        std::nth_element(values.begin(), middle, values.end(), less);
        if (values.size() % 2 != 0) return middle->value;
        return (std::max_element(values.begin(), middle, less)->value + middle->value) * 0.5;
    }
    std::sort(values.begin(), values.end(), [](const WeightedSample& left, const WeightedSample& right) {
        return left.value < right.value;
    });
    double totalWeight = 0.0;
    for (const auto& sample : values) {
        totalWeight += sample.weight;
    }
    const double midpoint = totalWeight * 0.5;
    double cumulative = 0.0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        cumulative += values[index].weight;
        if (cumulative < midpoint) {
            continue;
        }
        const double tolerance = std::max(1.0, totalWeight) * 1.0e-12;
        if (std::fabs(cumulative - midpoint) <= tolerance && index + 1 < values.size()) {
            return (values[index].value + values[index + 1].value) * 0.5;
        }
        return values[index].value;
    }
    return values.back().value;
}

double weightedPercentile(std::vector<WeightedSample> values, double position) {
    if (values.empty()) {
        return 0.0;
    }
    if (std::all_of(values.begin(), values.end(), [](const auto& sample) { return sample.weight == 1.0; })) {
        const auto rank = static_cast<std::size_t>(std::clamp(position, 0.0, 1.0) * (values.size() - 1));
        const auto selected = values.begin() + rank;
        std::nth_element(values.begin(), selected, values.end(),
            [](const auto& left, const auto& right) { return left.value < right.value; });
        return selected->value;
    }
    std::sort(values.begin(), values.end(), [](const WeightedSample& left, const WeightedSample& right) {
        return left.value < right.value;
    });
    double totalWeight = 0.0;
    for (const auto& sample : values) {
        totalWeight += sample.weight;
    }
    const double rank = std::clamp(position, 0.0, 1.0) * std::max(0.0, totalWeight - 1.0);
    double cumulative = 0.0;
    for (const auto& sample : values) {
        cumulative += sample.weight;
        if (cumulative > rank) {
            return sample.value;
        }
    }
    return values.back().value;
}

std::vector<WeightedSample> validLuminanceSamples(const ImageBuffer& image) {
    std::vector<WeightedSample> values;
    values.reserve(image.pixelCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        float coverage = 1.0F;
        if (image.channels == 4) {
            const float alpha = image.pixels[offset + 3];
            if (!std::isfinite(alpha)) {
                continue;
            }
            coverage = std::clamp(alpha, 0.0F, 1.0F);
        }
        if (coverage <= 1.0e-6F || !detail::pixelHasValidColor(image, pixel)) {
            continue;
        }
        const double value = luminanceAt(image, pixel);
        if (std::isfinite(value)) {
            values.push_back({.value = value, .weight = coverage});
        }
    }
    return values;
}

struct AutoStatistics {
    double background = 0, mad = 0, minimum = 0, maximum = 0, white = 0;
    bool valid = false;
};

AutoStatistics autoStatistics(const ImageBuffer& image, float clip) {
    bool opaque = true;
    if (image.channels == 4) {
        for (std::size_t p = 0; p < image.pixelCount(); ++p) {
            if (image.pixels[p * 4 + 3] != 1.0F) { opaque = false; break; }
        }
    }
    if (opaque) {
        std::vector<double> values;
        values.reserve(image.pixelCount());
        for (std::size_t p = 0; p < image.pixelCount(); ++p) {
            if (!detail::pixelHasValidColor(image, p)) continue;
            const double value = luminanceAt(image, p);
            if (std::isfinite(value)) values.push_back(value);
        }
        if (values.empty()) return {};
        const auto median = [](std::vector<double>& samples) {
            auto mid = samples.begin() + samples.size() / 2;
            std::nth_element(samples.begin(), mid, samples.end());
            return samples.size() % 2 ? *mid : (*std::max_element(samples.begin(), mid) + *mid) * 0.5;
        };
        const auto [low, high] = std::minmax_element(values.begin(), values.end());
        AutoStatistics result;
        result.minimum = *low; result.maximum = *high;
        result.background = median(values);
        auto white = values.begin() + static_cast<std::size_t>(static_cast<double>(clip) * (values.size() - 1));
        std::nth_element(values.begin(), white, values.end());
        result.white = *white;
        for (auto& value : values) value = std::fabs(value - result.background);
        result.mad = median(values) * 1.4826;
        result.valid = true;
        return result;
    }
    auto luminance = validLuminanceSamples(image);
    if (luminance.empty()) return {};
    auto medianSamples = luminance;
    AutoStatistics result;
    result.background = weightedMedian(medianSamples);
    for (auto& sample : luminance) sample.value = std::fabs(sample.value - result.background);
    result.mad = weightedMedian(luminance) * 1.4826;
    const auto [low, high] = std::minmax_element(medianSamples.begin(), medianSamples.end(),
        [](const auto& a, const auto& b) { return a.value < b.value; });
    result.minimum = low->value; result.maximum = high->value;
    result.white = weightedPercentile(std::move(medianSamples), clip);
    result.valid = true;
    return result;
}

bool hasInvalidCoveredSample(const ImageBuffer& image) {
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        float coverage = 1.0F;
        if (image.channels == 4) {
            const float alpha = image.pixels[offset + 3];
            if (!std::isfinite(alpha)) {
                return true;
            }
            coverage = std::clamp(alpha, 0.0F, 1.0F);
        }
        if (coverage <= 1.0e-6F) {
            continue;
        }
        for (std::uint16_t channel = 0; channel < detail::colorChannelCount(image); ++channel) {
            if (!std::isfinite(image.pixels[offset + channel])) {
                return true;
            }
        }
        if (!std::isfinite(luminanceAt(image, pixel))) {
            return true;
        }
    }
    return false;
}

bool hasValidLuminanceSample(const ImageBuffer& image) {
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (detail::pixelHasValidColor(image, pixel) && std::isfinite(luminanceAt(image, pixel))) {
            return true;
        }
    }
    return false;
}

} // namespace

StretchResult Stretch::apply(const ImageBuffer& image, const StretchOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return stretchError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (!std::isfinite(options.blackPoint) || !std::isfinite(options.midPoint) ||
        !std::isfinite(options.whitePoint) || !std::isfinite(options.arcsinhStrength)) {
        return stretchError("ArgumentInvalid", "Stretch options must be finite");
    }
    if (options.whitePoint <= options.blackPoint) {
        return stretchError("ArgumentInvalid", "whitePoint must be greater than blackPoint");
    }
    if (options.midPoint <= 0.0F || options.midPoint >= 1.0F) {
        return stretchError("ArgumentInvalid", "midPoint must be between 0 and 1");
    }
    if (options.arcsinhStrength < 0.0F) {
        return stretchError("ArgumentInvalid", "arcsinhStrength must be greater than or equal to zero");
    }
    if (hasInvalidCoveredSample(image)) {
        return stretchError("ImageBufferInvalid", "Covered image colors and alpha must be finite");
    }

    ImageBuffer output = image;
    const double mid = std::clamp(static_cast<double>(options.midPoint), 0.001, 0.999);
    const double gamma = std::log(0.5) / std::log(mid);
    detail::parallelRanges(image.pixelCount(), 262144, [&](std::size_t begin, std::size_t end) noexcept {
        for (std::size_t pixel = begin; pixel < end; ++pixel) {
            const auto offset = pixel * image.channels;
            if (!detail::pixelHasValidColor(image, pixel)) {
                detail::clearMaskedPixel(output, pixel);
                continue;
            }
            const auto colorChannels = std::min<std::uint16_t>(3, image.channels);
            if (options.preserveColor && colorChannels >= 3) {
                const double luminance = luminanceAt(image, pixel);
                const float mapped = stretchSample(static_cast<float>(luminance), options, gamma);
                // Measure color ratios from the selected black point. Background
                // subtraction can leave signed samples with luminance near zero;
                // dividing by that original luminance amplifies chroma without bound.
                const double signal = luminance - options.blackPoint;
                if (signal <= 1.0e-6) {
                    output.pixels[offset] = mapped;
                    output.pixels[offset + 1] = mapped;
                    output.pixels[offset + 2] = mapped;
                } else {
                    const double ratio = mapped / signal;
                    output.pixels[offset] =
                        static_cast<float>(std::clamp((image.pixels[offset] - options.blackPoint) * ratio, 0.0, 1.0));
                    output.pixels[offset + 1] =
                        static_cast<float>(std::clamp((image.pixels[offset + 1] - options.blackPoint) * ratio, 0.0, 1.0));
                    output.pixels[offset + 2] =
                        static_cast<float>(std::clamp((image.pixels[offset + 2] - options.blackPoint) * ratio, 0.0, 1.0));
                }
            } else {
                for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                    output.pixels[offset + channel] = stretchSample(image.pixels[offset + channel], options, gamma);
                }
            }
        }
    });
    output.colorEncoding = ColorEncoding::SRGB;

    StretchResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

StretchOptions Stretch::estimateAuto(const ImageBuffer& image, const AutoStretchOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return {};
    }
    const AutoStretchOptions defaults;
    const AutoStretchOptions safeOptions{
        .shadowsSigma = std::isfinite(options.shadowsSigma) && options.shadowsSigma > 0.0F
                            ? options.shadowsSigma
                            : defaults.shadowsSigma,
        .targetBackground = std::isfinite(options.targetBackground) && options.targetBackground > 0.0F &&
                                    options.targetBackground < 1.0F
                                ? options.targetBackground
                                : defaults.targetBackground,
        .highlightClip = std::isfinite(options.highlightClip) && options.highlightClip > 0.0F &&
                                 options.highlightClip <= 1.0F
                             ? options.highlightClip
                             : defaults.highlightClip,
        .arcsinhStrength = std::isfinite(options.arcsinhStrength) && options.arcsinhStrength >= 0.0F
                               ? options.arcsinhStrength
                               : defaults.arcsinhStrength,
    };
    const auto stats = autoStatistics(image, safeOptions.highlightClip);
    if (!stats.valid) return {};
    const double background = stats.background;
    const double dataScale =
        std::max({1.0, std::fabs(background), std::fabs(stats.minimum), std::fabs(stats.maximum)});
    const double minimumSpan = dataScale * 1.0e-6;
    double black = std::max(stats.minimum, background - static_cast<double>(safeOptions.shadowsSigma) * stats.mad);
    double white = stats.white;
    if (!std::isfinite(black) || !std::isfinite(white) || white - black <= minimumSpan) {
        black = background - minimumSpan;
        white = background + minimumSpan;
    }
    const double floatLimit = std::numeric_limits<float>::max();
    black = std::clamp(black, -floatLimit, floatLimit);
    white = std::clamp(white, -floatLimit, floatLimit);
    float blackPoint = static_cast<float>(black);
    float whitePoint = static_cast<float>(white);
    if (!(whitePoint > blackPoint)) {
        const float center = static_cast<float>(std::clamp(background, -floatLimit, floatLimit));
        if (center >= std::numeric_limits<float>::max()) {
            blackPoint = std::nextafter(center, -std::numeric_limits<float>::infinity());
            whitePoint = center;
        } else if (center <= -std::numeric_limits<float>::max()) {
            blackPoint = center;
            whitePoint = std::nextafter(center, std::numeric_limits<float>::infinity());
        } else {
            blackPoint = std::nextafter(center, -std::numeric_limits<float>::infinity());
            whitePoint = std::nextafter(center, std::numeric_limits<float>::infinity());
        }
    }
    const double normalizedBackground = std::clamp(
        (background - blackPoint) / (static_cast<double>(whitePoint) - blackPoint), 0.001, 0.999
    );
    const double target = std::clamp(static_cast<double>(safeOptions.targetBackground), 0.001, 0.999);
    const double gamma = std::log(target) / std::log(normalizedBackground);
    const float midPoint = static_cast<float>(std::clamp(std::pow(0.5, 1.0 / gamma), 0.001, 0.999));

    return {
        .blackPoint = blackPoint,
        .midPoint = midPoint,
        .whitePoint = whitePoint,
        .arcsinhStrength = safeOptions.arcsinhStrength,
    };
}

StretchResult Stretch::applyAuto(const ImageBuffer& image, const AutoStretchOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return stretchError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (!std::isfinite(options.shadowsSigma) || !std::isfinite(options.targetBackground) ||
        !std::isfinite(options.highlightClip) || !std::isfinite(options.arcsinhStrength) ||
        options.shadowsSigma <= 0.0F || options.targetBackground <= 0.0F || options.targetBackground >= 1.0F ||
        options.highlightClip <= 0.0F || options.highlightClip > 1.0F || options.arcsinhStrength < 0.0F) {
        return stretchError("ArgumentInvalid", "Auto stretch options are outside valid ranges");
    }

    if (hasInvalidCoveredSample(image)) {
        return stretchError("ImageBufferInvalid", "Covered image colors and alpha must be finite");
    }

    if (!hasValidLuminanceSample(image)) {
        return stretchError("ImageCoverageEmpty", "Auto stretch requires at least one valid covered pixel");
    }

    return apply(image, estimateAuto(image, options));
}

} // namespace photonstack
