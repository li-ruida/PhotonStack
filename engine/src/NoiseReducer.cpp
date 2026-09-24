#include "photonstack/NoiseReducer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "MaskedImageSampling.hpp"
#include "DisplayGamut.hpp"

namespace photonstack {
namespace {

NoiseReductionResult noiseError(std::string code, std::string message) {
    NoiseReductionResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

} // namespace

NoiseReductionResult NoiseReducer::reduce(const ImageBuffer& image, const NoiseReductionOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return noiseError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (!std::isfinite(options.amount) || !std::isfinite(options.chromaAmount) ||
        !std::isfinite(options.edgeThreshold) || options.amount < 0.0F || options.amount > 1.0F ||
        options.chromaAmount < 0.0F || options.chromaAmount > 1.0F || options.radius == 0 ||
        options.radius > 8 || options.edgeThreshold < 0.0F) {
        return noiseError("ArgumentInvalid", "Noise reduction options are outside valid ranges");
    }
    if (detail::imageHasInvalidCoveredColor(image)) {
        return noiseError("ImageBufferInvalid", "Covered image colors and alpha must be finite");
    }

    ImageBuffer output = image;
    const int radius = static_cast<int>(options.radius);
    const auto colorChannels = detail::colorChannelCount(image);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            const auto offset = pixel * image.channels;
            if (!detail::pixelHasValidColor(image, pixel)) {
                detail::clearMaskedPixel(output, pixel);
                continue;
            }

            float blurredChannels[3] = {0.0F, 0.0F, 0.0F};
            for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                const float center = image.pixels[offset + channel];
                double sum = 0.0;
                double weightSum = 0.0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        float sample = 0.0F;
                        float coverage = 0.0F;
                        if (!detail::sampleClampedCovered(
                                image,
                                static_cast<int>(x) + dx,
                                static_cast<int>(y) + dy,
                                channel,
                                sample,
                                coverage
                            )) {
                            continue;
                        }
                        const double delta = std::fabs(static_cast<double>(sample) - center);
                        const double edgeWeight = delta > options.edgeThreshold
                                                      ? 0.0
                                                      : 1.0 - delta / std::max(1.0e-6, static_cast<double>(options.edgeThreshold));
                        const double weight = edgeWeight * coverage;
                        sum += static_cast<double>(sample) * weight;
                        weightSum += weight;
                    }
                }
                blurredChannels[channel] =
                    weightSum <= 0.0 ? center : static_cast<float>(sum / weightSum);
            }

            if (colorChannels >= 3) {
                const double centerY = 0.2126 * image.pixels[offset] + 0.7152 * image.pixels[offset + 1] +
                                       0.0722 * image.pixels[offset + 2];
                const double blurredY = 0.2126 * blurredChannels[0] + 0.7152 * blurredChannels[1] +
                                        0.0722 * blurredChannels[2];
                const double outputY = centerY * (1.0 - options.amount) + blurredY * options.amount;
                double adjustedChannels[3];
                bool centerInGamut = true;
                for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                    const double centerChroma = image.pixels[offset + channel] - centerY;
                    const double blurredChroma = blurredChannels[channel] - blurredY;
                    const double outputChroma =
                        centerChroma * (1.0 - options.chromaAmount) + blurredChroma * options.chromaAmount;
                    const double adjusted = outputY + outputChroma;
                    adjustedChannels[channel] = adjusted;
                    centerInGamut = centerInGamut && image.pixels[offset + channel] >= 0 &&
                                    image.pixels[offset + channel] <= 1;
                    if (!options.clampOutput &&
                        (!std::isfinite(adjusted) || std::fabs(adjusted) > std::numeric_limits<float>::max())) {
                        return noiseError("ImageValueInvalid", "Noise reduction output exceeds Float32 range");
                    }
                }
                double adjustment = 1;
                if (options.clampOutput && centerInGamut) {
                    double available = std::numeric_limits<double>::infinity();
                    for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                        const double center = image.pixels[offset + channel];
                        const double delta = adjustedChannels[channel] - center;
                        // Attenuate the whole RGB update, rather than clipping
                        // channels separately. Chroma-only denoising therefore
                        // retains luminance even next to a bright colored star.
                        if (delta > 0)
                            available = std::min(available,
                                std::max(0.0, 1 - detail::displayCodeStep - center) / delta);
                        if (delta < 0)
                            available = std::min(available,
                                std::max(0.0, center - detail::displayCodeStep) / -delta);
                    }
                    adjustment = detail::softGamutScale(available);
                }
                for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                    const double center = image.pixels[offset + channel];
                    const double adjusted = adjustment == 1 ? adjustedChannels[channel]
                        : center + adjustment * (adjustedChannels[channel] - center);
                    output.pixels[offset + channel] = options.clampOutput
                                                          ? static_cast<float>(std::clamp(adjusted, 0.0, 1.0))
                                                          : static_cast<float>(adjusted);
                }
            } else {
                for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                    const double center = image.pixels[offset + channel];
                    const double blurred = blurredChannels[channel];
                    const double adjusted = center * (1.0 - options.amount) + blurred * options.amount;
                    if (!options.clampOutput &&
                        (!std::isfinite(adjusted) || std::fabs(adjusted) > std::numeric_limits<float>::max())) {
                        return noiseError("ImageValueInvalid", "Noise reduction output exceeds Float32 range");
                    }
                    output.pixels[offset + channel] = options.clampOutput
                                                          ? static_cast<float>(std::clamp(adjusted, 0.0, 1.0))
                                                          : static_cast<float>(adjusted);
                }
            }
        }
    }

    NoiseReductionResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

} // namespace photonstack
