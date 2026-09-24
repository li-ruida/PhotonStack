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
    // Median/MAD sky statistics keep isolated bright trails or hot pixels from
    // raising the detection threshold across an otherwise usable exposure.
    bool robustStatistics = false;
    // Count all separated detections while retaining only the brightest maxStars.
    bool countAllDetections = false;
};

struct StarDetectionResult {
    bool ok = false;
    std::vector<Star> stars;
    std::string errorCode;
    std::string message;
    // Complete only when countAllDetections is enabled; otherwise stars.size().
    std::size_t detectedCount = 0;
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
