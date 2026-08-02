#pragma once

#include <cstdint>
#include <string>

#include "photonstack/ImageBuffer.hpp"
#include "photonstack/StarDetector.hpp"

namespace photonstack {

struct StarMaskOptions {
    StarDetectionOptions detection;
    std::uint32_t radius = 2;
    std::uint32_t largeRadius = 6;
    float largeStarPeak = 0.75F;
    float haloOpacity = 0.35F;
    float opacity = 1.0F;
    bool layered = false;
};

struct StarMaskResult {
    bool ok = false;
    ImageBuffer mask;
    std::size_t stars = 0;
    std::size_t largeStars = 0;
    std::string errorCode;
    std::string message;
};

class StarMask {
  public:
    StarMaskResult create(const ImageBuffer& image, const StarMaskOptions& options = {}) const;
};

} // namespace photonstack
