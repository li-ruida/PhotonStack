#include "photonstack/SensorQuality.hpp"
#include "photonstack/FileDigest.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace photonstack {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
double median(std::vector<float>& values) {
    const auto n = values.size(), middle = n / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    return n % 2 ? values[middle] : (double(values[middle]) + *std::max_element(values.begin(), values.begin() + middle)) / 2;
}
}
SensorQualityResult inspectSensorQuality(SensorMasterSource source, const SensorQualityOptions& options) {
    require(std::isfinite(options.sigma) && options.sigma >= 1 && options.sigma <= 100 &&
            std::isfinite(options.absoluteTolerance) && options.absoluteTolerance >= 0 &&
            std::isfinite(options.minimumFlatResponse) && options.minimumFlatResponse > 0 && options.minimumFlatResponse < 1 &&
            std::isfinite(options.maximumFlatResponse) && options.maximumFlatResponse > 1 && options.maximumFlatResponse <= 100 &&
            options.memoryBytes >= 4 * 1024 * 1024 && options.memoryBytes <= 8ULL * 1024 * 1024 * 1024, "Invalid sensor quality settings");
    source.path = std::filesystem::canonical(source.path);
    const auto sha = fileSHA256(source.path); require(source.sha.empty() || source.sha == sha, "Quality source changed"); source.sha = sha;
    SensorFitsReader reader(source.path); const auto& a = reader.metadata();
    const auto role = calibrationFrameType(a);
    require(a.channels == 1 && a.issues.empty() && a.text.contains("sampling") && a.text.contains("domain") && a.text.at("domain") == "linear" &&
            (role == "dark" || role == "bias" || role == "flat" || role == "dark-flat"), "Inspect a linear CFA or mono bias, dark or flat master");
    const auto count = std::size_t(a.width) * a.height;
    require(count > 0 && count <= (options.memoryBytes - 1024 * 1024) / 16, "Sensor quality exceeds memory budget");
    SensorQualityResult result; result.source = source; result.acquisition = a; result.options = options; result.role = role;
    const bool cfa = a.text.at("sampling").starts_with("CFA:");
    result.phases.resize(cfa ? 4 : 1);
    const auto phase = [&](std::size_t p) { return cfa ? (p / a.width % 2) * 2 + p % a.width % 2 : 0; };
    std::vector<float> values(count); reader.read(0, values);
    auto& mask = result.badPixels;
    mask.width = a.width; mask.height = a.height; mask.channels = 1; mask.format = PixelFormat::Float32Gray; mask.pixels.resize(count);
    std::vector<float> work; work.reserve(count);
    for (std::size_t q = 0; q < result.phases.size(); ++q) {
        auto& stats = result.phases[q]; work.clear();
        for (std::size_t p = 0; p < count; ++p) if (phase(p) == q) {
            if (std::isfinite(values[p])) work.push_back(values[p]); else ++stats.invalid;
        }
        require(!work.empty(), "A sensor phase has no finite samples");
        stats.finite = work.size();
        const auto range = std::minmax_element(work.begin(), work.end()); stats.minimum = *range.first; stats.maximum = *range.second;
        stats.median = median(work);
        for (auto& v : work) v = float(std::abs(double(v) - stats.median));
        stats.madSigma = 1.482602218505602 * median(work);
        require(std::isfinite(stats.madSigma), "Quality dispersion overflows");
        if (role == "flat") require(stats.median > 0, "Flat phase has no positive median response");
        const auto tolerance = std::max(options.absoluteTolerance, options.sigma * stats.madSigma);
        const auto low = role == "flat" ? stats.median * options.minimumFlatResponse : stats.median - tolerance;
        const auto high = role == "flat" ? stats.median * options.maximumFlatResponse : stats.median + tolerance;
        for (std::size_t p = 0; p < count; ++p) if (phase(p) == q) {
            const auto v = values[p];
            if (std::isfinite(v)) { stats.low += v < low; stats.high += v > high; }
            if (!std::isfinite(v) || v < low || v > high) { mask.pixels[p] = 1; ++result.rejected; }
        }
    }
    require(fileSHA256(source.path) == sha, "Quality source changed during reading");
    return result;
}
void validateSensorDefectMap(const CalibrationAcquisition& input, const CalibrationAcquisition& map) {
    require(map.channels == 1 && map.width == input.width && map.height == input.height && map.issues.empty() &&
            map.text.contains("badPixelMap") && map.text.at("badPixelMap") == "defect-v1", "Invalid sensor defect map geometry or type");
    require(map.text.contains("badPixelSourceRole"), "Defect map has no source role");
    const auto& role = map.text.at("badPixelSourceRole");
    require(role == "bias" || role == "dark" || role == "flat" || role == "dark-flat", "Invalid defect map source role");
    if (role == "flat") for (const auto* key : {"filter", "opticalTrain"})
        require(input.text.contains(key) && map.text.contains(key) && input.text.at(key) == map.text.at(key), "Flat-derived defect candidates require the same filter and optical train");
    for (const auto* key : {"camera", "cameraSerial", "readoutMode", "sampling"})
        require(input.text.contains(key) && map.text.contains(key) && input.text.at(key) == map.text.at(key), "Defect map sensor identity differs or is unknown");
    for (const auto* key : {"gain", "offset", "binX", "binY", "originX", "originY"})
        require(input.numeric.contains(key) && map.numeric.contains(key) && input.numeric.at(key) == map.numeric.at(key), "Defect map acquisition geometry differs or is unknown");
}
}
