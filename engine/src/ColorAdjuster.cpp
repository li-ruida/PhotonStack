#include "photonstack/ColorAdjuster.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "DisplayGamut.hpp"
#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

ColorAdjustmentResult colorError(std::string code, std::string message) {
    ColorAdjustmentResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

float channelMedian(const ImageBuffer& image, std::uint16_t channel) {
    std::vector<detail::WeightedValue> values;
    values.reserve(image.pixelCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (!detail::pixelHasValidColor(image, pixel)) {
            continue;
        }
        values.push_back({
            .value = image.pixels[pixel * image.channels + channel],
            .weight = detail::pixelCoverage(image, pixel),
        });
    }
    return static_cast<float>(detail::weightedMedian(std::move(values)));
}

float smoothstep(float edge0, float edge1, float value) {
    if (edge0 == edge1) {
        return value < edge0 ? 0.0F : 1.0F;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

} // namespace

ColorAdjustmentResult ColorAdjuster::neutralizeBackground(const ImageBuffer& image,
                                                          const BackgroundNeutralizationOptions& options) const {
    if (image.empty() || image.channels < 3 || image.pixels.size() != image.sampleCount()) {
        return colorError("ImageBufferInvalid", "Input image must be a non-empty RGB/RGBA float image");
    }
    if (detail::imageHasInvalidCoveredColor(image)) {
        return colorError("ImageBufferInvalid", "Input image contains non-finite visible color samples");
    }
    if (!std::isfinite(options.strength) || !std::isfinite(options.epsilon) || options.strength < 0.0F ||
        options.strength > 1.0F || options.epsilon <= 0.0F) {
        return colorError("ArgumentInvalid", "Background neutralization options are outside valid ranges");
    }
    if (!detail::imageHasCoveredColor(image)) {
        return colorError("ImageCoverageEmpty", "Input image has no visible color samples");
    }

    const float red = channelMedian(image, 0);
    const float green = channelMedian(image, 1);
    const float blue = channelMedian(image, 2);
    const float target = (red + green + blue) / 3.0F;

    const float redScale = target / std::max(options.epsilon, red);
    const float greenScale = target / std::max(options.epsilon, green);
    const float blueScale = target / std::max(options.epsilon, blue);

    ImageBuffer output = image;
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (!detail::pixelHasValidColor(image, pixel)) {
            detail::clearMaskedPixel(output, pixel);
            continue;
        }
        const auto offset = pixel * image.channels;
        output.pixels[offset] =
            std::clamp(image.pixels[offset] * (1.0F + (redScale - 1.0F) * options.strength), 0.0F, 1.0F);
        output.pixels[offset + 1] =
            std::clamp(image.pixels[offset + 1] * (1.0F + (greenScale - 1.0F) * options.strength), 0.0F, 1.0F);
        output.pixels[offset + 2] =
            std::clamp(image.pixels[offset + 2] * (1.0F + (blueScale - 1.0F) * options.strength), 0.0F, 1.0F);
    }

    ColorAdjustmentResult result;
    result.ok = true;
    result.image = std::move(output);
    result.redBackground = red;
    result.greenBackground = green;
    result.blueBackground = blue;
    return result;
}

ColorAdjustmentResult ColorAdjuster::adjustSaturation(const ImageBuffer& image,
                                                      const SaturationOptions& options) const {
    if (image.empty() || image.channels < 3 || image.pixels.size() != image.sampleCount()) {
        return colorError("ImageBufferInvalid", "Input image must be a non-empty RGB/RGBA float image");
    }
    if (detail::imageHasInvalidCoveredColor(image)) {
        return colorError("ImageBufferInvalid", "Input image contains non-finite visible color samples");
    }
    if (!std::isfinite(options.amount) || options.amount < -1.0F || options.amount > 2.0F) {
        return colorError("ArgumentInvalid", "Saturation amount must be between -1 and 2");
    }

    ImageBuffer output = image;
    const float factor = 1.0F + options.amount;
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (!detail::pixelHasValidColor(image, pixel)) {
            detail::clearMaskedPixel(output, pixel);
            continue;
        }
        const auto offset = pixel * image.channels;
        const float luminance =
            image.pixels[offset] * 0.2126F + image.pixels[offset + 1] * 0.7152F + image.pixels[offset + 2] * 0.0722F;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            output.pixels[offset + channel] =
                std::clamp(luminance + (image.pixels[offset + channel] - luminance) * factor, 0.0F, 1.0F);
        }
    }

    ColorAdjustmentResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

ColorAdjustmentResult ColorAdjuster::suppressGreenCast(const ImageBuffer& image,
                                                       const GreenCastSuppressionOptions& options) const {
    if (image.empty() || image.channels < 3 || image.pixels.size() != image.sampleCount()) {
        return colorError("ImageBufferInvalid", "Input image must be a non-empty RGB/RGBA float image");
    }
    if (detail::imageHasInvalidCoveredColor(image)) {
        return colorError("ImageBufferInvalid", "Input image contains non-finite visible color samples");
    }
    if (!std::isfinite(options.amount) || !std::isfinite(options.backgroundLimit) ||
        !std::isfinite(options.greenExcessThreshold) || options.amount < 0.0F || options.amount > 1.0F ||
        options.backgroundLimit <= 0.0F || options.backgroundLimit > 1.0F || options.greenExcessThreshold < 0.0F) {
        return colorError("ArgumentInvalid", "Green cast suppression options are outside valid ranges");
    }
    if (!detail::imageHasCoveredColor(image)) {
        return colorError("ImageCoverageEmpty", "Input image has no visible color samples");
    }

    ImageBuffer output = image;
    std::size_t affectedPixels = 0;
    if (options.averageNeutral || options.preserveLightness) {
        if (image.colorEncoding != ColorEncoding::SRGB)
            return colorError("ImageColorEncodingMismatch",
                              "Lightness-preserving green styling requires sRGB display input");
        for (std::size_t p = 0; p < image.pixelCount(); ++p)
            if (detail::pixelHasValidColor(image, p))
                for (int c = 0; c < 3; ++c)
                    if (image.pixels[p * image.channels + c] < 0 || image.pixels[p * image.channels + c] > 1)
                        return colorError("ImageValueInvalid", "Green display styling requires samples in [0,1]");
    }
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (!detail::pixelHasValidColor(image, pixel)) {
            detail::clearMaskedPixel(output, pixel);
            continue;
        }
        const auto offset = pixel * image.channels;
        const float red = image.pixels[offset];
        const float green = image.pixels[offset + 1];
        const float blue = image.pixels[offset + 2];
        const float luminance = red * 0.2126F + green * 0.7152F + blue * 0.0722F;
        const float neutralGreen = (red + blue) * 0.5F;
        const float excess = green - neutralGreen;
        if (excess <= options.greenExcessThreshold) {
            continue;
        }

        const float backgroundMask =
            1.0F - smoothstep(options.backgroundLimit * 0.65F, options.backgroundLimit, luminance);
        const float excessMask = std::clamp(excess / std::max(0.03F, luminance * 0.35F), 0.0F, 1.0F);
        const float blend = options.averageNeutral
                                ? options.amount
                                : std::clamp(options.amount * backgroundMask * excessMask, 0.0F, 1.0F);
        if (blend <= 0.0F) {
            continue;
        }

        output.pixels[offset + 1] = std::clamp(green + (neutralGreen - green) * blend, 0.0F, 1.0F);
        if (options.preserveLightness) {
            const auto decode = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
            const auto encode = [](double v) {
                return v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055;
            };
            const std::array<double, 3> weights{.2126, .7152, .0722};
            std::array<double, 3> color;
            double originalY = 0, adjustedY = 0;
            for (int c = 0; c < 3; ++c) {
                originalY += weights[c] * decode(image.pixels[offset + c]);
                color[c] = decode(output.pixels[offset + c]);
                adjustedY += weights[c] * color[c];
            }
            // A pure green pixel may become black at full suppression. Its
            // achromatic value is the only finite neutral solution at that Y.
            for (double& value : color)
                value = adjustedY > 0 ? value * originalY / adjustedY : originalY;
            double available = std::numeric_limits<double>::infinity();
            for (double value : color) {
                if (value > originalY)
                    available = std::min(available, (1 - originalY) / (value - originalY));
                if (value < originalY)
                    available = std::min(available, originalY / (originalY - value));
            }
            const double scale = detail::softGamutScale(available);
            for (int c = 0; c < 3; ++c)
                output.pixels[offset + c] =
                    static_cast<float>(encode(std::clamp(originalY + scale * (color[c] - originalY), 0.0, 1.0)));
        }
        ++affectedPixels;
    }

    ColorAdjustmentResult result;
    result.ok = true;
    result.image = std::move(output);
    result.redBackground = channelMedian(result.image, 0);
    result.greenBackground = channelMedian(result.image, 1);
    result.blueBackground = channelMedian(result.image, 2);
    result.affectedPixels = affectedPixels;
    return result;
}

} // namespace photonstack
