#include "photonstack/Sharpen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include "photonstack/ParallelRanges.hpp"
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
    std::atomic<bool> overflow{false};
    detail::parallelRows(image.width, image.height, [&](int y) noexcept {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            const auto offset = pixel * image.channels;
            if (!detail::pixelHasValidColor(image, pixel)) {
                detail::clearMaskedPixel(output, pixel);
                continue;
            }
            std::array<double, 3> sums{};
            double weightSum = 0;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const auto neighbor = detail::clampedPixelIndex(image, static_cast<int>(x) + dx, y + dy);
                    const float coverage = detail::pixelCoverage(image, neighbor);
                    if (coverage <= 1e-6F) continue;
                    const auto base = neighbor * image.channels;
                    for (std::uint16_t c = 0; c < colorChannels; ++c)
                        sums[c] += static_cast<double>(image.pixels[base + c]) * coverage;
                    weightSum += coverage;
                }
            }
            for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                const float center = image.pixels[offset + channel];
                const double blurred = weightSum <= 0.0 ? center : sums[channel] / weightSum;
                const double detailValue = static_cast<double>(center) - blurred;
                const double adjusted = std::fabs(detailValue) < options.threshold
                                            ? center
                                            : static_cast<double>(center) + detailValue * options.amount;
                if (!options.clampOutput &&
                    (!std::isfinite(adjusted) || std::fabs(adjusted) > std::numeric_limits<float>::max())) {
                    overflow.store(true, std::memory_order_relaxed);
                    return;
                }
                output.pixels[offset + channel] = options.clampOutput
                                                      ? static_cast<float>(std::clamp(adjusted, 0.0, 1.0))
                                                      : static_cast<float>(adjusted);
            }
        }
    });
    if (overflow.load(std::memory_order_relaxed)) return sharpenError("ImageValueInvalid", "Sharpen output exceeds Float32 range");

    SharpenResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

} // namespace photonstack
