#include "photonstack/LocalContrast.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

LocalContrastResult localContrastError(std::string code, std::string message) {
    LocalContrastResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

std::vector<float> gaussianKernel(std::uint32_t radius) {
    const int r = static_cast<int>(radius);
    const float sigma = std::max(1.0F, static_cast<float>(radius) * 0.5F);
    std::vector<float> kernel(static_cast<std::size_t>(r * 2 + 1), 0.0F);
    float sum = 0.0F;
    for (int i = -r; i <= r; ++i) {
        const float value = std::exp(-(static_cast<float>(i * i)) / (2.0F * sigma * sigma));
        kernel[static_cast<std::size_t>(i + r)] = value;
        sum += value;
    }
    for (auto& value : kernel) {
        value /= sum;
    }
    return kernel;
}

ImageBuffer gaussianBlur(const ImageBuffer& image, std::uint32_t radius) {
    const auto kernel = gaussianKernel(radius);
    ImageBuffer temp = image;
    ImageBuffer output = image;
    const int r = static_cast<int>(radius);
    const auto colorChannels = detail::colorChannelCount(image);
    std::vector<float> horizontalCoverage(image.pixelCount(), 0.0F);

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            const auto offset = pixel * image.channels;
            double sums[3] = {0.0, 0.0, 0.0};
            double weightSum = 0.0;
            for (int k = -r; k <= r; ++k) {
                const auto samplePixel = detail::clampedPixelIndex(image, static_cast<int>(x) + k, static_cast<int>(y));
                const float coverage = detail::pixelCoverage(image, samplePixel);
                if (!detail::pixelHasValidColor(image, samplePixel) || coverage <= 1.0e-6F) {
                    continue;
                }
                const double weight = kernel[static_cast<std::size_t>(k + r)] * coverage;
                const auto sampleOffset = samplePixel * image.channels;
                for (std::uint16_t c = 0; c < colorChannels; ++c) {
                    sums[c] += static_cast<double>(image.pixels[sampleOffset + c]) * weight;
                }
                weightSum += weight;
            }
            if (weightSum > 0.0F) {
                horizontalCoverage[pixel] = static_cast<float>(weightSum);
                for (std::uint16_t c = 0; c < colorChannels; ++c) {
                    temp.pixels[offset + c] = static_cast<float>(sums[c] / weightSum);
                }
            }
        }
    }

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            const auto offset = pixel * image.channels;
            if (!detail::pixelHasValidColor(image, pixel)) {
                detail::clearMaskedPixel(output, pixel);
                continue;
            }

            double sums[3] = {0.0, 0.0, 0.0};
            double weightSum = 0.0;
            for (int k = -r; k <= r; ++k) {
                const auto samplePixel = detail::clampedPixelIndex(temp, static_cast<int>(x), static_cast<int>(y) + k);
                if (horizontalCoverage[samplePixel] <= 1.0e-6F) {
                    continue;
                }
                const double weight =
                    kernel[static_cast<std::size_t>(k + r)] * horizontalCoverage[samplePixel];
                const auto sampleOffset = samplePixel * temp.channels;
                for (std::uint16_t c = 0; c < colorChannels; ++c) {
                    sums[c] += static_cast<double>(temp.pixels[sampleOffset + c]) * weight;
                }
                weightSum += weight;
            }
            for (std::uint16_t c = 0; c < colorChannels; ++c) {
                output.pixels[offset + c] = weightSum > 0.0
                                                ? static_cast<float>(sums[c] / weightSum)
                                                : image.pixels[offset + c];
            }
        }
    }
    return output;
}

} // namespace

LocalContrastResult LocalContrast::apply(const ImageBuffer& image, const LocalContrastOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return localContrastError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (!std::isfinite(options.amount) || !std::isfinite(options.threshold) || options.amount < 0.0F ||
        options.amount > 2.0F || options.radius == 0 || options.radius > 64 || options.threshold < 0.0F) {
        return localContrastError("ArgumentInvalid", "Local contrast options are outside valid ranges");
    }
    if (detail::imageHasInvalidCoveredColor(image)) {
        return localContrastError("ImageBufferInvalid", "Covered image colors and alpha must be finite");
    }

    const auto blurred = gaussianBlur(image, options.radius);
    ImageBuffer output = image;
    const auto colorChannels = detail::colorChannelCount(image);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        if (!detail::pixelHasValidColor(image, pixel)) {
            detail::clearMaskedPixel(output, pixel);
            continue;
        }
        for (std::uint16_t c = 0; c < colorChannels; ++c) {
            const double detailValue =
                static_cast<double>(image.pixels[offset + c]) - blurred.pixels[offset + c];
            const double adjusted = std::fabs(detailValue) >= options.threshold
                                        ? static_cast<double>(image.pixels[offset + c]) +
                                              detailValue * options.amount
                                        : image.pixels[offset + c];
            if (!options.clampOutput &&
                (!std::isfinite(adjusted) || std::fabs(adjusted) > std::numeric_limits<float>::max())) {
                return localContrastError("ImageValueInvalid", "Local contrast output exceeds Float32 range");
            }
            output.pixels[offset + c] = options.clampOutput
                                            ? static_cast<float>(std::clamp(adjusted, 0.0, 1.0))
                                            : static_cast<float>(adjusted);
        }
    }

    LocalContrastResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

} // namespace photonstack
