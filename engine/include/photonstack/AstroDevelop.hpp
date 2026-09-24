#pragma once

#include "photonstack/ImageBuffer.hpp"
#include <array>
#include <cstddef>
#include <string>

namespace photonstack {
enum class AstroToneCurve { Rational, Asinh };

struct AstroDevelopOptions {
    bool stellarBalance = true;
    std::array<float, 3> gains = {1, 1, 1};
    float background = 0.025F;
    // Half-response in input physical units; zero estimates it from the scene.
    float toneScale = 0.0F;
    float brightness = 1.0F;
    float saturation = 1.0F;
    // Display-only neutralization of color below three robust sky-noise sigmas.
    // Zero bypasses; the luminance transfer and scientific master are unchanged.
    float shadowNeutralization = 0.0F;
    float starExposure = 1.0F; // Linear stellar flux multiplier, 0.05...1.
    // Display-mapped local-background-subtracted stellar peak at which
    // attenuation begins, ramping smoothly to full strength at white.
    // Zero preserves attenuation of all detected stars.
    float starPeakThreshold = 0.0F;
    AstroToneCurve toneCurve = AstroToneCurve::Rational;
    float whitePoint = 0; // Asinh normalization in physical units, zero estimates it.
};
struct AstroDevelopResult {
    bool ok = false;
    ImageBuffer image;
    std::array<float, 3> gains = {1, 1, 1};
    std::array<float, 3> sky = {};
    std::size_t calibrationStars = 0;
    std::size_t adjustedStars = 0;
    float toneScale = 0;
    float whitePoint = 0;
    double shadowNoise = 0;
    std::string errorCode;
    std::string message;
};
class AstroDevelop {
  public:
    // Statistical stellar white balance, not catalog-based photometric calibration.
    AstroDevelopResult apply(const ImageBuffer&, const AstroDevelopOptions& = {}) const;
};
} // namespace photonstack
