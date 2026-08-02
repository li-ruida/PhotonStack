#pragma once

#include <cstdint>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct SharpenOptions {
    float amount = 0.35F;
    std::uint32_t radius = 1;
    float threshold = 0.01F;
    bool clampOutput = true;
};

struct SharpenResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

class Sharpen {
  public:
    SharpenResult unsharpMask(const ImageBuffer& image, const SharpenOptions& options = {}) const;
};

} // namespace photonstack
