#pragma once

#include <string>
#include <array>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

enum class CalibrationBiasState {
    Unknown,
    Included,
    Removed,
};

struct CalibrationFlatMeans {
    const ImageBuffer* flat = nullptr;
    const ImageBuffer* bias = nullptr;
    std::vector<double> channels;
};

// Run-owned evidence only: referenced master buffers must remain immutable
// for its lifetime. The light is still validated on every calibration call.
class CalibrationMasterValidation {
    friend class Calibrator;
    std::array<const ImageBuffer*, 3> masters_{};
    bool valid_ = false;
};

struct CalibrationOptions {
    const ImageBuffer* dark = nullptr;
    const ImageBuffer* bias = nullptr;
    const ImageBuffer* flat = nullptr;
    CalibrationBiasState darkBiasState = CalibrationBiasState::Unknown;
    CalibrationBiasState flatBiasState = CalibrationBiasState::Unknown;
    float flatEpsilon = 1.0e-6F;
    bool clampNegativeValues = true;
    // Optional session evidence for immutable master pixels.
    const CalibrationFlatMeans* flatMeans = nullptr;
    const CalibrationMasterValidation* masterValidation = nullptr;
};

struct CalibrationResult {
    bool ok = false;
    ImageBuffer image;
    bool subtractedBiasFromLight = false;
    bool subtractedBiasFromFlat = false;
    std::string errorCode;
    std::string message;
};

const char* calibrationBiasStateName(CalibrationBiasState state);

class Calibrator {
  public:
    static CalibrationFlatMeans measureFlat(const CalibrationOptions& options);
    static CalibrationMasterValidation validateMasters(const CalibrationOptions& options);
    CalibrationResult calibrate(const ImageBuffer& light, const CalibrationOptions& options) const;
};

} // namespace photonstack
