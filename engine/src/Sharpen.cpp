#include "photonstack/Sharpen.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

SharpenResult sharpenError(std::string code, std::string message) {
    SharpenResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

} // namespace

SharpenResult Sharpen::unsharpMask(const ImageBuffer& image, const SharpenOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return sharpenError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (!std::isfinite(options.amount) || !std::isfinite(options.threshold) || options.amount < 0.0F ||
        options.amount > 2.0F || options.radius == 0 || options.radius > 8 || options.threshold < 0.0F) {
        return sharpenError("ArgumentInvalid", "Sharpen options are outside valid ranges");
    }
    if (detail::imageHasInvalidCoveredColor(image)) {
        return sharpenError("ImageBufferInvalid", "Covered image colors and alpha must be finite");
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
            for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
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
                        sum += static_cast<double>(sample) * coverage;
                        weightSum += coverage;
                    }
                }
                const float center = image.pixels[offset + channel];
                const double blurred = weightSum <= 0.0 ? center : sum / weightSum;
                const double detailValue = static_cast<double>(center) - blurred;
                const double adjusted = std::fabs(detailValue) < options.threshold
                                            ? center
                                            : static_cast<double>(center) + detailValue * options.amount;
                if (!options.clampOutput &&
                    (!std::isfinite(adjusted) || std::fabs(adjusted) > std::numeric_limits<float>::max())) {
                    return sharpenError("ImageValueInvalid", "Sharpen output exceeds Float32 range");
                }
                output.pixels[offset + channel] = options.clampOutput
                                                      ? static_cast<float>(std::clamp(adjusted, 0.0, 1.0))
                                                      : static_cast<float>(adjusted);
            }
        }
    }

    SharpenResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

} // namespace photonstack
