#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace photonstack {

enum class PixelFormat {
    Float32RGBA,
    Float32Gray,
};

enum class ColorEncoding {
    Linear,
    SRGB,
    Unknown,
};

struct ImageBuffer {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t channels = 4;
    PixelFormat format = PixelFormat::Float32RGBA;
    ColorEncoding colorEncoding = ColorEncoding::Linear;
    std::uint16_t sourceBitsPerChannel = 32;
    std::vector<float> pixels;

    [[nodiscard]] bool empty() const { return width == 0 || height == 0 || pixels.empty(); }

    [[nodiscard]] std::size_t pixelCount() const {
        const auto convertedWidth = static_cast<std::size_t>(width);
        const auto convertedHeight = static_cast<std::size_t>(height);
        if (convertedHeight != 0 && convertedWidth > std::numeric_limits<std::size_t>::max() / convertedHeight) {
            return std::numeric_limits<std::size_t>::max();
        }
        return convertedWidth * convertedHeight;
    }

    [[nodiscard]] std::size_t sampleCount() const {
        const auto pixels = pixelCount();
        const auto convertedChannels = static_cast<std::size_t>(channels);
        if (convertedChannels != 0 && pixels > std::numeric_limits<std::size_t>::max() / convertedChannels) {
            return std::numeric_limits<std::size_t>::max();
        }
        return pixels * convertedChannels;
    }
};

} // namespace photonstack
