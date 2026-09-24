#pragma once
#include "photonstack/FitsCodec.hpp"
#include <array>
#include <functional>
#include <vector>

namespace photonstack {
struct SensorPatternSource {
    std::filesystem::path path;
    std::uintmax_t bytes = 0;
    std::int64_t modifiedTicks = 0;
    unsigned fold = 0;
};
struct SensorPatternOptions {
    unsigned minimumGroupSamples = 12;
    // >=12 native pixels; conservative edge where the CFA filters are supported.
    unsigned border = 24;
    // Measured field center in raw sensor coordinates, one per input. Each
    // training fold needs broad movement; this is a conservative eligibility
    // check, not a proof that all extended scene structure has been excluded.
    std::vector<std::array<double, 2>> sensorPositions;
    double minimumMotion = 8;
    double minimumCorrelation = .2;
    std::filesystem::path temporaryDirectory;
    std::function<void(double)> progress;
};
struct SensorPatternModel {
    // Signed additive residual, native CFA coordinates, one channel. Unknown
    // pixels are zero correction; validity is a SEPARATE grayscale mask.
    ImageBuffer correction, validity;
    unsigned excludedFold = 0;
    std::vector<std::size_t> trainingIndices;
    std::array<double, 4> retention{}, correlation{};
};
struct SensorPatternResult {
    bool ok = false;
    FitsSensorIdentity sensor;
    std::vector<SensorPatternSource> sources;
    std::array<SensorPatternModel, 3> models;
    std::string errorCode, message;
};
class SensorPatternBuilder {
public:
    // Frame i belongs to fold i%3 and MUST use models[i%3] when corrected.
    // A truly external exposure may use any model with matching acquisition.
    // Only raw, uncalibrated CFA inputs; no dark/bias/flat interaction is inferred.
    // This estimates a high-frequency additive pattern, not an actual dark/flat.
    // Adequate field movement and signal-leakage validation remain caller duties.
    // Smooth source confidence and a robust weighted location avoid abrupt
    // sample-exclusion changes. A positive correction still requires at least
    // minimumGroupSamples positive-weight samples in each included fold.
    // Disk cache: value + confidence (two floats) per sensor pixel per frame;
    // combination tiles <=32MiB. The cache is owned and removed by this call.
    SensorPatternResult build(const std::vector<std::filesystem::path>& inputs,
        const SensorPatternOptions& options = {}) const;
};
}
