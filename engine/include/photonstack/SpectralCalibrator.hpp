#pragma once
#include "photonstack/SkyProjection.hpp"
#include "photonstack/SpectralCalibration.hpp"
#include "photonstack/StellarPhotometry.hpp"
#include <set>
namespace photonstack {
struct SpectralReferenceStar {
    std::string id;
    // ICRS degrees at the image observation epoch. Catalog propagation is the
    // caller's responsibility; this API never silently assumes an epoch.
    double ra = 0, dec = 0;
    SampledSpectrum spectrum;
};
struct SpectralCalibrationOptions {
    StellarApertureOptions aperture;
    double maxRelativeSpectralError = .1;
    double maxApertureColorChange = .03;
    double maxValidationLogError = .2;
    std::set<std::string> excluded;
};
struct SpectralStarReview {
    std::string id, status;
    bool hasPosition = false;
    double predictedX = 0, predictedY = 0;
    StellarApertureResult photometry, expandedPhotometry;
    std::array<double, 3> predicted{}, spectralErrorBound{}, apertureColorChange{};
};
struct SpectralCalibrationReview {
    unsigned width = 0, height = 0;
    std::size_t detected = 0;
    std::array<double, 3> whiteFlux{};
    std::vector<SpectralStarReview> stars;
    SpectralColorFit fit;
};
// Shared native-pixel measurements, spectral-error checks, catalog/detected
// neighbor rejection and independent larger-aperture color-stability checks.
// Source pixels are read-only. Response accuracy and full photometric error
// (including atmospheric throughput) cannot be established by these gates.
SpectralCalibrationReview reviewSpectralCalibration(const ImageBuffer& image, const SolvedPanel& sky,
                                                    const std::vector<SpectralReferenceStar>& stars,
                                                    const std::array<SpectralBand, 3>& response,
                                                    const SampledSpectrum& white,
                                                    const SpectralCalibrationOptions& options = {});
// Apply only validated multiplicative gains, preserving signed linear values
// and coverage. Does not stretch, subtract a background or clip to display range.
ImageBuffer applySpectralCalibration(const ImageBuffer& source, const SpectralColorFit& fit);
} // namespace photonstack
