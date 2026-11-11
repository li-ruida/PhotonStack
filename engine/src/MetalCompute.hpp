#pragma once
#include "photonstack/Registration.hpp"
namespace photonstack::detail {
// Full-frame, finite, linear RGBA similarity registration.
bool metalWarp(const ImageBuffer& image, SimilarityTransform transform, RegistrationInterpolation interpolation,
               ImageBuffer& output);
bool metalDemosaic(const std::vector<float>& mosaic, const std::array<int, 4>& colors, ImageBuffer& result,
                   bool guarded);
bool metalWinsorCenters(const std::vector<float>& tile, std::size_t pixels, std::size_t frames,
                        std::vector<float>& centers);
} // namespace photonstack::detail
