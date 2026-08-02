#pragma once

#include <cstdint>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct FrameNormalizationOptions {
    float targetBackground = 0.0F;
    float targetScale = 0.0F;
    float epsilon = 1.0e-6F;
    bool clampOutput = true;
};

struct LocalNormalizationOptions {
    FrameNormalizationOptions normalization;
    std::uint32_t columns = 6;
    std::uint32_t rows = 4;
};

struct FrameNormalizationResult {
    bool ok = false;
    ImageBuffer image;
    float inputBackground = 0.0F;
    float outputBackground = 0.0F;
    float scale = 1.0F;
    std::string errorCode;
    std::string message;
};

class FrameNormalizer {
  public:
    FrameNormalizationResult normalize(const ImageBuffer& image, const FrameNormalizationOptions& options = {}) const;
    FrameNormalizationResult matchReference(const ImageBuffer& image, const ImageBuffer& reference,
                                            const FrameNormalizationOptions& options = {}) const;
    FrameNormalizationResult matchReferenceLocal(const ImageBuffer& image, const ImageBuffer& reference,
                                                 const LocalNormalizationOptions& options = {}) const;
};

} // namespace photonstack
