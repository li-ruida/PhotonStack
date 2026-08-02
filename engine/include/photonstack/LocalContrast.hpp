#pragma once

#include <cstdint>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct LocalContrastOptions {
    float amount = 0.25F;
    std::uint32_t radius = 8;
    float threshold = 0.0F;
    bool clampOutput = true;
};

struct LocalContrastResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

class LocalContrast {
  public:
    LocalContrastResult apply(const ImageBuffer& image, const LocalContrastOptions& options = {}) const;
};

} // namespace photonstack
