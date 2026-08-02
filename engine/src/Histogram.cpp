#include "photonstack/Histogram.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

HistogramResult histogramError(std::string code, std::string message) {
    HistogramResult result;
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

} // namespace

HistogramResult Histogram::luminance(const ImageBuffer& image, const HistogramOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return histogramError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    constexpr std::size_t maximumBins = 1U << 20U;
    if (options.bins == 0 || options.bins > maximumBins) {
        return histogramError("ArgumentInvalid", "Histogram bin count must be between 1 and 1048576");
    }

    HistogramResult result;
    result.ok = true;
    result.bins.assign(options.bins, 0);
    result.minimum = std::numeric_limits<float>::max();
    result.maximum = std::numeric_limits<float>::lowest();

    double sum = 0.0;
    double totalCoverage = 0.0;
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        float coverage = 1.0F;
        if (image.channels == 4) {
            const float alpha = image.pixels[offset + 3];
            if (!std::isfinite(alpha)) {
                return histogramError("ImageBufferInvalid", "Input image alpha must be finite");
            }
            coverage = std::clamp(alpha, 0.0F, 1.0F);
        }
        if (coverage <= 1.0e-6F) {
            continue;
        }
        for (std::uint16_t channel = 0; channel < detail::colorChannelCount(image); ++channel) {
            if (!std::isfinite(image.pixels[offset + channel])) {
                return histogramError("ImageBufferInvalid", "Covered image colors must be finite");
            }
        }
        const double rawValue = luminanceAt(image, pixel);
        if (!std::isfinite(rawValue)) {
            return histogramError("ImageBufferInvalid", "Covered image luminance must be finite");
        }
        const double value = std::clamp(rawValue, 0.0, 1.0);
        result.minimum = std::min(result.minimum, static_cast<float>(value));
        result.maximum = std::max(result.maximum, static_cast<float>(value));
        sum += value * coverage;
        totalCoverage += coverage;

        const auto index = std::min<std::size_t>(
            options.bins - 1, static_cast<std::size_t>(std::floor(value * static_cast<double>(options.bins))));
        result.bins[index] += coverage;
    }

    if (!(totalCoverage > 1.0e-6) || !std::isfinite(totalCoverage)) {
        return histogramError("ImageCoverageEmpty", "Histogram requires at least one valid covered pixel");
    }
    result.mean = static_cast<float>(sum / totalCoverage);
    return result;
}

} // namespace photonstack
