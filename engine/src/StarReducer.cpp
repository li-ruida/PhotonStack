#include "photonstack/StarReducer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

StarReductionResult reductionError(std::string code, std::string message) {
    StarReductionResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

float localMinimum(const ImageBuffer& image, std::uint32_t x, std::uint32_t y, std::uint16_t channel,
                   std::uint32_t radius) {
    const auto centerPixel = static_cast<std::size_t>(y) * image.width + x;
    std::vector<detail::WeightedValue> samples;
    samples.reserve(static_cast<std::size_t>(radius * 2 + 1) * (radius * 2 + 1));
    const auto searchRadius = static_cast<int>(radius);
    for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
        for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
            const int sampleX = static_cast<int>(x) + dx;
            const int sampleY = static_cast<int>(y) + dy;
            if (sampleX < 0 || sampleY < 0 || sampleX >= static_cast<int>(image.width) ||
                sampleY >= static_cast<int>(image.height)) {
                continue;
            }
            const auto samplePixel = static_cast<std::size_t>(static_cast<std::uint32_t>(sampleY)) * image.width +
                                     static_cast<std::uint32_t>(sampleX);
            if (!detail::pixelHasValidColor(image, samplePixel)) {
                continue;
            }
            const auto offset = samplePixel * image.channels + channel;
            samples.push_back({
                .value = image.pixels[offset],
                .weight = detail::pixelCoverage(image, samplePixel),
            });
        }
    }
    return samples.empty()
               ? image.pixels[centerPixel * image.channels + channel]
               : static_cast<float>(detail::weightedPercentile(std::move(samples), 0.05));
}

float localMean(const ImageBuffer& image, std::uint32_t x, std::uint32_t y, std::uint16_t channel,
                std::uint32_t radius) {
    double sum = 0.0;
    double weightSum = 0.0;
    const auto searchRadius = static_cast<int>(radius);
    for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
        for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
            const int sampleX = static_cast<int>(x) + dx;
            const int sampleY = static_cast<int>(y) + dy;
            if (sampleX < 0 || sampleY < 0 || sampleX >= static_cast<int>(image.width) ||
                sampleY >= static_cast<int>(image.height)) {
                continue;
            }
            const auto samplePixel = static_cast<std::size_t>(static_cast<std::uint32_t>(sampleY)) * image.width +
                                     static_cast<std::uint32_t>(sampleX);
            if (!detail::pixelHasValidColor(image, samplePixel)) {
                continue;
            }
            const auto offset = samplePixel * image.channels + channel;
            const double weight = detail::pixelCoverage(image, samplePixel);
            sum += static_cast<double>(image.pixels[offset]) * weight;
            weightSum += weight;
        }
    }
    return weightSum <= 0.0
               ? image.pixels[(static_cast<std::size_t>(y) * image.width + x) * image.channels + channel]
               : static_cast<float>(sum / weightSum);
}

float luminanceAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
    return image.pixels[offset] * 0.2126F + image.pixels[offset + 1] * 0.7152F + image.pixels[offset + 2] * 0.0722F;
}

float smoothStep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / std::max(1.0e-6F, edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

float estimateBackground(const ImageBuffer& image, int centerX, int centerY, std::uint32_t radius) {
    std::vector<detail::WeightedValue> samples;
    samples.reserve(static_cast<std::size_t>(radius * radius));
    const float radiusF = static_cast<float>(radius);
    const float innerRadius = radiusF * 0.62F;
    for (int dy = -static_cast<int>(radius); dy <= static_cast<int>(radius); ++dy) {
        for (int dx = -static_cast<int>(radius); dx <= static_cast<int>(radius); ++dx) {
            const int sampleX = centerX + dx;
            const int sampleY = centerY + dy;
            if (sampleX < 0 || sampleY < 0 || sampleX >= static_cast<int>(image.width) ||
                sampleY >= static_cast<int>(image.height)) {
                continue;
            }
            const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (distance < innerRadius || distance > radiusF) {
                continue;
            }
            const auto samplePixel = static_cast<std::size_t>(static_cast<std::uint32_t>(sampleY)) * image.width +
                                     static_cast<std::uint32_t>(sampleX);
            if (!detail::pixelHasValidColor(image, samplePixel)) {
                continue;
            }
            samples.push_back({
                .value = luminanceAt(image, static_cast<std::uint32_t>(sampleX),
                                     static_cast<std::uint32_t>(sampleY)),
                .weight = detail::pixelCoverage(image, samplePixel),
            });
        }
    }
    return static_cast<float>(detail::weightedPercentile(std::move(samples), 0.35));
}

void reduceEdgeProfiles(
    const ImageBuffer& source,
    const ImageBuffer& detectionImage,
    ImageBuffer& output,
    const StarReductionOptions& options
) {
    StarDetectionOptions detectionOptions = options.mask.detection;
    detectionOptions.border = std::max<std::uint32_t>(detectionOptions.border, options.radius + 9);
    detectionOptions.maxStars = std::max<std::size_t>(detectionOptions.maxStars, 2500);
    detectionOptions.sigmaThreshold = std::min(detectionOptions.sigmaThreshold, 2.6F);

    const StarDetector detector;
    const auto detected = detector.detect(detectionImage, detectionOptions);
    if (!detected.ok) {
        return;
    }

    const float imageCenterX = static_cast<float>(source.width - 1) * 0.5F;
    const float imageCenterY = static_cast<float>(source.height - 1) * 0.5F;
    const float maxCenterDistance =
        std::max(1.0F, std::sqrt(imageCenterX * imageCenterX + imageCenterY * imageCenterY));

    for (const auto& star : detected.stars) {
        const float centerDistance = std::sqrt((star.x - imageCenterX) * (star.x - imageCenterX) +
                                               (star.y - imageCenterY) * (star.y - imageCenterY));
        const float edgeWeight = smoothStep(0.38F, 0.95F, centerDistance / maxCenterDistance);
        if (edgeWeight <= 0.02F) {
            continue;
        }

        const auto supportRadius = static_cast<std::uint32_t>(
            std::max(5.0F, static_cast<float>(options.radius + 5) + edgeWeight * 4.0F));
        const int centerX = static_cast<int>(std::lround(star.x));
        const int centerY = static_cast<int>(std::lround(star.y));
        if (centerX < static_cast<int>(supportRadius) || centerY < static_cast<int>(supportRadius) ||
            centerX + static_cast<int>(supportRadius) >= static_cast<int>(source.width) ||
            centerY + static_cast<int>(supportRadius) >= static_cast<int>(source.height)) {
            continue;
        }

        const float background = estimateBackground(source, centerX, centerY, supportRadius);
        const float supportRadiusF = static_cast<float>(supportRadius);
        float peakLuminance = 0.0F;
        double weightedRadiusSum = 0.0;
        double weightSum = 0.0;
        for (int dy = -static_cast<int>(supportRadius); dy <= static_cast<int>(supportRadius); ++dy) {
            for (int dx = -static_cast<int>(supportRadius); dx <= static_cast<int>(supportRadius); ++dx) {
                const int px = centerX + dx;
                const int py = centerY + dy;
                const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (distance > supportRadiusF) {
                    continue;
                }
                const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(py)) * source.width +
                                   static_cast<std::uint32_t>(px);
                if (!detail::pixelHasValidColor(source, pixel)) {
                    continue;
                }
                const float luminance =
                    luminanceAt(source, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
                peakLuminance = std::max(peakLuminance, luminance);
            }
        }
        const float peakSignal = peakLuminance - background;
        if (peakSignal <= 1.0e-4F) {
            continue;
        }
        for (int dy = -static_cast<int>(supportRadius); dy <= static_cast<int>(supportRadius); ++dy) {
            for (int dx = -static_cast<int>(supportRadius); dx <= static_cast<int>(supportRadius); ++dx) {
                const int px = centerX + dx;
                const int py = centerY + dy;
                const float distanceSquared = static_cast<float>(dx * dx + dy * dy);
                if (std::sqrt(distanceSquared) > supportRadiusF) {
                    continue;
                }
                const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(py)) * source.width +
                                   static_cast<std::uint32_t>(px);
                if (!detail::pixelHasValidColor(source, pixel)) {
                    continue;
                }
                const float luminance =
                    luminanceAt(source, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
                const float signal = std::max(0.0F, luminance - background);
                if (signal <= peakSignal * 0.025F) {
                    continue;
                }
                const double weight = static_cast<double>(signal) * detail::pixelCoverage(source, pixel);
                weightedRadiusSum += static_cast<double>(distanceSquared) * weight;
                weightSum += weight;
            }
        }
        if (weightSum <= 1.0e-8) {
            continue;
        }
        const float sourceSigma =
            std::clamp(static_cast<float>(std::sqrt(weightedRadiusSum / (2.0 * weightSum))), 0.85F,
                       supportRadiusF * 0.45F);
        const float shrinkFactor = 1.0F - options.amount * (0.18F + 0.20F * edgeWeight);
        const float targetSigma = std::max(0.72F, sourceSigma * std::clamp(shrinkFactor, 0.58F, 0.95F));

        for (int dy = -static_cast<int>(supportRadius); dy <= static_cast<int>(supportRadius); ++dy) {
            for (int dx = -static_cast<int>(supportRadius); dx <= static_cast<int>(supportRadius); ++dx) {
                const int px = centerX + dx;
                const int py = centerY + dy;
                if (px < 0 || py < 0 || px >= static_cast<int>(source.width) ||
                    py >= static_cast<int>(source.height)) {
                    continue;
                }

                const float offsetX = static_cast<float>(px) - star.x;
                const float offsetY = static_cast<float>(py) - star.y;
                const float distance = std::sqrt(offsetX * offsetX + offsetY * offsetY);
                if (distance > supportRadiusF) {
                    continue;
                }
                const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(py)) * source.width +
                                   static_cast<std::uint32_t>(px);
                if (!detail::pixelHasValidColor(source, pixel)) {
                    continue;
                }

                const float currentLuminance =
                    luminanceAt(output, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
                if (currentLuminance <= background + 1.0e-5F || currentLuminance <= 1.0e-6F) {
                    continue;
                }

                const float targetSignal =
                    peakSignal * std::exp(-(distance * distance) / (2.0F * targetSigma * targetSigma));
                const float targetLuminance = background + targetSignal;
                if (targetLuminance >= currentLuminance) {
                    continue;
                }

                const float profileWeight = smoothStep(targetSigma * 0.55F, supportRadiusF * 0.92F, distance);
                if (profileWeight <= 0.0F) {
                    continue;
                }

                float blend = options.amount * (0.38F + 0.72F * edgeWeight) * profileWeight *
                              detail::pixelCoverage(source, pixel);
                const auto detectionPixel = static_cast<std::size_t>(static_cast<std::uint32_t>(py)) *
                                                detectionImage.width +
                                            static_cast<std::uint32_t>(px);
                const float protectionLuminance = detail::pixelHasValidColor(detectionImage, detectionPixel)
                                                      ? luminanceAt(
                                                            detectionImage,
                                                            static_cast<std::uint32_t>(px),
                                                            static_cast<std::uint32_t>(py)
                                                        )
                                                      : 0.0F;
                if (protectionLuminance >= options.haloProtection) {
                    blend *= 0.55F;
                }
                blend = std::clamp(blend, 0.0F, 0.78F);
                if (blend <= 0.0F) {
                    continue;
                }

                const float newLuminance = currentLuminance - (currentLuminance - targetLuminance) * blend;
                const float scale = std::clamp(newLuminance / currentLuminance, 0.0F, 1.0F);
                const auto pixelOffset =
                    (static_cast<std::size_t>(static_cast<std::uint32_t>(py)) * source.width +
                     static_cast<std::uint32_t>(px)) *
                    source.channels;
                for (std::uint16_t channel = 0; channel < std::min<std::uint16_t>(3, source.channels); ++channel) {
                    output.pixels[pixelOffset + channel] =
                        std::min(output.pixels[pixelOffset + channel], output.pixels[pixelOffset + channel] * scale);
                }
            }
        }
    }
}

} // namespace

StarReductionResult StarReducer::reduce(const ImageBuffer& image, const StarReductionOptions& options) const {
    return reduceUsingDetectionImage(image, image, options);
}

StarReductionResult StarReducer::reduceUsingDetectionImage(
    const ImageBuffer& image,
    const ImageBuffer& detectionImage,
    const StarReductionOptions& options
) const {
    if (image.empty() || image.channels < 3 || image.pixels.size() != image.sampleCount() ||
        detectionImage.empty() || detectionImage.channels < 3 ||
        detectionImage.pixels.size() != detectionImage.sampleCount()) {
        return reductionError(
            "ImageBufferInvalid",
            "Input and detection image must be non-empty RGB/RGBA float buffers"
        );
    }
    if (image.width != detectionImage.width || image.height != detectionImage.height) {
        return reductionError("ImageDimensionsMismatch", "Input and detection image dimensions must match");
    }
    if (!std::isfinite(options.amount) || options.amount < 0.0F || options.amount > 1.0F) {
        return reductionError("ArgumentInvalid", "Star reduction amount must be between 0 and 1");
    }
    if (options.radius == 0 || options.radius > 32) {
        return reductionError("ArgumentInvalid", "Star reduction radius must be between 1 and 32");
    }
    if (!std::isfinite(options.haloProtection) || options.haloProtection < 0.0F ||
        options.haloProtection > 1.0F) {
        return reductionError("ArgumentInvalid", "haloProtection must be between 0 and 1");
    }
    if (detail::imageHasInvalidCoveredColor(image) || detail::imageHasInvalidCoveredColor(detectionImage)) {
        return reductionError("ImageBufferInvalid", "Covered source and detection colors must be finite");
    }

    StarMask maskBuilder;
    StarMaskOptions maskOptions = options.mask;
    maskOptions.radius = std::max(maskOptions.radius, options.radius);
    if (options.edgeAware) {
        const auto expandedRadius = static_cast<std::uint64_t>(options.radius) + 5U;
        if (expandedRadius > 32) {
            return reductionError("ArgumentInvalid", "Edge-aware star reduction radius exceeds 32 pixels");
        }
        maskOptions.layered = true;
        maskOptions.largeRadius = std::max<std::uint32_t>(
            maskOptions.largeRadius,
            static_cast<std::uint32_t>(expandedRadius)
        );
        maskOptions.largeStarPeak = std::min(maskOptions.largeStarPeak, 0.65F);
    }
    const auto maskResult = maskBuilder.create(detectionImage, maskOptions);
    if (!maskResult.ok) {
        return reductionError(maskResult.errorCode, maskResult.message);
    }

    ImageBuffer output = image;
    const float imageCenterX = static_cast<float>(image.width - 1) * 0.5F;
    const float imageCenterY = static_cast<float>(image.height - 1) * 0.5F;
    const float maxCenterDistance =
        std::max(1.0F, std::sqrt(imageCenterX * imageCenterX + imageCenterY * imageCenterY));
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            const auto pixelOffset = pixel * image.channels;
            if (!detail::pixelHasValidColor(image, pixel)) {
                detail::clearMaskedPixel(output, pixel);
                continue;
            }
            const auto maskOffset = pixel * maskResult.mask.channels;
            const float maskValue = maskResult.mask.pixels[maskOffset];
            if (maskValue <= 0.0F) {
                continue;
            }

            float edgeWeight = 0.0F;
            if (options.edgeAware) {
                const float centerDistance =
                    std::sqrt((static_cast<float>(x) - imageCenterX) * (static_cast<float>(x) - imageCenterX) +
                              (static_cast<float>(y) - imageCenterY) * (static_cast<float>(y) - imageCenterY));
                edgeWeight = smoothStep(0.42F, 0.96F, centerDistance / maxCenterDistance);
            }
            const float edgeBlendBoost = options.edgeAware ? (0.70F + 1.25F * edgeWeight) : 1.0F;
            const std::uint32_t localRadius =
                options.radius + (options.edgeAware ? static_cast<std::uint32_t>(std::lround(edgeWeight)) : 0);
            const float blend = std::clamp(
                maskValue * options.amount * edgeBlendBoost * detail::pixelCoverage(image, pixel),
                0.0F,
                1.0F
            );
            for (std::uint16_t channel = 0; channel < std::min<std::uint16_t>(3, image.channels); ++channel) {
                const auto offset = pixelOffset + channel;
                const float reduced = localMinimum(image, x, y, channel, localRadius);
                float target = reduced;
                float channelBlend = blend;
                if (options.profileAware) {
                    const float mean = localMean(image, x, y, channel, localRadius + 1);
                    target = std::max(reduced, mean);
                    const auto detectionOffset = pixel * detectionImage.channels + channel;
                    if (detail::pixelHasValidColor(detectionImage, pixel) &&
                        detectionImage.pixels[detectionOffset] >= options.haloProtection) {
                        channelBlend *= 0.45F;
                    }
                }
                output.pixels[offset] = image.pixels[offset] * (1.0F - channelBlend) + target * channelBlend;
            }
        }
    }

    if (options.edgeAware) {
        reduceEdgeProfiles(image, detectionImage, output, options);
    }

    StarReductionResult result;
    result.ok = true;
    result.image = std::move(output);
    result.stars = maskResult.stars;
    return result;
}

} // namespace photonstack
