#include "photonstack/CfaCalibration.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace photonstack {
namespace {
void match(const CfaSensorInfo& a, const CfaSensorInfo& b, bool exposure) {
    if (a.width != b.width || a.height != b.height || a.colors != b.colors)
        throw std::invalid_argument("CFA calibration requires matching sensor dimensions and Bayer phase");
    if (!a.camera.empty() && !b.camera.empty() && a.camera != b.camera)
        throw std::invalid_argument("CFA calibration camera does not match");
    const auto equal = [](auto x, auto y) { return !x || !y || std::abs(*x - *y) <= 1e-6; };
    if (!equal(a.gain, b.gain) || !equal(a.binX, b.binX) || !equal(a.binY, b.binY))
        throw std::invalid_argument("CFA calibration gain or binning does not match");
    if (exposure && !equal(a.exposure, b.exposure))
        throw std::invalid_argument("Dark and light exposure times must match; dark scaling is not applied");
}
}
void CfaCalibration::validate(const CfaSensorInfo& input) const {
    match(reference, input, requiresMatchingExposure);
}
float CfaCalibration::apply(std::size_t pixel, float value) const {
    const double corrected = (double(value) - subtract[pixel]) * multiply[pixel];
    if (!std::isfinite(corrected)) return std::numeric_limits<float>::quiet_NaN();
    if (std::abs(corrected) > std::numeric_limits<float>::max())
        throw std::runtime_error("Calibrated CFA sample exceeds Float32 range");
    return static_cast<float>(corrected);
}
CfaCalibration prepareCfaCalibration(const std::filesystem::path& reference, const CfaCalibrationOptions& options) {
    const auto known = [](auto state) {
        return state == CalibrationBiasState::Included || state == CalibrationBiasState::Removed;
    };
    if ((!options.dark.empty() && !known(options.darkBiasState)) ||
        (!options.flat.empty() && !known(options.flatBiasState)))
        throw std::invalid_argument("Specify whether each dark/flat master includes bias");
    if ((!options.dark.empty() && options.darkBiasState == CalibrationBiasState::Removed && options.bias.empty()) ||
        (!options.flat.empty() && options.flatBiasState == CalibrationBiasState::Included && options.bias.empty()))
        throw std::invalid_argument("A bias master is required for a bias-removed dark or a bias-included flat");
    CfaCalibration result;
    result.reference = inspectCfaSensor(reference);
    result.requiresMatchingExposure = !options.dark.empty();
    const std::size_t count = std::size_t(result.reference.width) * result.reference.height;
    result.subtract.assign(count, 0); result.multiply.assign(count, 1);
    const auto read = [&](const auto& path, bool exposure) {
        const auto info = inspectCfaSensor(path);
        match(result.reference, info, exposure);
        if (result.reference.camera.empty()) result.reference.camera = info.camera;
        if (!result.reference.gain) result.reference.gain = info.gain;
        if (!result.reference.binX) result.reference.binX = info.binX;
        if (!result.reference.binY) result.reference.binY = info.binY;
        if (exposure && !result.reference.exposure) result.reference.exposure = info.exposure;
        return readCfaSensor(path);
    };
    ImageBuffer bias;
    if (!options.bias.empty()) bias = read(options.bias, false);
    const bool lightBias = !options.bias.empty() &&
        (options.dark.empty() || options.darkBiasState == CalibrationBiasState::Removed);
    if (lightBias) for (std::size_t i = 0; i < count; ++i) result.subtract[i] = bias.pixels[i];
    if (!options.dark.empty()) {
        const auto dark = read(options.dark, true);
        for (std::size_t i = 0; i < count; ++i) result.subtract[i] += dark.pixels[i];
    }
    if (!options.flat.empty()) {
        const auto flat = read(options.flat, false);
        const bool flatBias = options.flatBiasState == CalibrationBiasState::Included;
        std::array<double, 4> sums{};
        std::array<std::size_t, 4> counts{};
        for (std::size_t i = 0; i < count; ++i) {
            const double value = double(flat.pixels[i]) - (flatBias ? bias.pixels[i] : 0.0);
            result.multiply[i] = value;
            const auto phase = 2 * ((i / result.reference.width) % 2) + i % result.reference.width % 2;
            if (std::isfinite(value) && value > 0) { sums[phase] += value; ++counts[phase]; }
        }
        for (unsigned p = 0; p < 4; ++p) {
            if (!counts[p] || !std::isfinite(sums[p]) || sums[p] <= 0)
                throw std::invalid_argument("Flat has no valid positive samples in a Bayer phase");
            sums[p] /= double(counts[p]);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto phase = 2 * ((i / result.reference.width) % 2) + i % result.reference.width % 2;
            const double normalized = result.multiply[i] / sums[phase];
            result.multiply[i] = std::isfinite(normalized) && normalized > 1e-6
                ? 1.0 / normalized : std::numeric_limits<double>::quiet_NaN();
        }
    }
    return result;
}
}
