#pragma once
#include "photonstack/ImageBuffer.hpp"
#include <array>
#include <string>
namespace photonstack {
struct StellarApertureOptions {
    double radius = 8, skyInner = 11, skyOuter = 15;
    // Zero means unknown; no ADC threshold is inferred from stacked pixels.
    double saturation = 0, minSNR = 20, maxCenterOffset = 2;
};
struct StellarApertureResult {
    bool valid = false;
    std::string status;
    double x = 0, y = 0, centerOffset = 0, area = 0;
    std::array<double, 3> flux{}, skySigma{}, backgroundNoise{}, snr{};
    // Sum w^2 * max(sample - fitted sky, 0). Divide by effective e-/unit
    // to estimate independent-pixel source shot variance; excludes covariance.
    std::array<double, 3> sourceShotCoefficient{};
    std::array<std::array<double, 3>, 3> skyPlane{}; // intercept, dx, dy
    std::array<unsigned, 3> skyUsed{};
};
// Native linear RGB photometry, one shared fitted center. Fractional circular
// aperture weights use 8x8 subpixels; robust planar annulus rejects contaminating
// sky samples. Background noise includes estimated sky-level uncertainty but
// excludes source shot noise and resampling covariance (gain/history unknown).
StellarApertureResult measureStellarAperture(const ImageBuffer& image, double predictedX, double predictedY,
                                             const StellarApertureOptions& options = {});
} // namespace photonstack
