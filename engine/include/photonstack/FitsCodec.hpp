#pragma once

#include "photonstack/SkyProjection.hpp"
#include "photonstack/CfaCalibration.hpp"
#include <filesystem>
#include <optional>

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

enum class DualBandSampling { CellAverage, RedGrid };

struct DualBandPlanes {
    ImageBuffer hydrogen, oxygen;
};

// Estimates H-alpha and OIII from one linear H-alpha/OIII Bayer exposure.
// RedGrid samples both lines at the red photosite of each complete 2x2 cell.
// CellAverage preserves the historical three-non-red-samples estimate.
DualBandPlanes readDualBandPlanes(const std::filesystem::path& path, const CfaCalibration* calibration = nullptr,
                                  DualBandSampling sampling = DualBandSampling::RedGrid);

struct LinearScienceImage {
    ImageBuffer image;
    std::optional<SolvedPanel> sky;
};
// Linear mono/RGB/RGBA FITS, decoded to RGBA with nonfinite colors masked.
// Rejects CFA and explicit nonlinear encoding; validates supplied WCS.
LinearScienceImage readLinearScienceImage(const std::filesystem::path& path);

struct LinearMonoMaster {
    ImageBuffer image;
    std::optional<SolvedPanel> sky;
};
// Rejects CFA mosaics, display encodings and incomplete/unsupported WCS.
LinearMonoMaster readLinearMonoMaster(const std::filesystem::path& path);

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
    ImageWriteResult write(const ImageBuffer& image, const std::filesystem::path& path,
                           const TanGrid* sky = nullptr,
                           const std::vector<SipTerm>* distortion = nullptr) const;
};

bool isFitsPath(const std::filesystem::path& path);

} // namespace photonstack
