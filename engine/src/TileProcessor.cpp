#include "photonstack/TileProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace photonstack {
namespace {

TileProcessResult tileError(std::string code, std::string message) {
    TileProcessResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

} // namespace

TileProcessOptions TileProcessor::optionsForBudget(const ImageBuffer& image, const TileBudget& budget) const {
    const auto channels = std::max<std::uint16_t>(image.channels, 1);
    const auto bytesPerPixel = static_cast<std::size_t>(channels) * sizeof(float);
    const auto maxPixels = std::max<std::size_t>(1, budget.maxBytes / std::max<std::size_t>(bytesPerPixel, 1));
    const auto preferredWidth = std::max<std::uint32_t>(
        1, std::min(std::max<std::uint32_t>(budget.preferredTileWidth, 1), image.width == 0 ? 1 : image.width));
    const auto preferredHeight = std::max<std::uint32_t>(
        1, std::min(std::max<std::uint32_t>(budget.preferredTileHeight, 1), image.height == 0 ? 1 : image.height));
    const auto preferredPixels =
        static_cast<std::size_t>(preferredWidth) * static_cast<std::size_t>(preferredHeight);

    TileProcessOptions options;
    if (preferredPixels <= maxPixels) {
        options.tileWidth = preferredWidth;
        options.tileHeight = preferredHeight;
        return options;
    }

    const double aspect = static_cast<double>(preferredWidth) / preferredHeight;
    const auto idealWidth = static_cast<std::size_t>(std::max(
        1.0, std::floor(std::sqrt(static_cast<double>(maxPixels) * aspect))));
    const auto width = std::max<std::size_t>(
        1, std::min({static_cast<std::size_t>(preferredWidth), idealWidth, maxPixels}));
    const auto height = std::max<std::size_t>(
        1, std::min(static_cast<std::size_t>(preferredHeight), maxPixels / width));
    options.tileWidth = static_cast<std::uint32_t>(width);
    options.tileHeight = static_cast<std::uint32_t>(height);
    return options;
}

TileProcessResult TileProcessor::forEachTile(const ImageBuffer& image, const TileProcessOptions& options,
                                             const TileCallback& callback) const {
    if (image.empty() || image.pixels.size() != image.sampleCount()) {
        return tileError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (options.tileWidth == 0 || options.tileHeight == 0) {
        return tileError("ArgumentInvalid", "Tile dimensions must be greater than zero");
    }
    if (!callback) {
        return tileError("ArgumentInvalid", "Tile callback must be valid");
    }

    TileProcessResult result;
    result.ok = true;

    for (std::uint32_t y = 0; y < image.height;) {
        for (std::uint32_t x = 0; x < image.width;) {
            const TileRect tile = {
                .x = x,
                .y = y,
                .width = std::min(options.tileWidth, image.width - x),
                .height = std::min(options.tileHeight, image.height - y),
            };
            callback(tile);
            result.tiles += 1;
            if (options.tileWidth >= image.width - x) {
                break;
            }
            x += options.tileWidth;
        }
        if (options.tileHeight >= image.height - y) {
            break;
        }
        y += options.tileHeight;
    }

    return result;
}

} // namespace photonstack
