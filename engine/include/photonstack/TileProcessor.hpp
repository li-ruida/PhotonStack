#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct TileRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct TileProcessOptions {
    std::uint32_t tileWidth = 512;
    std::uint32_t tileHeight = 512;
};

struct TileBudget {
    std::size_t maxBytes = 64U * 1024U * 1024U;
    std::uint32_t preferredTileWidth = 512;
    std::uint32_t preferredTileHeight = 512;
};

struct TileProcessResult {
    bool ok = false;
    std::size_t tiles = 0;
    std::string errorCode;
    std::string message;
};

class TileProcessor {
  public:
    using TileCallback = std::function<void(const TileRect&)>;

    TileProcessOptions optionsForBudget(const ImageBuffer& image, const TileBudget& budget = {}) const;
    TileProcessResult forEachTile(const ImageBuffer& image, const TileProcessOptions& options,
                                  const TileCallback& callback) const;
};

} // namespace photonstack
