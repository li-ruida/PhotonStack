#pragma once

#include <cstdint>
#include <string>

#include "photonstack/ImageBuffer.hpp"
#include "photonstack/StarDetector.hpp"

namespace photonstack {

struct ComaReductionOptions {
    StarDetectionOptions detection;
    float amount = 0.80F;
    std::uint32_t radius = 10;
    float eccentricityThreshold = 0.22F;
    bool edgeAware = true;
    float highlightProtection = 0.92F;
};

struct ComaReductionResult {
    bool ok = false;
    ImageBuffer image;
    std::size_t stars = 0;
    std::size_t correctedStars = 0;
    float averageEccentricity = 0.0F;
    std::string errorCode;
    std::string message;
};

class ComaReducer {
  public:
    ComaReductionResult reduce(const ImageBuffer& image, const ComaReductionOptions& options = {}) const;
    ComaReductionResult reduceUsingDetectionImage(
        const ImageBuffer& image,
        const ImageBuffer& detectionImage,
        const ComaReductionOptions& options = {}
    ) const;
};

} // namespace photonstack
