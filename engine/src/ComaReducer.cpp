#include "photonstack/ComaReducer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

ComaReductionResult comaError(std::string code, std::string message) {
    ComaReductionResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

float luminanceAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
    if (image.channels < 3) {
        return image.pixels[offset];
    }
    return image.pixels[offset] * 0.2126F + image.pixels[offset + 1] * 0.7152F + image.pixels[offset + 2] * 0.0722F;
}

bool sampleLuminanceBilinear(const ImageBuffer& image, float x, float y, float& result) {
    const float clampedX = std::clamp(x, 0.0F, static_cast<float>(image.width - 1));
    const float clampedY = std::clamp(y, 0.0F, static_cast<float>(image.height - 1));
    const auto x0 = static_cast<std::uint32_t>(std::floor(clampedX));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clampedY));
    const auto x1 = std::min<std::uint32_t>(x0 + 1, image.width - 1);
    const auto y1 = std::min<std::uint32_t>(y0 + 1, image.height - 1);
    const float tx = clampedX - static_cast<float>(x0);
    const float ty = clampedY - static_cast<float>(y0);
    const std::uint32_t xs[4] = {x0, x1, x0, x1};
    const std::uint32_t ys[4] = {y0, y0, y1, y1};
    const float weights[4] = {
        (1.0F - tx) * (1.0F - ty),
        tx * (1.0F - ty),
        (1.0F - tx) * ty,
        tx * ty,
    };
    double weightedSum = 0.0;
    double weightSum = 0.0;
    for (std::size_t sample = 0; sample < 4; ++sample) {
        if (weights[sample] <= 0.0F) {
            continue;
        }
        const auto pixel = static_cast<std::size_t>(ys[sample]) * image.width + xs[sample];
        if (!detail::pixelHasValidColor(image, pixel)) {
            continue;
        }
        const double weight = weights[sample] * detail::pixelCoverage(image, pixel);
        weightedSum += static_cast<double>(luminanceAt(image, xs[sample], ys[sample])) * weight;
        weightSum += weight;
    }
    if (weightSum <= 0.0F) {
        return false;
    }
    result = static_cast<float>(weightedSum / weightSum);
    return std::isfinite(result);
}

float smoothStep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / std::max(1.0e-6F, edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

struct StarShape {
    bool ok = false;
    float x = 0.0F;
    float y = 0.0F;
    float background = 0.0F;
    float peakSignal = 0.0F;
    float sigmaMajor = 1.0F;
    float sigmaMinor = 1.0F;
    float majorX = 1.0F;
    float majorY = 0.0F;
    float eccentricity = 0.0F;
};

StarShape analyzeStarShape(const ImageBuffer& image, const Star& detectedStar, std::uint32_t radius) {
    const int centerX = static_cast<int>(std::lround(detectedStar.x));
    const int centerY = static_cast<int>(std::lround(detectedStar.y));
    if (centerX < static_cast<int>(radius) || centerY < static_cast<int>(radius) ||
        centerX + static_cast<int>(radius) >= static_cast<int>(image.width) ||
        centerY + static_cast<int>(radius) >= static_cast<int>(image.height)) {
        return {};
    }

    std::vector<detail::WeightedValue> backgroundSamples;
    backgroundSamples.reserve((radius * 2 + 1) * 4);
    const float radiusF = static_cast<float>(radius);
    const float innerBackgroundRadius = radiusF * 0.62F;
    float maxLuminance = -std::numeric_limits<float>::infinity();
    for (int dy = -static_cast<int>(radius); dy <= static_cast<int>(radius); ++dy) {
        for (int dx = -static_cast<int>(radius); dx <= static_cast<int>(radius); ++dx) {
            const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (distance > radiusF) {
                continue;
            }
            const auto x = static_cast<std::uint32_t>(centerX + dx);
            const auto y = static_cast<std::uint32_t>(centerY + dy);
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            if (!detail::pixelHasValidColor(image, pixel)) {
                continue;
            }
            const float luminance = luminanceAt(image, x, y);
            const float coverage = detail::pixelCoverage(image, pixel);
            maxLuminance = std::max(maxLuminance, luminance * coverage);
            if (distance >= innerBackgroundRadius) {
                backgroundSamples.push_back({.value = luminance, .weight = coverage});
            }
        }
    }
    if (backgroundSamples.empty() || !std::isfinite(maxLuminance)) {
        return {};
    }
    const float background = static_cast<float>(detail::weightedPercentile(std::move(backgroundSamples), 0.30));
    const float peakSignal = std::max(0.0F, maxLuminance - background);
    if (peakSignal <= 1.0e-4F) {
        return {};
    }

    double weightSum = 0.0;
    double centroidX = 0.0;
    double centroidY = 0.0;
    for (int dy = -static_cast<int>(radius); dy <= static_cast<int>(radius); ++dy) {
        for (int dx = -static_cast<int>(radius); dx <= static_cast<int>(radius); ++dx) {
            const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (distance > radiusF) {
                continue;
            }
            const auto x = static_cast<std::uint32_t>(centerX + dx);
            const auto y = static_cast<std::uint32_t>(centerY + dy);
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            if (!detail::pixelHasValidColor(image, pixel)) {
                continue;
            }
            const float signal = std::max(0.0F, luminanceAt(image, x, y) - background);
            if (signal <= peakSignal * 0.035F) {
                continue;
            }
            const double weight = static_cast<double>(signal) * detail::pixelCoverage(image, pixel);
            weightSum += weight;
            centroidX += static_cast<double>(x) * weight;
            centroidY += static_cast<double>(y) * weight;
        }
    }
    if (weightSum <= 1.0e-8) {
        return {};
    }
    centroidX /= weightSum;
    centroidY /= weightSum;

    double momentXX = 0.0;
    double momentYY = 0.0;
    double momentXY = 0.0;
    for (int dy = -static_cast<int>(radius); dy <= static_cast<int>(radius); ++dy) {
        for (int dx = -static_cast<int>(radius); dx <= static_cast<int>(radius); ++dx) {
            const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (distance > radiusF) {
                continue;
            }
            const auto x = static_cast<std::uint32_t>(centerX + dx);
            const auto y = static_cast<std::uint32_t>(centerY + dy);
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            if (!detail::pixelHasValidColor(image, pixel)) {
                continue;
            }
            const float signal = std::max(0.0F, luminanceAt(image, x, y) - background);
            if (signal <= peakSignal * 0.035F) {
                continue;
            }
            const double offsetX = static_cast<double>(x) - centroidX;
            const double offsetY = static_cast<double>(y) - centroidY;
            const double weight = static_cast<double>(signal) * detail::pixelCoverage(image, pixel);
            momentXX += offsetX * offsetX * weight;
            momentYY += offsetY * offsetY * weight;
            momentXY += offsetX * offsetY * weight;
        }
    }
    momentXX /= weightSum;
    momentYY /= weightSum;
    momentXY /= weightSum;

    const double trace = momentXX + momentYY;
    const double determinant = std::max(0.0, momentXX * momentYY - momentXY * momentXY);
    const double discriminant = std::max(0.0, trace * trace * 0.25 - determinant);
    const double majorVariance = std::max(0.0, trace * 0.5 + std::sqrt(discriminant));
    const double minorVariance = std::max(0.0, trace * 0.5 - std::sqrt(discriminant));
    if (majorVariance <= 1.0e-6 || minorVariance <= 1.0e-6) {
        return {};
    }

    double majorX = momentXY;
    double majorY = majorVariance - momentXX;
    if (std::fabs(majorX) + std::fabs(majorY) < 1.0e-8) {
        majorX = majorVariance - momentYY;
        majorY = momentXY;
    }
    const double majorLength = std::sqrt(majorX * majorX + majorY * majorY);
    if (majorLength <= 1.0e-8) {
        majorX = 1.0;
        majorY = 0.0;
    } else {
        majorX /= majorLength;
        majorY /= majorLength;
    }

    const float sigmaMajor = static_cast<float>(std::sqrt(majorVariance));
    const float sigmaMinor = static_cast<float>(std::sqrt(minorVariance));
    const float eccentricity =
        std::sqrt(std::max(0.0F, 1.0F - (sigmaMinor * sigmaMinor) / (sigmaMajor * sigmaMajor)));
    return {
        .ok = true,
        .x = static_cast<float>(centroidX),
        .y = static_cast<float>(centroidY),
        .background = background,
        .peakSignal = peakSignal,
        .sigmaMajor = sigmaMajor,
        .sigmaMinor = sigmaMinor,
        .majorX = static_cast<float>(majorX),
        .majorY = static_cast<float>(majorY),
        .eccentricity = eccentricity,
    };
}

} // namespace

ComaReductionResult ComaReducer::reduce(const ImageBuffer& image, const ComaReductionOptions& options) const {
    return reduceUsingDetectionImage(image, image, options);
}

ComaReductionResult ComaReducer::reduceUsingDetectionImage(
    const ImageBuffer& image,
    const ImageBuffer& detectionImage,
    const ComaReductionOptions& options
) const {
    if (image.empty() || image.channels < 3 || image.pixels.size() != image.sampleCount() ||
        detectionImage.empty() || detectionImage.channels < 3 ||
        detectionImage.pixels.size() != detectionImage.sampleCount()) {
        return comaError(
            "ImageBufferInvalid",
            "Input and detection image must be non-empty RGB/RGBA float buffers"
        );
    }
    if (image.width != detectionImage.width || image.height != detectionImage.height) {
        return comaError("ImageDimensionsMismatch", "Input and detection image dimensions must match");
    }
    if (!std::isfinite(options.amount) || options.amount < 0.0F || options.amount > 1.0F) {
        return comaError("ArgumentInvalid", "Coma reduction amount must be between 0 and 1");
    }
    if (options.radius < 2 || options.radius > 32) {
        return comaError("ArgumentInvalid", "Coma reduction radius must be between 2 and 32 pixels");
    }
    if (!std::isfinite(options.eccentricityThreshold) || options.eccentricityThreshold < 0.0F ||
        options.eccentricityThreshold >= 1.0F) {
        return comaError("ArgumentInvalid", "eccentricityThreshold must be in [0, 1)");
    }
    if (!std::isfinite(options.highlightProtection) || options.highlightProtection < 0.0F ||
        options.highlightProtection > 1.0F) {
        return comaError("ArgumentInvalid", "highlightProtection must be between 0 and 1");
    }
    if (detail::imageHasInvalidCoveredColor(image) || detail::imageHasInvalidCoveredColor(detectionImage)) {
        return comaError("ImageBufferInvalid", "Covered source and detection colors must be finite");
    }

    StarDetectionOptions detectionOptions = options.detection;
    detectionOptions.border = std::max<std::uint32_t>(detectionOptions.border, options.radius + 1);
    detectionOptions.sigmaThreshold = std::min(detectionOptions.sigmaThreshold, 2.0F);
    detectionOptions.minPeak = std::min(detectionOptions.minPeak, 0.025F);

    const StarDetector detector;
    const auto detected = detector.detect(detectionImage, detectionOptions);
    if (!detected.ok) {
        return comaError(detected.errorCode, detected.message);
    }

    ImageBuffer output = image;
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (!detail::pixelHasValidColor(image, pixel)) {
            detail::clearMaskedPixel(output, pixel);
        }
    }
    std::size_t correctedStars = 0;
    std::size_t analyzedStars = 0;
    double eccentricitySum = 0.0;
    const float imageCenterX = static_cast<float>(image.width - 1) * 0.5F;
    const float imageCenterY = static_cast<float>(image.height - 1) * 0.5F;
    const float maxCenterDistance = std::max(1.0F, std::sqrt(imageCenterX * imageCenterX + imageCenterY * imageCenterY));

    for (const auto& star : detected.stars) {
        const auto shape = analyzeStarShape(image, star, options.radius);
        if (!shape.ok) {
            continue;
        }
        ++analyzedStars;
        eccentricitySum += shape.eccentricity;
        if (shape.eccentricity < options.eccentricityThreshold) {
            continue;
        }

        const float eccentricityWeight =
            std::clamp((shape.eccentricity - options.eccentricityThreshold) /
                           std::max(1.0e-4F, 1.0F - options.eccentricityThreshold),
                       0.0F, 1.0F);
        const float centerDistance = std::sqrt((shape.x - imageCenterX) * (shape.x - imageCenterX) +
                                               (shape.y - imageCenterY) * (shape.y - imageCenterY));
        const float normalizedEdge = std::clamp(centerDistance / maxCenterDistance, 0.0F, 1.0F);
        const float edgeWeight = options.edgeAware ? (0.45F + 0.75F * smoothStep(0.28F, 0.96F, normalizedEdge)) : 1.0F;
        const float axisRatio = std::clamp(shape.sigmaMinor / std::max(1.0e-4F, shape.sigmaMajor), 0.0F, 1.0F);
        const float elongationWeight = smoothStep(0.18F, 0.75F, 1.0F - axisRatio);
        const float baseSigmaTarget = std::sqrt(std::max(0.10F, shape.sigmaMajor * shape.sigmaMinor));
        const float roundSigmaTarget = std::max(0.55F, shape.sigmaMinor * (1.06F + 0.10F * (1.0F - normalizedEdge)));
        const float roundCorrection =
            std::clamp(options.amount * edgeWeight * (0.35F + 0.65F * eccentricityWeight) * elongationWeight, 0.0F, 1.0F);
        const float sigmaTarget =
            std::max(roundSigmaTarget, baseSigmaTarget * (1.0F - 0.62F * roundCorrection));
        const float squeezeStrength =
            std::clamp(options.amount * edgeWeight * (0.40F + 0.60F * eccentricityWeight) * elongationWeight, 0.0F, 1.0F);
        const float majorSqueeze = std::clamp(1.0F - 0.68F * squeezeStrength, 0.38F, 1.0F);
        const int centerX = static_cast<int>(std::lround(shape.x));
        const int centerY = static_cast<int>(std::lround(shape.y));
        bool changed = false;

        for (int dy = -static_cast<int>(options.radius); dy <= static_cast<int>(options.radius); ++dy) {
            for (int dx = -static_cast<int>(options.radius); dx <= static_cast<int>(options.radius); ++dx) {
                const int px = centerX + dx;
                const int py = centerY + dy;
                if (px < 0 || py < 0 || px >= static_cast<int>(image.width) || py >= static_cast<int>(image.height)) {
                    continue;
                }

                const float offsetX = static_cast<float>(px) - shape.x;
                const float offsetY = static_cast<float>(py) - shape.y;
                const float distance = std::sqrt(offsetX * offsetX + offsetY * offsetY);
                if (distance > static_cast<float>(options.radius)) {
                    continue;
                }
                const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(py)) * image.width +
                                   static_cast<std::uint32_t>(px);
                if (!detail::pixelHasValidColor(image, pixel)) {
                    continue;
                }

                const float u = offsetX * shape.majorX + offsetY * shape.majorY;
                const float v = -offsetX * shape.majorY + offsetY * shape.majorX;
                const float absMajor = std::fabs(u);
                const float absMinor = std::fabs(v);
                const float majorTailWeight = smoothStep(sigmaTarget * 0.42F, sigmaTarget * 1.95F, absMajor);
                const float minorProtection = 1.0F - 0.45F * smoothStep(sigmaTarget * 0.50F, sigmaTarget * 1.45F, absMinor);
                const float circularOuterWeight = 0.55F * smoothStep(sigmaTarget * 0.62F, sigmaTarget * 2.35F, distance);
                const float tailWeight = std::clamp(std::max(majorTailWeight * minorProtection, circularOuterWeight), 0.0F, 1.0F);
                const float feather = std::clamp(1.0F - distance / static_cast<float>(options.radius + 1), 0.0F, 1.0F);
                const float currentLuminance =
                    luminanceAt(output, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
                const float currentSignal = currentLuminance - shape.background;
                if (currentSignal <= 0.0F || currentLuminance <= 0.0F) {
                    continue;
                }

                const float targetSignal =
                    shape.peakSignal * std::exp(-(offsetX * offsetX + offsetY * offsetY) /
                                                 (2.0F * sigmaTarget * sigmaTarget));
                const float gaussianTargetLuminance = shape.background + targetSignal;
                const float squeezedU = u / majorSqueeze;
                const float squeezedX = shape.x + squeezedU * shape.majorX - v * shape.majorY;
                const float squeezedY = shape.y + squeezedU * shape.majorY + v * shape.majorX;
                float squeezedLuminance = 0.0F;
                if (!sampleLuminanceBilinear(image, squeezedX, squeezedY, squeezedLuminance)) {
                    continue;
                }
                const float squeezeTargetLuminance = std::max(shape.background, squeezedLuminance);
                const float targetLuminance = std::min(gaussianTargetLuminance, squeezeTargetLuminance);
                if (targetLuminance >= currentLuminance) {
                    continue;
                }

                const float excessWeight =
                    std::clamp((currentLuminance - targetLuminance) / std::max(1.0e-5F, currentSignal), 0.0F, 1.0F);
                const float squeezeWeight = smoothStep(sigmaTarget * 0.18F, sigmaTarget * 1.25F, absMajor);
                const float shapeWeight = std::max(tailWeight, squeezeWeight * 0.95F);
                float blend = 1.18F * options.amount * edgeWeight * (0.35F + 0.65F * eccentricityWeight) *
                              (0.45F + 0.55F * elongationWeight) * shapeWeight * excessWeight * feather *
                              detail::pixelCoverage(image, pixel);
                const float protectionLuminance =
                    detail::pixelHasValidColor(detectionImage, pixel)
                        ? luminanceAt(
                              detectionImage,
                              static_cast<std::uint32_t>(px),
                              static_cast<std::uint32_t>(py)
                          )
                        : 0.0F;
                if (protectionLuminance >= options.highlightProtection && distance < sigmaTarget * 1.15F) {
                    blend *= 0.45F;
                }
                blend = std::clamp(blend, 0.0F, 0.97F);
                if (blend <= 0.0F) {
                    continue;
                }

                const float newLuminance = currentLuminance - (currentLuminance - targetLuminance) * blend;
                const float scale = std::clamp(newLuminance / currentLuminance, 0.0F, 1.0F);
                const auto pixelOffset =
                    (static_cast<std::size_t>(static_cast<std::uint32_t>(py)) * image.width +
                     static_cast<std::uint32_t>(px)) *
                    image.channels;
                for (std::uint16_t channel = 0; channel < std::min<std::uint16_t>(3, image.channels); ++channel) {
                    const float proposed = output.pixels[pixelOffset + channel] * scale;
                    if (proposed != output.pixels[pixelOffset + channel]) {
                        output.pixels[pixelOffset + channel] = proposed;
                        changed = true;
                    }
                }
            }
        }
        correctedStars += changed ? 1 : 0;
    }

    ComaReductionResult result;
    result.ok = true;
    result.image = std::move(output);
    result.stars = detected.stars.size();
    result.correctedStars = correctedStars;
    result.averageEccentricity =
        analyzedStars == 0 ? 0.0F : static_cast<float>(eccentricitySum / static_cast<double>(analyzedStars));
    return result;
}

} // namespace photonstack
