#pragma once
#include "photonstack/SensorMasterBuilder.hpp"

namespace photonstack {
struct SensorCalibrationMaster {
    SensorMasterSource source;
    std::string biasState;
};
struct SensorCalibrationOptions {
    std::optional<SensorCalibrationMaster> bias, dark, flat;
    std::optional<SensorMasterSource> badPixels;
    double temperatureTolerance = 1;
    std::size_t memoryBytes = 512 * 1024 * 1024, tilePixels = 65536;
    unsigned workers = 4;
    std::function<void()> checkCancelled;
};
struct SensorCalibrationResult {
    ImageBuffer calibrated, valid;
    CalibrationAcquisition acquisition;
    SensorMasterSource input;
    SensorCalibrationOptions options;
    std::array<double, 4> flatMeans{1, 1, 1, 1};
    std::size_t invalidPixels = 0, negativePixels = 0, tilePixels = 0, workingBytes = 0;
    unsigned workers = 1;
    bool biasOnLight = false, biasOnFlat = false;
};
// Strict metadata and preparation-state preflight; no pixel quality claim.
// Source digests are bound and checked separately by the executing operation.
void validateSensorCalibration(const std::filesystem::path& input, const SensorCalibrationOptions& options);
void validateSensorCalibration(const CalibrationAcquisition& light, const SensorCalibrationOptions& options,
                               const std::map<std::filesystem::path, CalibrationAcquisition>& masters);
// Owned by one sequential calibration job. Readers retain their verified file
// descriptors and validate their pathname and file version after each light.
class SensorCalibrationSession {
  public:
    SensorCalibrationResult calibrate(SensorMasterSource input, const SensorCalibrationOptions& options);

  private:
    std::map<std::filesystem::path, SensorFitsReader> readers_;
    std::optional<std::array<double, 4>> flatMeans_;
    std::filesystem::path flatPath_, flatBiasPath_;
};
SensorCalibrationResult calibrateSensorFrame(SensorMasterSource input, const SensorCalibrationOptions& options);
// Same pixel geometry: preserve source acquisition/WCS cards, replace storage
// scaling and stale statistics, and mark calibrated sensor-domain output.
void writeCalibratedSensor(const SensorCalibrationResult& result, const std::filesystem::path& path);
} // namespace photonstack
