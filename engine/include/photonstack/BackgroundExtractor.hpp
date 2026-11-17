#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

enum class BackgroundMode {
    Subtract,
    Divide,
};

struct BackgroundExtractionOptions {
    BackgroundMode mode = BackgroundMode::Subtract;
    float strength = 1.0F;
    float epsilon = 1.0e-6F;
    bool preserveBrightness = true;
    bool clampOutput = true;
};

struct BackgroundExclusionEllipse {
    float x = 0, y = 0, major = 1, minor = 1, angleDegrees = 0;
};
struct BackgroundGridOptions {
    BackgroundExtractionOptions extraction;
    std::uint32_t columns = 6;
    std::uint32_t rows = 4;
    bool protectBrightTargets = true;
    bool extrapolateEdges = false;
    // Cells retaining less than 25% usable pixels after exclusions are filled
    // from valid sky cells, rather than estimating a pedestal from tiny slivers.
    std::vector<BackgroundExclusionEllipse> exclusions;
};

struct BackgroundSamplePoint {
    // Zero-based pixel centers in the decoded image (same orientation as previews).
    double x = 0, y = 0;
    std::uint32_t radius = 4;
    bool enabled = true;
};
struct BackgroundSampleMeasurement {
    BackgroundSamplePoint point;
    std::array<double, 3> measured{}, fitted{};
    std::array<bool, 3> used{};
    // RBF leave-one-out residual for retained samples; prediction residual for rejected samples.
    std::array<double, 3> validationResidual{};
    std::string status;
};

struct BackgroundPolynomialOptions {
    BackgroundExtractionOptions extraction;
    std::uint32_t columns = 32, rows = 48;
    // Total degree, not degree per axis. Two retains the historical quadratic;
    // higher orders need well-distributed sky samples and target exclusions.
    std::uint32_t degree = 2;
    bool manualSamples = false;
    bool diagnostics = false;
    std::vector<BackgroundSamplePoint> samples;
    std::vector<BackgroundExclusionEllipse> exclusions;
};

struct BackgroundRBFOptions {
    // Shared sky sampling and correction options; polynomial degree is unused.
    BackgroundPolynomialOptions sampling;
    double smoothing = 0.001;
    bool rejectOutliers = true;
};

struct BackgroundExtractionResult {
    bool ok = false;
    ImageBuffer image;
    // Allocated only when requested. Difference is input minus corrected output.
    ImageBuffer modelImage, differenceImage;
    std::vector<BackgroundSampleMeasurement> measurements;
    std::array<double, 3> residualRMS{}, validationRMS{};
    float background = 0.0F;
    std::vector<float> backgroundGrid;
    std::uint32_t gridColumns = 0;
    std::uint32_t gridRows = 0;
    std::uint32_t sampledGridCells = 0;
    std::uint32_t filledGridCells = 0;
    std::string errorCode;
    std::string message;
};

class BackgroundExtractor {
  public:
    BackgroundExtractionResult extractPolynomial(const ImageBuffer& image,
                                                 const BackgroundPolynomialOptions& options) const;
    BackgroundExtractionResult extractRBF(const ImageBuffer& image, const BackgroundRBFOptions& options) const;
    BackgroundExtractionResult extractGlobal(const ImageBuffer& image,
                                             const BackgroundExtractionOptions& options) const;
    BackgroundExtractionResult extractGrid(const ImageBuffer& image, const BackgroundGridOptions& options) const;
};

} // namespace photonstack
