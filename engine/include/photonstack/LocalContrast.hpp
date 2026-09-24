#pragma once

#include <cstdint>
#include <string>

#include "photonstack/Curves.hpp"
#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct LocalContrastOptions {
    float amount = 0.25F;
    std::uint32_t radius = 8;
    float threshold = 0.0F;
    bool clampOutput = true;
    // Display-only band-limited luminance enhancement with compact-star and
    // dark-sky protection. False preserves the original unsharp-mask behavior.
    bool protectStructure = false;
    std::uint32_t fineRadius = 6;
    // Optional display styling inside the stellar mask; 1 preserves star color.
    float starChroma = 1.0F;
    // Optional display curve on the source-excluded coarse continuum. Residual
    // detail is recombined with one RGB scale bounded by the available gamut.
    // Empty preserves the existing protected-structure operation exactly.
    std::vector<CurvePoint> continuumCurvePoints;
};

struct LocalContrastResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

enum class CoolingContinuumEstimator { SourceExcluded, Opening };

struct ContinuumCoolingOptions {
    // Display styling: subtract the same smooth pedestal from encoded R/G,
    // retaining B. This deliberately lowers diffuse display luminance.
    double amount = 0.06;
    std::uint32_t radius = 32;
    // SourceExcluded detects sources and fills their masks; Opening uses a
    // fixed 13x13 grayscale opening. Both then smooth with the given radius.
    CoolingContinuumEstimator estimator = CoolingContinuumEstimator::SourceExcluded;
};

class LocalContrast {
  public:
    LocalContrastResult apply(const ImageBuffer& image, const LocalContrastOptions& options = {}) const;
    // Requires bounded sRGB RGB/RGBA. It is not a scientific white balance.
    // Compact residuals on a locally constant continuum are retained unless
    // the positive-floor guard limits the pedestal; curved backgrounds require
    // actual aperture checks. Alpha/coverage semantics match apply().
    LocalContrastResult coolContinuum(
        const ImageBuffer& image, const ContinuumCoolingOptions& options = {}) const;
};

} // namespace photonstack
