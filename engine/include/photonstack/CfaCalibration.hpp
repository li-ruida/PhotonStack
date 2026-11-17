#pragma once
#include "photonstack/ImageBuffer.hpp"
#include "photonstack/Calibrator.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <vector>

namespace photonstack {
struct CfaSensorInfo {
    std::uint32_t width = 0, height = 0;
    std::array<char, 4> colors{}; // Effective stored-row Bayer phases, including offsets.
    std::string camera;
    std::optional<double> exposure, gain, binX, binY;
};
CfaSensorInfo inspectCfaSensor(const std::filesystem::path& path);
ImageBuffer readCfaSensor(const std::filesystem::path& path);

struct CfaCalibrationOptions {
    std::filesystem::path bias, dark, flat;
    CalibrationBiasState darkBiasState = CalibrationBiasState::Unknown;
    CalibrationBiasState flatBiasState = CalibrationBiasState::Unknown;
};
struct CfaCalibration {
    CfaSensorInfo reference;
    bool requiresMatchingExposure = false;
    std::vector<double> subtract, multiply;
    void validate(const CfaSensorInfo& input) const;
    float apply(std::size_t pixel, float value) const;
};
// Calibrate each original sensor sample before interpolation or line extraction.
// Flat means are independent for R, G1, G2 and B; invalid samples remain missing.
CfaCalibration prepareCfaCalibration(const std::filesystem::path& reference,
                                     const CfaCalibrationOptions& options);
}
