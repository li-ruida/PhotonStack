#pragma once

#include <cstdint>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct NoiseReductionOptions {
    float amount = 0.5F;
    float chromaAmount = 0.5F;
    std::uint32_t radius = 1;
    float edgeThreshold = 0.08F;
    bool clampOutput = true;
};

struct NoiseReductionResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

class NoiseReducer {
  public:
    NoiseReductionResult reduce(const ImageBuffer& image, const NoiseReductionOptions& options = {}) const;
};

} // namespace photonstack
