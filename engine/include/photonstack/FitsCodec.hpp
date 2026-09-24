#pragma once

#include <filesystem>

#include "photonstack/ImageCodec.hpp"
#include "photonstack/ImageMetadata.hpp"
#include "photonstack/Calibrator.hpp"

namespace photonstack {

// Acquisition identity used for sensor-domain models. Missing acquisition
// fields are rejected by inspectSensor rather than guessed from pixel values.
struct FitsSensorIdentity {
    bool ok = false;
    std::uint32_t width = 0, height = 0;
    std::string camera, bayer;
    int xOffset = 0, yOffset = 0;
    double exposure = 0, gain = 0;
    std::string errorCode, message;
    bool sameAcquisition(const FitsSensorIdentity& other) const;
};

class FitsCodec {
  public:
    InspectResult inspect(const std::filesystem::path& path) const;
    FitsSensorIdentity inspectSensor(const std::filesystem::path& path) const;
    ImageReadResult read(const std::filesystem::path& path) const;
    ImageReadResult read(const std::filesystem::path& path, const FitsDecodeOptions& options) const;
    // Apply sensor-domain calibration before CFA interpolation, retaining the
    // source header's Bayer pattern and offsets in the same decode operation.
    ImageReadResult read(const std::filesystem::path& path, const FitsDecodeOptions& options,
                         const CalibrationOptions* calibration,
                         const ImageBuffer* sensorPattern = nullptr) const;
    // sensorPattern is a matching single-channel, signed linear additive model
    // applied before debayering. It is distinct from bias/dark/flat calibration;
    // combining them is rejected until their training stage is explicitly modeled.
    ImageWriteResult write(const ImageBuffer& image, const std::filesystem::path& path) const;
};

bool isFitsPath(const std::filesystem::path& path);

} // namespace photonstack
