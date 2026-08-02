#include "photonstack/ScientificDisplayBridge.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

namespace photonstack {
namespace {

ScientificDisplayBridgeResult bridgeError(std::string code, std::string message) {
    ScientificDisplayBridgeResult result;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool compatibleImages(const ImageBuffer& left, const ImageBuffer& right) {
    return !left.empty() && !right.empty() && left.width == right.width && left.height == right.height &&
           left.channels == right.channels && left.pixels.size() == left.sampleCount() &&
           right.pixels.size() == right.sampleCount();
}

bool representableFloat(double value) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    return std::isfinite(value) && value >= -maximum && value <= maximum;
}

bool validScientificPixel(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    if (image.channels == 4) {
        const float alpha = image.pixels[offset + 3];
        if (!std::isfinite(alpha) || alpha <= 1.0e-6F) {
            return false;
        }
    }
    const auto colorChannels = std::min<std::uint16_t>(3, image.channels);
    for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
        if (!std::isfinite(image.pixels[offset + channel])) {
            return false;
        }
    }
    return true;
}

} // namespace

ScientificDisplayBridgeResult ScientificDisplayBridge::applyCorrections(
    const ImageBuffer& scientificSource,
    const ImageBuffer& displaySource,
    const ImageBuffer& correctedDisplay
) const {
    if (!compatibleImages(scientificSource, displaySource) || !compatibleImages(scientificSource, correctedDisplay) ||
        scientificSource.channels == 0) {
        return bridgeError(
            "ImageBufferMismatch",
            "Scientific source, display source, and corrected display must have identical non-empty dimensions"
        );
    }
    if (scientificSource.colorEncoding != displaySource.colorEncoding ||
        displaySource.colorEncoding != correctedDisplay.colorEncoding) {
        return bridgeError(
            "ImageColorEncodingMismatch",
            "Scientific and display bridge images must use the same color encoding"
        );
    }

    const auto colorChannels = std::min<std::uint16_t>(3, scientificSource.channels);
    double minimumDisplay = std::numeric_limits<double>::infinity();
    double maximumDisplay = -std::numeric_limits<double>::infinity();
    double scientificAtMinimum = 0.0;
    double scientificAtMaximum = 0.0;
    std::size_t validSamples = 0;
    bool hasDisplayChanges = false;

    for (std::size_t pixel = 0; pixel < scientificSource.pixelCount(); ++pixel) {
        if (!validScientificPixel(scientificSource, pixel)) {
            continue;
        }
        const auto offset = pixel * scientificSource.channels;
        if (scientificSource.channels == 4) {
            const float scientificAlpha = scientificSource.pixels[offset + 3];
            const float displayAlpha = displaySource.pixels[offset + 3];
            const float correctedAlpha = correctedDisplay.pixels[offset + 3];
            if (!std::isfinite(displayAlpha) || !std::isfinite(correctedAlpha) ||
                std::fabs(displayAlpha - scientificAlpha) > 1.0e-6F ||
                std::fabs(correctedAlpha - scientificAlpha) > 1.0e-6F) {
                return bridgeError(
                    "ImageCoverageMismatch",
                    "Scientific display bridging cannot change valid-pixel alpha coverage"
                );
            }
        }
        for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
            const double displayValue = displaySource.pixels[offset + channel];
            const double correctedValue = correctedDisplay.pixels[offset + channel];
            if (!std::isfinite(displayValue) || !std::isfinite(correctedValue)) {
                return bridgeError("ImageValueInvalid", "Display bridge inputs must contain finite valid color samples");
            }
            if (displayValue < minimumDisplay) {
                minimumDisplay = displayValue;
                scientificAtMinimum = scientificSource.pixels[offset + channel];
            }
            if (displayValue > maximumDisplay) {
                maximumDisplay = displayValue;
                scientificAtMaximum = scientificSource.pixels[offset + channel];
            }
            hasDisplayChanges = hasDisplayChanges || std::fabs(correctedValue - displayValue) > 1.0e-7F;
            ++validSamples;
        }
    }

    if (validSamples == 0) {
        return bridgeError("ImageCoverageEmpty", "Scientific display bridging requires at least one valid color sample");
    }

    ScientificDisplayBridgeResult result;
    try {
        result.image = scientificSource;
    } catch (const std::bad_alloc&) {
        return bridgeError("MemoryAllocationFailed", "Unable to allocate the scientific bridge output");
    } catch (const std::length_error&) {
        return bridgeError("MemoryAllocationFailed", "Unable to allocate the scientific bridge output");
    }
    const double displaySpan = maximumDisplay - minimumDisplay;
    if (!(displaySpan > 1.0e-7) || !std::isfinite(displaySpan)) {
        if (hasDisplayChanges) {
            return bridgeError(
                "ImageValueMappingUnsupported",
                "Display corrections cannot be mapped back because the display source has no invertible value range"
            );
        }
        result.ok = true;
        return result;
    }

    const double scale = (scientificAtMaximum - scientificAtMinimum) / displaySpan;
    const double offset = scientificAtMinimum - scale * minimumDisplay;
    if (!std::isfinite(scale) || !std::isfinite(offset) || scale <= 0.0) {
        return bridgeError(
            "ImageValueMappingUnsupported",
            "Scientific and display values do not define a positive finite mapping"
        );
    }

    const double mappingTolerance = std::max(2.0e-5, std::fabs(scale) * 2.0e-5);
    for (std::size_t pixel = 0; pixel < scientificSource.pixelCount(); ++pixel) {
        if (!validScientificPixel(scientificSource, pixel)) {
            continue;
        }
        const auto sampleOffset = pixel * scientificSource.channels;
        for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
            const double expected = offset + scale * displaySource.pixels[sampleOffset + channel];
            if (!std::isfinite(expected) ||
                std::fabs(expected - scientificSource.pixels[sampleOffset + channel]) > mappingTolerance) {
                return bridgeError(
                    "ImageValueMappingUnsupported",
                    "Scientific and display values are not related by a reversible linear display mapping"
                );
            }
        }
    }

    for (std::size_t pixel = 0; pixel < scientificSource.pixelCount(); ++pixel) {
        if (!validScientificPixel(scientificSource, pixel)) {
            continue;
        }
        const auto sampleOffset = pixel * scientificSource.channels;
        for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
            const double originalDisplay = displaySource.pixels[sampleOffset + channel];
            const double correctedValue = correctedDisplay.pixels[sampleOffset + channel];
            if (std::fabs(correctedValue - originalDisplay) <= 1.0e-7F) {
                continue;
            }
            const double mapped = offset + scale * correctedValue;
            if (!representableFloat(mapped)) {
                return bridgeError(
                    "ImageValueInvalid", "Mapped scientific correction exceeds the Float32 range");
            }
            result.image.pixels[sampleOffset + channel] = static_cast<float>(mapped);
            ++result.correctedSamples;
        }
    }

    result.ok = true;
    result.displayToScientificScale = scale;
    result.displayToScientificOffset = offset;
    return result;
}

} // namespace photonstack
