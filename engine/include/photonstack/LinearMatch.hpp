#pragma once
#include "photonstack/ImageBuffer.hpp"
#include <array>
#include <cstddef>

namespace photonstack {
struct LinearMatchOptions {
    double lowQuantile = .02, highQuantile = .98;
};
struct LinearMatchCoefficients { double gain = 1, offset = 0; };
struct LinearMatchRegion {
    std::size_t count = 0;
    double beforeMean = 0, afterMean = 0, afterRms = 0;
};
struct LinearMatchMeasurement {
    LinearMatchCoefficients coefficients;
    std::size_t candidates = 0, fitSamples = 0, checkSamples = 0, downweighted = 0;
    double beforeRms = 0, afterRms = 0, medianResidual = 0, p95AbsoluteResidual = 0;
    double referenceSpan = 0, validFraction = 0;
    std::array<LinearMatchRegion, 16> regions;
};
struct LinearMatchResult {
    ImageBuffer image, validity, residual;
    LinearMatchMeasurement measurement;
};
// Already registered linear mono images. Fit reference = gain * target + offset.
// This statistical normalization is not a physical inter-filter calibration.
// Alternating 8x8 spatial blocks are held out from fitting. Residual = output-reference.
LinearMatchResult matchLinearMaster(const ImageBuffer& reference, const ImageBuffer& target,
    LinearMatchOptions options = {}, const LinearMatchCoefficients* replay = nullptr);
} // namespace photonstack
