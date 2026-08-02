#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "photonstack/ImageBuffer.hpp"
#include "photonstack/StarDetector.hpp"

namespace photonstack {

struct FrameQualityOptions {
    StarDetectionOptions starDetection;
    float saturationThreshold = 0.98F;
};

struct FrameQualityResult {
    bool ok = false;
    std::size_t starCount = 0;
    float medianFwhm = 0.0F;
    float medianEccentricity = 0.0F;
    float background = 0.0F;
    float noise = 0.0F;
    float saturatedFraction = 0.0F;
    float score = 0.0F;
    std::string errorCode;
    std::string message;
};

class FrameQualityAnalyzer {
  public:
    FrameQualityResult analyze(const ImageBuffer& image, const FrameQualityOptions& options = {}) const;
    FrameQualityResult analyzeLuminance(std::uint32_t width, std::uint32_t height,
                                        std::span<const float> luminance,
                                        const FrameQualityOptions& options = {}) const;
};

} // namespace photonstack
