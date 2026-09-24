#pragma once

#include <string>
#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct DisplayGridReductionOptions {
    // Blend the fixed narrow-band correction before its shared RGB gamut guard.
    // Zero returns the original buffer exactly, including masked samples.
    double amount = 1;
};

struct DisplayGridReductionResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

// Display sRGB only, with bounded gray/RGB/RGBA values. This suppresses both
// real detail and artifacts near the sampling limit; it is not deconvolution.
// The unbounded, full-strength transfer is the separable product
// (1 - sin(pi*fx)^128) (1 - sin(pi*fy)^128), with half-sample reflected edges.
// One continuous RGB correction factor protects gamut; aperture flux is not
// guaranteed. Partial coverage and pixels within 64 px of it stay unchanged.
class DisplayGridReducer {
  public:
    DisplayGridReductionResult apply(const ImageBuffer& image,
        const DisplayGridReductionOptions& options = {}) const;
};

} // namespace photonstack
