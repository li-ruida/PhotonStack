#include "photonstack/ImageResizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#include "StraightAlphaSampling.hpp"

namespace photonstack {
namespace {

ResizeResult resizeError(std::string code, std::string message) {
    ResizeResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

float sampleBilinear(const ImageBuffer& input, float x, float y, std::uint16_t channel) {
    return detail::sampleBilinearStraightAlpha(input, x, y, channel, true);
}

} // namespace

ResizeResult ImageResizer::resizeToWidth(const ImageBuffer& input, std::uint32_t targetWidth) const {
    if (input.empty() || input.channels != 4 || input.pixels.size() != input.sampleCount() ||
        !std::all_of(input.pixels.begin(), input.pixels.end(), [](float value) {
            return std::isfinite(value);
        })) {
        return resizeError("ImageBufferInvalid", "Input image must be non-empty finite Float32 RGBA");
    }
    if (targetWidth == 0) {
        return resizeError("ArgumentInvalid", "Target width must be greater than zero");
    }

    const double targetHeightValue = std::round(
        static_cast<double>(input.height) * static_cast<double>(targetWidth) / static_cast<double>(input.width)
    );
    if (!std::isfinite(targetHeightValue) || targetHeightValue > std::numeric_limits<std::uint32_t>::max()) {
        return resizeError("ImageDimensionsInvalid", "Resized image dimensions exceed the supported limit");
    }
    const auto targetHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(targetHeightValue));
    const auto pixelCount = static_cast<std::size_t>(targetWidth) * targetHeight;
    if (pixelCount > std::numeric_limits<std::size_t>::max() / input.channels) {
        return resizeError("ImageDimensionsInvalid", "Resized image sample count exceeds the supported limit");
    }

    ImageBuffer output;
    output.width = targetWidth;
    output.height = targetHeight;
    output.channels = input.channels;
    output.format = input.format;
    output.colorEncoding = input.colorEncoding;
    output.sourceBitsPerChannel = input.sourceBitsPerChannel;
    try {
        output.pixels.assign(pixelCount * input.channels, 0.0F);
    } catch (const std::bad_alloc&) {
        return resizeError("MemoryAllocationFailed", "Unable to allocate the resized image");
    } catch (const std::length_error&) {
        return resizeError("MemoryAllocationFailed", "Unable to allocate the resized image");
    }

    const float scaleX = static_cast<float>(input.width) / static_cast<float>(targetWidth);
    const float scaleY = static_cast<float>(input.height) / static_cast<float>(targetHeight);

    for (std::uint32_t y = 0; y < targetHeight; ++y) {
        for (std::uint32_t x = 0; x < targetWidth; ++x) {
            const float sourceX = (static_cast<float>(x) + 0.5F) * scaleX - 0.5F;
            const float sourceY = (static_cast<float>(y) + 0.5F) * scaleY - 0.5F;
            const auto outputOffset = (static_cast<std::size_t>(y) * targetWidth + x) * output.channels;
            for (std::uint16_t c = 0; c < output.channels; ++c) {
                output.pixels[outputOffset + c] = sampleBilinear(input, sourceX, sourceY, c);
            }
        }
    }

    ResizeResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

} // namespace photonstack
