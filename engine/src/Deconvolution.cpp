#include "photonstack/Deconvolution.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

DeconvolutionResult deconvolutionError(std::string code, std::string message) {
    DeconvolutionResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

std::vector<float> gaussianKernel(std::uint32_t radius, float sigma) {
    const int r = static_cast<int>(radius);
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

ImageBuffer gaussianBlur(const ImageBuffer& image, const std::vector<float>& kernel, std::uint32_t radius) {
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

DeconvolutionResult Deconvolution::richardsonLucy(const ImageBuffer& image, const DeconvolutionOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return deconvolutionError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (!std::isfinite(options.sigma) || !std::isfinite(options.damping) || options.iterations == 0 ||
        options.iterations > 100 || options.radius == 0 || options.radius > 8 || options.sigma <= 0.0F ||
        options.damping < 0.0F || options.damping > 1.0F) {
        return deconvolutionError("ArgumentInvalid", "Deconvolution options are outside valid ranges");
    }
    if (detail::imageHasInvalidCoveredColor(image)) {
        return deconvolutionError("ImageBufferInvalid", "Covered image colors and alpha must be finite");
    }

    const auto kernel = gaussianKernel(options.radius, options.sigma);
    ImageBuffer estimate = image;
    const auto colorChannels = detail::colorChannelCount(image);
    double channelScale[3] = {0.0, 0.0, 0.0};
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (!detail::pixelHasValidColor(image, pixel)) {
            detail::clearMaskedPixel(estimate, pixel);
            continue;
        }
        if (!options.clampOutput) {
            const auto offset = pixel * image.channels;
            for (std::uint16_t c = 0; c < colorChannels; ++c) {
                if (image.pixels[offset + c] < 0.0F) {
                    return deconvolutionError(
                        "ImageValueDomainInvalid",
                        "Richardson-Lucy deconvolution requires non-negative scientific samples"
                    );
                }
                channelScale[c] = std::max(channelScale[c], static_cast<double>(image.pixels[offset + c]));
            }
        } else {
            const auto offset = pixel * image.channels;
            for (std::uint16_t c = 0; c < colorChannels; ++c) {
                channelScale[c] = std::max(channelScale[c], std::fabs(static_cast<double>(image.pixels[offset + c])));
            }
        }
    }
    double denominatorFloor[3] = {0.0, 0.0, 0.0};
    for (std::uint16_t c = 0; c < colorChannels; ++c) {
        denominatorFloor[c] = std::max(
            channelScale[c] * 1.0e-6,
            static_cast<double>(std::numeric_limits<float>::denorm_min())
        );
    }

    for (std::uint32_t iteration = 0; iteration < options.iterations; ++iteration) {
        const auto blurred = gaussianBlur(estimate, kernel, options.radius);
        ImageBuffer ratio = image;
        for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
            const auto offset = pixel * image.channels;
            if (!detail::pixelHasValidColor(image, pixel)) {
                detail::clearMaskedPixel(ratio, pixel);
                continue;
            }
            for (std::uint16_t c = 0; c < colorChannels; ++c) {
                const double observed = image.pixels[offset + c];
                const double modeled = std::max(denominatorFloor[c], static_cast<double>(blurred.pixels[offset + c]));
                const double rawRatio = observed / modeled;
                ratio.pixels[offset + c] = static_cast<float>(std::clamp(
                    1.0 + (rawRatio - 1.0) * (1.0 - options.damping), 0.0, 4.0
                ));
            }
        }
        const auto correction = gaussianBlur(ratio, kernel, options.radius);
        for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
            const auto offset = pixel * image.channels;
            if (!detail::pixelHasValidColor(image, pixel) || !detail::pixelHasValidColor(correction, pixel)) {
                detail::clearMaskedPixel(estimate, pixel);
                continue;
            }
            for (std::uint16_t c = 0; c < colorChannels; ++c) {
                const double updated = std::max(
                    0.0,
                    static_cast<double>(estimate.pixels[offset + c]) * correction.pixels[offset + c]
                );
                if (!options.clampOutput &&
                    (!std::isfinite(updated) || updated > std::numeric_limits<float>::max())) {
                    return deconvolutionError("ImageValueInvalid", "Deconvolution output exceeds Float32 range");
                }
                estimate.pixels[offset + c] = options.clampOutput
                                                  ? static_cast<float>(std::clamp(updated, 0.0, 1.0))
                                                  : static_cast<float>(updated);
            }
        }
    }

    DeconvolutionResult result;
    result.ok = true;
    result.image = std::move(estimate);
    return result;
}

} // namespace photonstack
