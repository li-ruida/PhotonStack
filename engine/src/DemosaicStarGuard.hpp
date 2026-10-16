#pragma once

#include <array>
#include <span>
#include "photonstack/ImageBuffer.hpp"

namespace photonstack::detail {
// Constrain significant negative Malvar lobes around isolated stars. Native
// CFA samples and each repaired aperture's integrated RGB remain unchanged.
void guardDemosaicStars(ImageBuffer& image, std::span<const float> mosaic,
                       const std::array<int, 4>& colors,
                       const std::array<float, 3>& gains);
}
