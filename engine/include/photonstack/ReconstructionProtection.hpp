#pragma once

#include <cstddef>
#include <string>
#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct ReconstructionProtectionOptions {
    // Gaussian sigma, in native pixels; support is rounded to four sigma.
    double backgroundSigma = 8.0;
    double amount = 1.0;
};

struct ReconstructionProtectionResult {
    bool ok = false;
    ImageBuffer image;
    std::size_t protectedPixels = 0;
    std::string errorCode;
    std::string message;
};

// All inputs must already share coordinates and the same linear photometric
// scale. Reference is a directly combined image; mask is a grayscale [0,1]
// weight based on input validity (e.g. saturation), not generated here.
// Replaces fine-scale differences while retaining the reconstructed background.
// Scientific values are never display-clamped. This cannot recover clipped flux.
class ReconstructionProtection {
  public:
    ReconstructionProtectionResult apply(
        const ImageBuffer& reconstructed, const ImageBuffer& reference,
        const ImageBuffer& mask, const ReconstructionProtectionOptions& options = {}) const;
};

} // namespace photonstack
