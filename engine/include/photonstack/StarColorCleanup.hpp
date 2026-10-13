#pragma once

#include "photonstack/ImageBuffer.hpp"
#include <array>
#include <cstddef>
#include <string>

namespace photonstack {
struct StarColorCleanupResult {
    bool ok = false;
    ImageBuffer image;
    std::size_t correctedStars = 0;
    std::string errorCode, message;
};

class StarColorCleanup {
  public:
    // Display preparation on linear RGB. Redistributes color inside isolated
    // compact stars, preserving each pixel's gain-calibrated luminance and the
    // aperture's integrated RGB flux. Does not sharpen or perform photometry.
    // Zero is an exact bypass; uncovered pixels and alpha are never modified.
    StarColorCleanupResult apply(const ImageBuffer&, const std::array<float, 3>& gains,
                                 float strength) const;
};
} // namespace photonstack
