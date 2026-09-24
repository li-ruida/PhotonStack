#pragma once

#include <array>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

// A small, spatially invariant covariance correction, not a general deconvolution.
// Covariance is measured from stars in the input's native pixel grid (pixel^2).
struct PsfHomogenizeOptions {
    double covarianceXX = 1.0;
    double covarianceYY = 1.0;
    double covarianceXY = 0.0;
    double strength = 0.7;
};

struct PsfHomogenizeResult {
    bool ok = false;
    ImageBuffer image;
    std::array<double, 9> kernel{};
    double whiteNoiseGain = 1.0;
    std::string errorCode;
    std::string message;
};

class PsfHomogenizer {
  public:
    PsfHomogenizeResult apply(const ImageBuffer& image, const PsfHomogenizeOptions& options) const;
};

} // namespace photonstack
