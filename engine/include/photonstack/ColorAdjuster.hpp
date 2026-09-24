#pragma once

#include <cstddef>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct BackgroundNeutralizationOptions {
    float strength = 1.0F;
    float epsilon = 1.0e-6F;
};

struct SaturationOptions {
    float amount = 0.2F;
};

struct GreenCastSuppressionOptions {
    float amount = 0.65F;
    float backgroundLimit = 0.32F;
    float greenExcessThreshold = 0.01F;
    // Opt-in display styling; defaults retain the existing background weighting.
    bool averageNeutral = false;
    // Preserve linear-sRGB Y (and therefore CIE L*) with smooth gamut compression.
    // Requires bounded sRGB display input, never an unbounded scientific frame.
    bool preserveLightness = false;
};

struct ColorAdjustmentResult {
    bool ok = false;
    ImageBuffer image;
    float redBackground = 0.0F;
    float greenBackground = 0.0F;
    float blueBackground = 0.0F;
    std::size_t affectedPixels = 0;
    std::string errorCode;
    std::string message;
};

class ColorAdjuster {
  public:
    ColorAdjustmentResult neutralizeBackground(const ImageBuffer& image,
                                               const BackgroundNeutralizationOptions& options = {}) const;
    ColorAdjustmentResult adjustSaturation(const ImageBuffer& image, const SaturationOptions& options = {}) const;
    ColorAdjustmentResult suppressGreenCast(const ImageBuffer& image,
                                            const GreenCastSuppressionOptions& options = {}) const;
};

} // namespace photonstack
