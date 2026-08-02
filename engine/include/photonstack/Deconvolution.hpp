#pragma once

#include <cstdint>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct DeconvolutionOptions {
    std::uint32_t iterations = 8;
    std::uint32_t radius = 2;
    float sigma = 1.2F;
    float damping = 0.02F;
    bool clampOutput = true;
};

struct DeconvolutionResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

class Deconvolution {
  public:
    DeconvolutionResult richardsonLucy(const ImageBuffer& image, const DeconvolutionOptions& options = {}) const;
};

} // namespace photonstack
