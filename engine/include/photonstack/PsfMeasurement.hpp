#pragma once
#include "photonstack/ImageBuffer.hpp"
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace photonstack {
struct PsfMeasureOptions {
    std::string channel = "luminance";
    double sigmaThreshold = 5;
    // Zero means unknown, not proof that all stars are unsaturated.
    double saturation = 0;
    std::vector<std::size_t> excluded;
};
struct PsfStarMeasurement {
    std::size_t id = 0;
    unsigned sampleX = 0, sampleY = 0;
    double x = 0, y = 0, xx = 0, yy = 0, xy = 0, residual = 0;
    std::string status;
};
struct PsfRegionMeasurement {
    std::size_t count = 0;
    double fwhm = 0, xx = 0, yy = 0, xy = 0;
};
struct PsfMeasurement {
    unsigned width = 0, height = 0;
    std::size_t detected = 0, accepted = 0;
    double xx = 0, yy = 0, xy = 0;
    double majorFwhm = 0, minorFwhm = 0, angleDegrees = 0;
    double fwhmP10 = 0, fwhmP50 = 0, fwhmP90 = 0;
    double medianResidual = 0;
    bool spatiallyVariable = false;
    std::array<PsfRegionMeasurement,16> regions;
    std::vector<PsfStarMeasurement> stars;
};
// Elliptical Gaussian + local sky fits at native pixel scale. This estimates a
// single model from the field; it is neither a pixel-integrated ePSF nor an
// optical-resolution measurement. The input science pixels remain unchanged.
PsfMeasurement measurePsf(const ImageBuffer& image, const PsfMeasureOptions& options = {});
ImageBuffer gaussianPsfKernel(double xx, double yy, double xy);
} // namespace photonstack
