#pragma once

#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

enum class CalibrationBiasState {
    Unknown,
    Included,
    Removed,
};

struct CalibrationOptions {
    const ImageBuffer* dark = nullptr;
    const ImageBuffer* bias = nullptr;
    const ImageBuffer* flat = nullptr;
    CalibrationBiasState darkBiasState = CalibrationBiasState::Unknown;
    CalibrationBiasState flatBiasState = CalibrationBiasState::Unknown;
    float flatEpsilon = 1.0e-6F;
    bool clampNegativeValues = true;
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
    CalibrationResult calibrate(const ImageBuffer& light, const CalibrationOptions& options) const;
};

} // namespace photonstack
