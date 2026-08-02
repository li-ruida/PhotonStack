#include "photonstack/MeteorLayerComposer.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace photonstack {
namespace {

MeteorLayerResult errorResult(std::string code, std::string message) {
    MeteorLayerResult result;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool validRgba(const ImageBuffer& image) {
    return !image.empty() && image.channels == 4 && image.pixels.size() == image.sampleCount();
}

bool pixelSample(const ImageBuffer& image, std::uint32_t x, std::uint32_t y, float& luminance, float& alpha) {
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
    alpha = image.pixels[offset + 3];
    if (!std::isfinite(alpha) || alpha <= 1.0e-6F) {
        return false;
    }
    alpha = std::clamp(alpha, 0.0F, 1.0F);

    const float red = image.pixels[offset];
    const float green = image.pixels[offset + 1];
    const float blue = image.pixels[offset + 2];
    if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue)) {
        return false;
    }
    luminance = red * 0.2126F + green * 0.7152F + blue * 0.0722F;
    return std::isfinite(luminance);
}

float distanceToSegment(float px, float py, const ArtifactTrail& trail) {
    const float vx = trail.x2 - trail.x1;
    const float vy = trail.y2 - trail.y1;
    const float lengthSquared = vx * vx + vy * vy;
    if (lengthSquared <= 0.0001F) {
        const float dx = px - trail.x1;
        const float dy = py - trail.y1;
        return std::sqrt(dx * dx + dy * dy);
    }

    const float t = std::clamp(((px - trail.x1) * vx + (py - trail.y1) * vy) / lengthSquared, 0.0F, 1.0F);
    const float closestX = trail.x1 + t * vx;
    const float closestY = trail.y1 + t * vy;
    const float dx = px - closestX;
    const float dy = py - closestY;
    return std::sqrt(dx * dx + dy * dy);
}

float trailMaskAlpha(float distance, float coreRadius, float featherRadius) {
    if (distance <= coreRadius) {
        return 1.0F;
    }
    if (featherRadius <= 0.0001F) {
        return 0.0F;
    }
    const float t = std::clamp((distance - coreRadius) / featherRadius, 0.0F, 1.0F);
    return 1.0F - t;
}

float localBackground(
    const ImageBuffer& image,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t radius,
    float fallback
) {
    const auto minX = x > radius ? x - radius : 0;
    const auto minY = y > radius ? y - radius : 0;
    const auto maxX = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        image.width - 1,
        static_cast<std::uint64_t>(x) + radius
    ));
    const auto maxY = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        image.height - 1,
        static_cast<std::uint64_t>(y) + radius
    ));

    double weightedSum = 0.0;
    double totalWeight = 0.0;
    for (std::uint32_t sampleY = minY; sampleY <= maxY; ++sampleY) {
        for (std::uint32_t sampleX = minX; sampleX <= maxX; ++sampleX) {
            float luminance = 0.0F;
            float alpha = 0.0F;
            if (!pixelSample(image, sampleX, sampleY, luminance, alpha)) {
                continue;
            }
            weightedSum += static_cast<double>(luminance) * static_cast<double>(alpha);
            totalWeight += static_cast<double>(alpha);
        }
    }
    return totalWeight <= 1.0e-9 ? fallback : static_cast<float>(weightedSum / totalWeight);
}

ImageBuffer makeTransparentLike(const ImageBuffer& source) {
    ImageBuffer layer;
    layer.width = source.width;
    layer.height = source.height;
    layer.channels = 4;
    layer.format = PixelFormat::Float32RGBA;
    layer.colorEncoding = source.colorEncoding;
    layer.sourceBitsPerChannel = source.sourceBitsPerChannel;
    layer.pixels.assign(layer.sampleCount(), 0.0F);
    return layer;
}

void writeLayerPixel(
    ImageBuffer& layer,
    const ImageBuffer& source,
    std::uint32_t x,
    std::uint32_t y,
    float alpha
) {
    const auto offset = (static_cast<std::size_t>(y) * source.width + x) * source.channels;
    float luminance = 0.0F;
    float sourceAlpha = 0.0F;
    if (!std::isfinite(alpha) || !pixelSample(source, x, y, luminance, sourceAlpha)) {
        return;
    }
    const float effectiveAlpha = std::clamp(alpha * sourceAlpha, 0.0F, 1.0F);
    const float existingAlpha = layer.pixels[offset + 3];
    if (effectiveAlpha <= existingAlpha) {
        return;
    }
    layer.pixels[offset] = source.pixels[offset];
    layer.pixels[offset + 1] = source.pixels[offset + 1];
    layer.pixels[offset + 2] = source.pixels[offset + 2];
    layer.pixels[offset + 3] = effectiveAlpha;
}

bool acceptsMeteorLayerCandidate(const ImageBuffer& source,
                                 const ArtifactTrail& trail,
                                 const MeteorLayerOptions& options) {
    const float minDimension = static_cast<float>(std::min(source.width, source.height));
    const bool largeAstroFrame = minDimension >= 512.0F;
    const float minimumLength = largeAstroFrame
                                    ? std::max(options.detection.minLength * 3.0F, minDimension * 0.035F)
                                    : std::max(options.detection.minLength * 1.8F, 24.0F);
    if (trail.length < minimumLength) {
        return false;
    }

    if (largeAstroFrame) {
        const float maximumWidth = std::max(2.8F, options.detection.maxWidth * 0.58F);
        if (trail.width > maximumWidth) {
            return false;
        }
        if (trail.taperScore < 0.62F) {
            return false;
        }
    }

    return true;
}

} // namespace

MeteorLayerResult MeteorLayerComposer::extract(const ImageBuffer& source, const MeteorLayerOptions& options) const {
    return extractUsingDetectionImage(source, source, options);
}

MeteorLayerResult MeteorLayerComposer::extractUsingDetectionImage(
    const ImageBuffer& source,
    const ImageBuffer& detectionImage,
    const MeteorLayerOptions& options
) const {
    if (!validRgba(source) || !validRgba(detectionImage)) {
        return errorResult("ImageBufferInvalid", "Source and detection image must be non-empty Float32 RGBA");
    }
    if (source.width != detectionImage.width || source.height != detectionImage.height) {
        return errorResult("ImageDimensionsMismatch", "Source and detection image dimensions must match");
    }
    if (!std::isfinite(options.minConfidence) || !std::isfinite(options.maskRadius) ||
        !std::isfinite(options.featherRadius) || !std::isfinite(options.opacity) ||
        options.minConfidence < 0.0F || options.minConfidence > 1.0F || options.maskRadius < 0.0F ||
        options.maskRadius > 64.0F || options.featherRadius < 0.0F || options.featherRadius > 64.0F ||
        options.opacity < 0.0F || options.opacity > 1.0F) {
        return errorResult("ArgumentInvalid", "Meteor layer options are outside valid ranges");
    }

    ArtifactTrailOptions detectionOptions = options.detection;
    detectionOptions.preserveMeteors = true;
    detectionOptions.removeAirplanes = false;
    detectionOptions.removeDrones = false;
    detectionOptions.removeMeteors = false;
    detectionOptions.includeMeteors = true;

    const ArtifactTrailRemover detector;
    const auto detected = detector.detect(detectionImage, detectionOptions);
    if (!detected.ok) {
        return errorResult(detected.errorCode, detected.message);
    }

    MeteorLayerResult result;
    result.ok = true;
    result.layer = makeTransparentLike(source);

    const float coreRadius = std::max(0.5F, options.maskRadius);
    const float featherRadius = std::max(0.0F, options.featherRadius);
    const float searchRadius = coreRadius + featherRadius;
    const auto backgroundRadius = static_cast<std::uint32_t>(std::ceil(searchRadius + 4.0F));

    for (const auto& trail : detected.trails) {
        if (trail.kind != ArtifactTrailKind::Meteor || trail.confidence < options.minConfidence ||
            !acceptsMeteorLayerCandidate(detectionImage, trail, options)) {
            continue;
        }

        result.meteors.push_back(trail);
        const auto minX = static_cast<std::uint32_t>(
            std::max(0.0F, std::floor(std::min(trail.x1, trail.x2) - searchRadius - 1.0F)));
        const auto minY = static_cast<std::uint32_t>(
            std::max(0.0F, std::floor(std::min(trail.y1, trail.y2) - searchRadius - 1.0F)));
        const auto maxX = static_cast<std::uint32_t>(std::min(
            static_cast<float>(source.width - 1),
            std::ceil(std::max(trail.x1, trail.x2) + searchRadius + 1.0F)));
        const auto maxY = static_cast<std::uint32_t>(std::min(
            static_cast<float>(source.height - 1),
            std::ceil(std::max(trail.y1, trail.y2) + searchRadius + 1.0F)));

        for (std::uint32_t y = minY; y <= maxY; ++y) {
            for (std::uint32_t x = minX; x <= maxX; ++x) {
                const float distance = distanceToSegment(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, trail);
                const float geometricAlpha = trailMaskAlpha(distance, coreRadius, featherRadius);
                if (geometricAlpha <= 0.0F) {
                    continue;
                }

                float luminance = 0.0F;
                float sourceAlpha = 0.0F;
                if (!pixelSample(detectionImage, x, y, luminance, sourceAlpha)) {
                    continue;
                }
                const float background = localBackground(detectionImage, x, y, backgroundRadius, luminance);
                const float contrastAlpha = std::clamp((luminance - background + 0.04F) / 0.16F, 0.0F, 1.0F);
                const float alpha = std::clamp(geometricAlpha * std::max(contrastAlpha, 0.2F), 0.0F, 1.0F);
                if (alpha > 0.01F) {
                    writeLayerPixel(result.layer, source, x, y, alpha);
                }
            }
        }
    }

    result.image = result.layer;
    result.restoredMeteors = result.meteors.size();
    return result;
}

MeteorLayerResult MeteorLayerComposer::compose(const ImageBuffer& base, const ImageBuffer& layer, const MeteorLayerOptions& options) const {
    if (!validRgba(base) || !validRgba(layer)) {
        return errorResult("ImageBufferInvalid", "Base and meteor layer must be non-empty Float32 RGBA");
    }
    if (base.width != layer.width || base.height != layer.height) {
        return errorResult("ImageDimensionsMismatch", "Base image and meteor layer dimensions must match");
    }
    if (base.colorEncoding != layer.colorEncoding) {
        return errorResult("ImageColorEncodingMismatch",
                           "Base image and meteor layer must use the same color encoding");
    }
    if (!std::isfinite(options.opacity) || options.opacity < 0.0F || options.opacity > 1.0F) {
        return errorResult("ArgumentInvalid", "Meteor layer opacity must be between 0 and 1");
    }

    MeteorLayerResult result;
    result.ok = true;
    result.image = base;
    result.layer = layer;

    const float opacity = std::clamp(options.opacity, 0.0F, 1.0F);
    std::size_t touchedPixels = 0;
    for (std::size_t pixel = 0; pixel < base.pixelCount(); ++pixel) {
        const auto offset = pixel * base.channels;
        const float rawLayerAlpha = layer.pixels[offset + 3];
        if (!std::isfinite(rawLayerAlpha)) {
            continue;
        }
        const float sourceAlpha = std::clamp(rawLayerAlpha * opacity, 0.0F, 1.0F);
        if (sourceAlpha <= 0.0F) {
            continue;
        }
        const float sourceRed = layer.pixels[offset];
        const float sourceGreen = layer.pixels[offset + 1];
        const float sourceBlue = layer.pixels[offset + 2];
        if (!std::isfinite(sourceRed) || !std::isfinite(sourceGreen) || !std::isfinite(sourceBlue)) {
            continue;
        }

        const float rawBaseAlpha = base.pixels[offset + 3];
        const float baseAlpha = std::isfinite(rawBaseAlpha) ? std::clamp(rawBaseAlpha, 0.0F, 1.0F) : 0.0F;
        const float outputAlpha = sourceAlpha + baseAlpha * (1.0F - sourceAlpha);
        ++touchedPixels;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float rawBase = base.pixels[offset + channel];
            const float baseColor = baseAlpha > 1.0e-6F && std::isfinite(rawBase) ? rawBase : 0.0F;
            const float sourceColor = layer.pixels[offset + channel];
            const float blendColor = std::max(baseColor, sourceColor);
            const float premultiplied = sourceAlpha * (1.0F - baseAlpha) * sourceColor +
                                        sourceAlpha * baseAlpha * blendColor +
                                        (1.0F - sourceAlpha) * baseAlpha * baseColor;
            result.image.pixels[offset + channel] =
                outputAlpha > 1.0e-6F ? premultiplied / outputAlpha : 0.0F;
        }
        result.image.pixels[offset + 3] = outputAlpha;
    }
    result.restoredMeteors = touchedPixels > 0 ? 1 : 0;
    return result;
}

MeteorLayerResult MeteorLayerComposer::restore(const ImageBuffer& base, const ImageBuffer& source, const MeteorLayerOptions& options) const {
    return restoreUsingDetectionImage(base, source, source, options);
}

MeteorLayerResult MeteorLayerComposer::restoreUsingDetectionImage(
    const ImageBuffer& base,
    const ImageBuffer& source,
    const ImageBuffer& detectionImage,
    const MeteorLayerOptions& options
) const {
    if (!validRgba(base) || !validRgba(source)) {
        return errorResult("ImageBufferInvalid", "Base and source images must be non-empty Float32 RGBA");
    }
    if (base.width != source.width || base.height != source.height) {
        return errorResult("ImageDimensionsMismatch", "Base image and source image dimensions must match");
    }
    if (base.colorEncoding != source.colorEncoding) {
        return errorResult("ImageColorEncodingMismatch",
                           "Base image and meteor source must use the same color encoding");
    }

    auto extracted = extractUsingDetectionImage(source, detectionImage, options);
    if (!extracted.ok) {
        return extracted;
    }

    auto composed = compose(base, extracted.layer, options);
    if (!composed.ok) {
        return composed;
    }
    composed.layer = std::move(extracted.layer);
    composed.meteors = std::move(extracted.meteors);
    composed.restoredMeteors = composed.meteors.size();
    return composed;
}

} // namespace photonstack
