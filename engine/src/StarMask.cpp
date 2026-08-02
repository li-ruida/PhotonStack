#include "photonstack/StarMask.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

namespace photonstack {
namespace {

StarMaskResult maskError(std::string code, std::string message) {
    StarMaskResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

void setMaskPixel(ImageBuffer& mask, std::uint32_t x, std::uint32_t y, float value) {
    const auto offset = (static_cast<std::size_t>(y) * mask.width + x) * mask.channels;
    mask.pixels[offset] = value;
    mask.pixels[offset + 1] = value;
    mask.pixels[offset + 2] = value;
    mask.pixels[offset + 3] = 1.0F;
}

} // namespace

StarMaskResult StarMask::create(const ImageBuffer& image, const StarMaskOptions& options) const {
    if (image.empty() || image.channels < 3 || image.pixels.size() != image.sampleCount()) {
        return maskError("ImageBufferInvalid", "Input image must be a non-empty RGB/RGBA float buffer");
    }
    if (options.radius == 0 || options.radius > 32 || options.largeRadius == 0 ||
        (options.layered && options.largeRadius > 32) || !std::isfinite(options.largeStarPeak) ||
        !std::isfinite(options.haloOpacity) || !std::isfinite(options.opacity) ||
        options.largeStarPeak < 0.0F || options.largeStarPeak > 1.0F || options.haloOpacity < 0.0F ||
        options.haloOpacity > 1.0F || options.opacity < 0.0F || options.opacity > 1.0F) {
        return maskError("ArgumentInvalid", "Star mask options are outside valid ranges");
    }

    const StarDetector detector;
    const auto detection = detector.detect(image, options.detection);
    if (!detection.ok) {
        return maskError(detection.errorCode, detection.message);
    }

    ImageBuffer mask;
    mask.width = image.width;
    mask.height = image.height;
    mask.channels = 4;
    mask.format = PixelFormat::Float32RGBA;
    mask.colorEncoding = ColorEncoding::Unknown;
    mask.pixels.assign(mask.sampleCount(), 0.0F);
    for (std::size_t pixel = 0; pixel < mask.pixelCount(); ++pixel) {
        mask.pixels[pixel * mask.channels + 3] = 1.0F;
    }

    const float opacity = std::clamp(options.opacity, 0.0F, 1.0F);
    float largeStarFluxThreshold = 0.0F;
    if (options.layered && !detection.stars.empty()) {
        std::vector<float> fluxes;
        fluxes.reserve(detection.stars.size());
        std::transform(detection.stars.begin(), detection.stars.end(), std::back_inserter(fluxes), [](const Star& star) {
            return star.flux;
        });
        const auto largeStarIndex = static_cast<std::size_t>(
            std::ceil(static_cast<double>(fluxes.size() - 1) * 0.85));
        std::nth_element(fluxes.begin(), fluxes.begin() + static_cast<std::ptrdiff_t>(largeStarIndex), fluxes.end());
        largeStarFluxThreshold = fluxes[largeStarIndex];
    }
    std::size_t largeStars = 0;
    for (const auto& star : detection.stars) {
        const auto centerX = static_cast<int>(std::lround(star.x));
        const auto centerY = static_cast<int>(std::lround(star.y));
        const bool isLarge = options.layered && star.peak >= options.largeStarPeak && star.flux >= largeStarFluxThreshold;
        const auto radius = static_cast<int>(isLarge ? std::max(options.largeRadius, options.radius) : options.radius);
        if (isLarge) {
            ++largeStars;
        }

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int px = centerX + dx;
                const int py = centerY + dy;
                if (px < 0 || py < 0 || px >= static_cast<int>(mask.width) || py >= static_cast<int>(mask.height)) {
                    continue;
                }

                const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (distance > static_cast<float>(radius)) {
                    continue;
                }

                const float normalizedDistance = distance / static_cast<float>(radius + 1);
                const float feather = 1.0F - normalizedDistance;
                float layerOpacity = opacity;
                if (isLarge && normalizedDistance > 0.55F) {
                    layerOpacity *= std::clamp(options.haloOpacity, 0.0F, 1.0F);
                }
                const float value = std::clamp(feather * layerOpacity, 0.0F, 1.0F);
                const auto offset = (static_cast<std::size_t>(static_cast<std::uint32_t>(py)) * mask.width +
                                     static_cast<std::uint32_t>(px)) *
                                    mask.channels;
                const float combined = std::max(mask.pixels[offset], value);
                setMaskPixel(mask, static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py), combined);
            }
        }
    }

    StarMaskResult result;
    result.ok = true;
    result.mask = std::move(mask);
    result.stars = detection.stars.size();
    result.largeStars = largeStars;
    return result;
}

} // namespace photonstack
