#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

enum class FrameSelectionOverride { Automatic, Keep, Reject };

struct SequenceQualityOptions {
    std::uint32_t sampleSide = 192;
    bool measureStars = true;
    bool measureTrails = true;
    double residualSigma = 6.0;
    double signalTolerance = 0.05;
    double minimumCoverage = 0.70;
    double maximumNoiseRatio = 2.0;
    double maximumFwhmRatio = 1.7;
    double maximumEccentricity = 0.80;
    double minimumStarRatio = 0.30;
    double maximumResidualFraction = 0.10;
    std::size_t minimumTemporalPeers = 4;
    bool applySelection = false;
    std::size_t minimumKeptFrames = 3;
    double minimumKeptFraction = 0.5;
    std::vector<FrameSelectionOverride> overrides;
};

// Samples share the registered reference grid. Noise and sky stay in input
// scientific units. Unknown coverage is never interpreted as black sky.
struct SequenceFrameSample {
    bool ok = false;
    std::uint32_t width = 0, height = 0, columns = 0, rows = 0;
    std::vector<float> luminance;
    std::vector<float> coverage;
    double sky = 0, noise = 0, validFraction = 0;
    bool starsMeasured = false;
    std::size_t starCount = 0;
    // Number of accepted native Gaussian fits (bounded to 128). Shape medians
    // remain zero when fewer than five fits are available; zero is not a PSF.
    std::size_t starShapeCount = 0;
    double medianFwhm = 0, medianEccentricity = 0;
    std::string errorCode, message;
};

struct SequenceTransientTrail {
    int sign = 1;
    std::size_t samples = 0;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

struct SequenceFrameAssessment {
    bool usable = false;
    bool recommendedKeep = false;
    bool selected = false;
    bool temporalAvailable = false;
    std::size_t comparedSamples = 0;
    double positiveFraction = 0, negativeFraction = 0;
    double residualRms = 0;
    double noiseRatio = 0, fwhmRatio = 0;
    // Line candidates in signed temporal outliers, not asserted object identities.
    // Coordinates refer to the registered reference canvas. Sparse sampling can
    // miss thin trails; pixel rejection operates independently at full resolution.
    std::vector<SequenceTransientTrail> trails;
    std::vector<std::string> reasons;
    std::vector<std::string> warnings;
};

struct SequenceQualityResult {
    bool ok = false;
    std::vector<SequenceFrameAssessment> frames;
    std::size_t selectedCount = 0;
    double medianNoise = 0, medianFwhm = 0;
    std::string errorCode, message;
};

class SequenceQualityAnalyzer {
  public:
    // Deterministic bounded sampling; does not modify image or reconstruct pixels.
    SequenceFrameSample sample(const ImageBuffer& image,
                               const SequenceQualityOptions& options = {}) const;
    // Temporal residual centers exclude the frame being assessed. Call after
    // registration, with consistent linear photometric units and sampling grids.
    SequenceQualityResult assess(const std::vector<SequenceFrameSample>& frames,
                                 const SequenceQualityOptions& options = {}) const;
};

} // namespace photonstack
