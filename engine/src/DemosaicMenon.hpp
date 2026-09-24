#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace photonstack::detail {
// Interleaved RGB, in the original sensor units. Non-finite neighborhoods are
// left non-finite so the caller can retain its finite-neighbor fallback.
std::vector<float> demosaicMenon(const std::vector<float>& mosaic, int width, int height,
                                 const std::array<int, 4>& colors, const std::array<float, 3>& gains);
} // namespace photonstack::detail
