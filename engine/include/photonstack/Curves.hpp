#pragma once

#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct CurvePoint {
    float input = 0.0F;
    float output = 0.0F;
};

enum class CurveChannel {
    RGB,
    Red,
    Green,
    Blue,
    Luminance,
};

struct CurvesOptions {
    std::vector<CurvePoint> points = {
        {.input = 0.0F, .output = 0.0F},
        {.input = 1.0F, .output = 1.0F},
    };
    CurveChannel channel = CurveChannel::RGB;
    bool preserveAlpha = true;
};

struct CurvesResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

class Curves {
  public:
    CurvesResult apply(const ImageBuffer& image, const CurvesOptions& options = {}) const;
};

} // namespace photonstack
