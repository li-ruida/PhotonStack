#pragma once

#include <array>
#include <limits>
#include <string>
#include <vector>
#include "photonstack/ImageBuffer.hpp"

namespace photonstack {
// Coordinates refer to each image's native pixels: input x,y, reference x,y.
using StarColorPair = std::array<float, 4>;
struct StarColorCalibrationOptions {
    float inputSaturation = std::numeric_limits<float>::infinity();
    float referenceSaturation = std::numeric_limits<float>::infinity();
};
struct StarColorCalibrationResult {
    bool ok = false;
    std::array<float, 3> gains{1, 1, 1};
    std::array<float, 3> validationMedianLogError{};
    std::array<float, 3> validationP90AbsoluteLogError{};
    std::size_t trainingStars = 0, validationStars = 0, rejectedStars = 0;
    std::string errorCode, message;
};
// Relative color transfer between linear RGB images, not an absolute/catalog
// calibration. Native aperture radius 8 and planar sky annulus 11..15 pixels.
// Alternating spatially sorted stars are held out from the robust gain fit.
// Images and coordinates are never resampled or modified.
class StarColorCalibrator {
  public:
    StarColorCalibrationResult estimate(const ImageBuffer& input, const ImageBuffer& reference,
        const std::vector<StarColorPair>& pairs, const StarColorCalibrationOptions& options = {}) const;
};
} // namespace photonstack
