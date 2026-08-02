#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct Star {
    float x = 0.0F;
    float y = 0.0F;
    float flux = 0.0F;
    float peak = 0.0F;
    float fwhm = 0.0F;
    float eccentricity = 0.0F;
};

struct StarDetectionOptions {
    float sigmaThreshold = 3.0F;
    float minPeak = 0.05F;
    std::uint32_t border = 1;
    std::size_t maxStars = 1000;
};

struct StarDetectionResult {
    bool ok = false;
    std::vector<Star> stars;
    std::string errorCode;
    std::string message;
};

class StarDetector {
  public:
    StarDetectionResult detect(const ImageBuffer& image, const StarDetectionOptions& options = {}) const;
    StarDetectionResult detectLuminance(std::uint32_t width, std::uint32_t height,
                                        std::span<const float> luminance,
                                        const StarDetectionOptions& options = {}) const;
    StarDetectionResult detectLuminanceWithCoverage(std::uint32_t width, std::uint32_t height,
                                                    std::span<const float> luminance,
                                                    std::span<const float> coverage,
                                                    const StarDetectionOptions& options = {}) const;
};

} // namespace photonstack
