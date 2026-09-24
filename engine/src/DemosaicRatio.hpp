#pragma once

#include <array>
#include <vector>

namespace photonstack::detail {
// Original sensor units, interleaved RGB. The RGBA baseline is the conservative
// Malvar estimate including its finite-neighbour fallback. Measured CFA sites
// are copied exactly; invalid estimates retain that baseline.
std::vector<float> demosaicRatio(const std::vector<float>& mosaic, int width, int height,
                                const std::array<int, 4>& colors,
                                const std::array<float, 3>& gains,
                                const std::vector<float>& baseline);
} // namespace photonstack::detail
