#pragma once

#include <cstdint>
#include <string>

#include "photonstack/ImageBuffer.hpp"
#include "photonstack/StarMask.hpp"

namespace photonstack {

struct StarReductionOptions {
    StarMaskOptions mask;
    float amount = 0.35F;
    std::uint32_t radius = 1;
    bool profileAware = false;
    bool edgeAware = false;
    float haloProtection = 0.85F;
};

struct StarReductionResult {
    bool ok = false;
    ImageBuffer image;
    std::size_t stars = 0;
    std::string errorCode;
    std::string message;
};

class StarReducer {
  public:
    StarReductionResult reduce(const ImageBuffer& image, const StarReductionOptions& options = {}) const;
    StarReductionResult reduceUsingDetectionImage(
        const ImageBuffer& image,
        const ImageBuffer& detectionImage,
        const StarReductionOptions& options = {}
    ) const;
};

} // namespace photonstack
