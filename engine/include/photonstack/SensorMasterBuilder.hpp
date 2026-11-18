#pragma once
#include "photonstack/CalibrationMetadata.hpp"
#include "photonstack/ImageBuffer.hpp"
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace photonstack {
// Uninterpolated physical samples, including signed values. BLANK and floating
// nonfinite values become NaN; finite values outside Float32 range are errors.
class SensorFitsReader {
  public:
    explicit SensorFitsReader(const std::filesystem::path& path);
    ~SensorFitsReader();
    SensorFitsReader(SensorFitsReader&&) noexcept;
    SensorFitsReader& operator=(SensorFitsReader&&) noexcept;
    const CalibrationAcquisition& metadata() const;
    void read(std::size_t start, std::span<float> samples);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct SensorMasterSource {
    std::filesystem::path path;
    std::string sha;
};
struct SensorMasterOptions {
    std::string role = "bias", method = "median";
    // Raw input frames include their bias pedestal. An optional verified bias
    // removes it from darks; flats require either bias or matching dark-flat.
    std::optional<SensorMasterSource> bias, darkFlat;
    std::string darkFlatBiasState;
    double temperatureTolerance = 1;
    std::size_t memoryBytes = 512 * 1024 * 1024, tilePixels = 65536;
    unsigned minimumSamples = 2, workers = 4;
    std::function<void()> checkCancelled;
};
struct SensorMasterResult {
    ImageBuffer master, accepted;
    CalibrationAcquisition acquisition;
    std::string role, biasState, method;
    std::vector<SensorMasterSource> sources;
    std::vector<std::array<double, 4>> normalization;
    std::size_t invalidPixels = 0, missingSamples = 0, workingBytes = 0, tilePixels = 0;
    unsigned workers = 1;
};
// No registration, debayer, light-frame background normalization or clipping
// of negative values. Flat phases are normalized independently before stacking.
SensorMasterResult buildSensorMaster(std::vector<SensorMasterSource> sources, const SensorMasterOptions& options);
// Acquisition cards are canonical, not blindly copied from a single exposure.
// No sky solution, DATE-OBS, or input BSCALE/BZERO is propagated into a master.
void writeSensorMaster(const SensorMasterResult& result, const std::filesystem::path& path);
} // namespace photonstack
