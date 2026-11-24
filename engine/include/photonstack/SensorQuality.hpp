#pragma once
#include "photonstack/SensorMasterBuilder.hpp"

namespace photonstack {
struct SensorQualityOptions {
    double sigma = 8, absoluteTolerance = 10;
    double minimumFlatResponse = .5, maximumFlatResponse = 1.5;
    std::size_t memoryBytes = 512 * 1024 * 1024;
};
struct SensorQualityPhase {
    double median = 0, madSigma = 0, minimum = 0, maximum = 0;
    std::size_t finite = 0, invalid = 0, low = 0, high = 0;
};
struct SensorQualityResult {
    SensorMasterSource source;
    CalibrationAcquisition acquisition;
    SensorQualityOptions options;
    std::string role;
    std::vector<SensorQualityPhase> phases;
    ImageBuffer badPixels;
    std::size_t rejected = 0;
};
// Candidate defects, never an automatic assertion of physical sensor damage.
SensorQualityResult inspectSensorQuality(SensorMasterSource source, const SensorQualityOptions& options);
void writeSensorDefectMap(const SensorQualityResult& result, const std::filesystem::path& output);
void validateSensorDefectMap(const CalibrationAcquisition& input, const CalibrationAcquisition& map);
}
