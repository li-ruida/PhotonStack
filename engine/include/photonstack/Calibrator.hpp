#pragma once

#include <string>

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
    CalibrationResult calibrate(const ImageBuffer& light, const CalibrationOptions& options) const;
};

} // namespace photonstack
