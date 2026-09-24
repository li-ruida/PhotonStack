#pragma once

#include <array>
#include <string>
#include "photonstack/ImageBuffer.hpp"

namespace photonstack {
enum class NonlocalDenoiseMode { Luminance, ConservativeRGB };
struct NonlocalDenoiseOptions {
    // Distance scale in scientific input units. Zero is an exact bypass.
    double h = 1;
    std::array<double, 3> luminanceWeights{.2126, .7152, .0722};
    NonlocalDenoiseMode mode = NonlocalDenoiseMode::Luminance;
};
struct NonlocalDenoiseResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};
// Scientific RGB, complete coverage only. Fixed 7x7 patch / 11x11 search.
// Caller supplies a co-registered grayscale blend map [0,1]: zero protects
// the original pixel exactly. This class does not discover sources or choose h.
// Common nonnegative RGB weights; no display clipping or spatial resampling.
// ConservativeRGB uses symmetric transfers and weighted RGB patch distance.
// It preserves channel sums up to float rounding, not arbitrary aperture flux.
class NonlocalDenoiser {
  public:
    NonlocalDenoiseResult apply(const ImageBuffer& image, const ImageBuffer& blend,
        const NonlocalDenoiseOptions& options = {}) const;
};
} // namespace photonstack
