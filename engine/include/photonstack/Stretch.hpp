#pragma once

#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct StretchOptions {
    float blackPoint = 0.0F;
    float midPoint = 0.5F;
    float whitePoint = 1.0F;
    float arcsinhStrength = 0.0F;
    bool preserveColor = true;
};

struct AutoStretchOptions {
    float shadowsSigma = 2.8F;
    float targetBackground = 0.25F;
    float highlightClip = 0.999F;
    float arcsinhStrength = 0.0F;
};

struct StretchResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

class Stretch {
  public:
    StretchResult apply(const ImageBuffer& image, const StretchOptions& options) const;
    StretchResult applyAuto(const ImageBuffer& image, const AutoStretchOptions& options = {}) const;
    StretchOptions estimateAuto(const ImageBuffer& image, const AutoStretchOptions& options = {}) const;
};

} // namespace photonstack
