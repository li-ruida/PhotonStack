#pragma once
#include <array>
#include <string>
#include <vector>
namespace photonstack {
// Wavelengths are nm. Spectrum values are F_lambda (one common energy-flux
// density unit); response components are dimensionless QE/transmission.
struct SpectralCurve {
    std::vector<double> wavelength, value;
};
struct SampledSpectrum {
    SpectralCurve flux;
    std::vector<double> error;
};
using SpectralBand = std::vector<SpectralCurve>;
struct SyntheticFlux {
    // Integral F_lambda * product(response) * lambda d(lambda). A shared 1/hc
    // factor is omitted: these are relative photon signals, not electron rates.
    double value = 0;
    // Conservative fully correlated bound. Not a covariance-derived sigma.
    double errorBound = 0;
    bool hasErrors = false;
    double meanWavelength = 0, rmsBandwidth = 0;
};
SyntheticFlux integratePhotonSpectrum(const SampledSpectrum& spectrum, const SpectralBand& response);
struct SpectralColorSample {
    std::string id;
    double x = 0, y = 0;
    std::array<double, 3> observed{}, predicted{};
};
struct SpectralColorResidual {
    std::string id, status;
    double x = 0, y = 0;
    std::array<double, 3> logGain{}, residual{};
    bool heldOut = false;
};
struct SpectralColorFit {
    bool valid = false;
    std::string reason;
    std::array<double, 3> gains{1, 1, 1}, instrumentGains{1, 1, 1}, whiteRatios{1, 1, 1};
    std::array<double, 3> trainingScatter{}, validationMedian{}, validationP90{};
    unsigned training = 0, validation = 0, rejected = 0, validationRegions = 0;
    std::vector<SpectralColorResidual> stars;
};
// Fit log color ratios with a robust location, G anchored at 1. Spatially sorted
// every fifth eligible star is held out before estimating/rejecting fit outliers.
// White flux is integrated through the same response, not inferred from image
// statistics. Failed held-out validation retains diagnostics but forbids apply.
SpectralColorFit fitSpectralColors(const std::vector<SpectralColorSample>& stars,
                                   const std::array<double, 3>& whiteFlux, unsigned width, unsigned height,
                                   double maxValidationLogError = 0.2);
} // namespace photonstack
