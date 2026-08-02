#include "photonstack/FrameQualityAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

FrameQualityResult qualityError(std::string code, std::string message) {
    FrameQualityResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
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
    float value = 0.0F;
    double weight = 0.0;
};

float weightedMedian(std::vector<WeightedSample>& values) {
    if (values.empty()) {
        return 0.0F;
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
            return static_cast<float>((static_cast<double>(values[index].value) + values[index + 1].value) * 0.5);
        }
        return values[index].value;
    }
    return values.back().value;
}

FrameQualityResult analyzeLuminanceBuffer(std::uint32_t width, std::uint32_t height,
                                          std::span<const float> luminance,
                                          std::span<const float> coverage,
                                          const FrameQualityOptions& options) {
    if (width == 0 || height == 0 || luminance.size() != static_cast<std::size_t>(width) * height) {
        return qualityError("ImageBufferInvalid", "Input luminance must match the image dimensions");
    }
    if (!coverage.empty() && coverage.size() != luminance.size()) {
        return qualityError("ImageBufferInvalid", "Input coverage must match the image dimensions");
    }
    if (std::any_of(coverage.begin(), coverage.end(), [](float value) { return !std::isfinite(value); })) {
        return qualityError("ImageBufferInvalid", "Input coverage must be finite");
    }
    if (!std::isfinite(options.saturationThreshold) || options.saturationThreshold <= 0.0F ||
        options.saturationThreshold > 1.0F) {
        return qualityError("ArgumentInvalid", "saturationThreshold must be in (0, 1]");
    }

    const auto coverageAt = [&](std::size_t index) {
        return coverage.empty() ? 1.0F : std::clamp(coverage[index], 0.0F, 1.0F);
    };
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    double totalCoverage = 0.0;
    for (std::size_t index = 0; index < luminance.size(); ++index) {
        const float weight = coverageAt(index);
        if (!std::isfinite(luminance[index]) || weight <= 1.0e-6F) {
            continue;
        }
        minimum = std::min(minimum, static_cast<double>(luminance[index]));
        maximum = std::max(maximum, static_cast<double>(luminance[index]));
        totalCoverage += weight;
    }
    if (!(totalCoverage > 1.0e-6) || !std::isfinite(totalCoverage)) {
        return qualityError("ImageCoverageEmpty", "Frame quality analysis requires at least one valid pixel");
    }

    const double dataScale = std::max({1.0, std::fabs(minimum), std::fabs(maximum)});
    const double span = maximum - minimum;
    const bool hasRange = std::isfinite(span) && span > dataScale * 1.0e-6;
    std::vector<float> normalized(luminance.size(), std::numeric_limits<float>::quiet_NaN());
    std::vector<float> normalizedCoverage(luminance.size(), 0.0F);
    std::vector<WeightedSample> backgroundSamples;
    backgroundSamples.reserve(luminance.size());
    double saturatedCoverage = 0.0;
    for (std::size_t index = 0; index < luminance.size(); ++index) {
        const float weight = coverageAt(index);
        if (!std::isfinite(luminance[index]) || weight <= 1.0e-6F) {
            continue;
        }
        const double normalizedValue =
            hasRange ? std::clamp((static_cast<double>(luminance[index]) - minimum) / span, 0.0, 1.0) : 0.5;
        const float value = static_cast<float>(normalizedValue);
        normalized[index] = value;
        normalizedCoverage[index] = weight;
        backgroundSamples.push_back({.value = value, .weight = weight});
        if (value >= options.saturationThreshold) {
            saturatedCoverage += weight;
        }
    }
    const float background = weightedMedian(backgroundSamples);
    for (auto& sample : backgroundSamples) {
        sample.value = std::fabs(sample.value - background);
    }
    const float noise = weightedMedian(backgroundSamples) * 1.4826F;

    const StarDetector detector;
    const auto stars = detector.detectLuminanceWithCoverage(
        width, height, normalized, normalizedCoverage, options.starDetection
    );
    if (!stars.ok) {
        return qualityError(stars.errorCode, stars.message);
    }

    std::vector<WeightedSample> fwhmValues;
    std::vector<WeightedSample> eccentricityValues;
    fwhmValues.reserve(stars.stars.size());
    eccentricityValues.reserve(stars.stars.size());
    for (const auto& star : stars.stars) {
        fwhmValues.push_back({.value = star.fwhm, .weight = 1.0});
        eccentricityValues.push_back({.value = star.eccentricity, .weight = 1.0});
    }
    const float medianFwhm = weightedMedian(fwhmValues);
    const float medianEccentricity = weightedMedian(eccentricityValues);
    const float saturatedFraction = static_cast<float>(saturatedCoverage / totalCoverage);

    const float starScore = std::clamp(static_cast<float>(stars.stars.size()) / 100.0F, 0.0F, 1.0F);
    const float fwhmScore = stars.stars.empty() ? 0.0F : std::clamp(4.0F / (medianFwhm + 1.0F), 0.0F, 1.0F);
    const float eccentricityScore =
        stars.stars.empty() ? 0.0F : std::clamp(1.0F - medianEccentricity, 0.0F, 1.0F);
    const float noiseScore = std::clamp(1.0F - noise * 8.0F, 0.0F, 1.0F);
    const float saturationScore = std::clamp(1.0F - saturatedFraction * 20.0F, 0.0F, 1.0F);

    FrameQualityResult result;
    result.ok = true;
    result.starCount = stars.stars.size();
    result.medianFwhm = medianFwhm;
    result.medianEccentricity = medianEccentricity;
    result.background = background;
    result.noise = noise;
    result.saturatedFraction = saturatedFraction;
    result.score =
        starScore * 0.25F + fwhmScore * 0.25F + eccentricityScore * 0.2F + noiseScore * 0.2F + saturationScore * 0.1F;
    return result;
}

} // namespace

FrameQualityResult FrameQualityAnalyzer::analyze(const ImageBuffer& image, const FrameQualityOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return qualityError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (!std::isfinite(options.saturationThreshold) || options.saturationThreshold <= 0.0F ||
        options.saturationThreshold > 1.0F) {
        return qualityError("ArgumentInvalid", "saturationThreshold must be in (0, 1]");
    }

    std::vector<float> luminance(image.pixelCount(), std::numeric_limits<float>::quiet_NaN());
    std::vector<float> coverage(image.pixelCount(), 0.0F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        float pixelCoverage = 1.0F;
        if (image.channels == 4) {
            const float alpha = image.pixels[offset + 3];
            if (!std::isfinite(alpha)) {
                return qualityError("ImageBufferInvalid", "Input image alpha must be finite");
            }
            pixelCoverage = std::clamp(alpha, 0.0F, 1.0F);
        }
        if (pixelCoverage <= 1.0e-6F) {
            continue;
        }
        for (std::uint16_t channel = 0; channel < detail::colorChannelCount(image); ++channel) {
            if (!std::isfinite(image.pixels[offset + channel])) {
                return qualityError("ImageBufferInvalid", "Covered image colors must be finite");
            }
        }
        const double value = luminanceAt(image, pixel);
        if (!std::isfinite(value) || std::fabs(value) > std::numeric_limits<float>::max()) {
            return qualityError("ImageBufferInvalid", "Covered image luminance must be finite and representable");
        }
        luminance[pixel] = static_cast<float>(value);
        coverage[pixel] = pixelCoverage;
    }
    return analyzeLuminanceBuffer(image.width, image.height, luminance, coverage, options);
}

FrameQualityResult FrameQualityAnalyzer::analyzeLuminance(std::uint32_t width, std::uint32_t height,
                                                          std::span<const float> luminance,
                                                          const FrameQualityOptions& options) const {
    return analyzeLuminanceBuffer(width, height, luminance, {}, options);
}

} // namespace photonstack
