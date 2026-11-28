#include "photonstack/SensorCalibration.hpp"
#include "photonstack/FileDigest.hpp"
#include "photonstack/ParallelRanges.hpp"
#include "photonstack/SensorQuality.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace photonstack {
namespace {
void require(bool ok, const std::string& why) {
    if (!ok)
        throw std::invalid_argument(why);
}
void settings(const SensorCalibrationOptions& o) {
    require(o.bias || o.dark, "Raw lights require a bias or dark master before flat correction");
    require(o.memoryBytes >= 4 * 1024 * 1024 && o.memoryBytes <= 8ULL * 1024 * 1024 * 1024 && o.tilePixels >= 1 &&
                o.tilePixels <= 1048576 && o.workers >= 1 && o.workers <= 32,
            "Invalid sensor calibration resource limit");
    require(std::isfinite(o.temperatureTolerance) && o.temperatureTolerance >= 0 && o.temperatureTolerance <= 10,
            "Invalid calibration temperature tolerance");
    require(!o.bias || o.bias->biasState == "included", "Bias master must include the bias pedestal");
    for (const auto* m : {&o.dark, &o.flat})
        if (*m)
            require((*m)->biasState == "included" || (*m)->biasState == "removed", "Declare dark and flat bias state");
    require(!o.dark || o.dark->biasState != "removed" || o.bias, "A bias-removed dark requires a bias master");
    require(!o.flat || o.flat->biasState != "included" || o.bias, "A bias-included flat requires a bias master");
}
template <class Lookup>
void metadata(const CalibrationAcquisition& input, const SensorCalibrationOptions& o, Lookup masterMetadata) {
    if (o.badPixels) validateSensorDefectMap(input, masterMetadata(o.badPixels->path));
    require(input.issues.empty() && calibrationFrameType(input) == "light" && input.channels == 1 &&
                input.text.contains("sampling") && input.text.contains("domain") && input.text.at("domain") == "linear",
            "Calibration requires a verified raw linear CFA or mono light");
    require(!input.text.contains("biasState") && !input.text.contains("normalization") &&
                !input.text.contains("calibration"),
            "Do not recalibrate a preprocessed light");
    require(input.numeric.contains("exposure") && input.numeric.at("exposure") > 0,
            "Light exposure must be known and positive");
    const std::array<std::pair<const std::optional<SensorCalibrationMaster>*, CalibrationMasterRole>, 3> masters{
        {{&o.bias, CalibrationMasterRole::Bias},
         {&o.dark, CalibrationMasterRole::Dark},
         {&o.flat, CalibrationMasterRole::Flat}}};
    for (const auto& [selected, role] : masters)
        if (*selected) {
            const auto& m = masterMetadata((*selected)->source.path);
            const auto match = matchCalibrationAcquisition(input, m, role, o.temperatureTolerance);
            if (match.status != "compatible") {
                std::string why = "Unverified calibration master:";
                for (const auto& e : match.evidence)
                    if (e.status == "missing" || e.status == "conflict" || e.status == "invalid")
                        why += " " + e.field + " (" + e.status + ")";
                throw std::invalid_argument(why);
            }
            require(!m.text.contains("biasState") || m.text.at("biasState") == (*selected)->biasState,
                    "Master preparation contradicts PSBIAS");
            if (m.text.contains("normalization") && m.text.at("normalization") != "none")
                require(role == CalibrationMasterRole::Flat && (*selected)->biasState == "removed",
                        "Only a bias-removed flat may carry normalization; bias and dark masters must retain physical "
                        "units");
            // Bias for an uncorrected flat must also match that flat's measured
            // temperature, not only the light's temperature.
            if (role == CalibrationMasterRole::Flat && (*selected)->biasState == "included") {
                const auto& bias = masterMetadata(o.bias->source.path);
                require(
                    matchCalibrationAcquisition(m, bias, CalibrationMasterRole::Bias, o.temperatureTolerance).status ==
                        "compatible",
                    "Bias master does not match the uncorrected flat");
            }
        }
}
} // namespace
void validateSensorCalibration(const std::filesystem::path& input, const SensorCalibrationOptions& o) {
    settings(o);
    metadata(inspectCalibrationAcquisition(input), o, inspectCalibrationAcquisition);
}
void validateSensorCalibration(const CalibrationAcquisition& light, const SensorCalibrationOptions& options,
                               const std::map<std::filesystem::path, CalibrationAcquisition>& masters) {
    settings(options);
    metadata(light, options, [&](const auto& path) -> const auto& { return masters.at(path); });
}
SensorCalibrationResult calibrateSensorFrame(SensorMasterSource input, const SensorCalibrationOptions& options) {
    settings(options);
    SensorCalibrationResult result;
    result.options = options;
    auto& o = result.options;
    const auto cancel = [&] {
        if (o.checkCancelled)
            o.checkCancelled();
    };
    std::set<std::filesystem::path> paths;
    std::set<std::string> hashes;
    const auto bind = [&](SensorMasterSource& s) {
        cancel();
        s.path = std::filesystem::canonical(s.path);
        require(std::filesystem::is_regular_file(s.path), "Calibration source must be a regular file");
        const auto sha = fileSHA256(s.path);
        require(s.sha.empty() || s.sha == sha, "Calibration source changed");
        s.sha = sha;
        require(paths.insert(s.path).second && hashes.insert(s.sha).second,
                "Duplicate calibration source path or content");
    };
    bind(input);
    if (o.badPixels) bind(*o.badPixels);
    result.input = input;
    for (auto* m : {&o.bias, &o.dark, &o.flat})
        if (*m)
            bind((*m)->source);
    SensorFitsReader light(input.path, true);
    std::map<std::filesystem::path, SensorFitsReader> masters;
    const auto openMaster = [&](const std::optional<SensorCalibrationMaster>& master) -> SensorFitsReader* {
        if (!master) return nullptr;
        const auto& path = master->source.path;
        return &masters.try_emplace(path, path, true).first->second;
    };
    auto* bias = openMaster(o.bias);
    auto* dark = openMaster(o.dark);
    auto* flat = openMaster(o.flat);
    auto* bad = o.badPixels ? &masters.try_emplace(o.badPixels->path, o.badPixels->path, true).first->second : nullptr;
    result.acquisition = light.metadata();
    metadata(result.acquisition, o, [&](const auto& path) -> const auto& { return masters.at(path).metadata(); });
    const auto& a = result.acquisition;
    const std::size_t count = std::size_t(a.width) * a.height;
    require(count <= o.memoryBytes / 8, "Calibration output planes exceed memory budget");
    const std::size_t fixed = count * 8 + (1 + masters.size()) * 65536 + o.workers * 4096 + 1048576;
    require(fixed < o.memoryBytes, "Calibration outputs exceed memory budget");
    const auto tileBytes = o.badPixels ? 20 : 16;
    const auto tile = std::min({o.tilePixels, count, (o.memoryBytes - fixed) / tileBytes});
    require(tile > 0, "No memory left for calibration tiles");
    result.tilePixels = tile;
    result.workingBytes = fixed + tile * tileBytes;
    result.workers = unsigned(
        std::max<std::size_t>(1, std::min<std::size_t>({o.workers, std::thread::hardware_concurrency(), tile / 1024})));
    const auto plane = [&] {
        ImageBuffer im;
        im.width = a.width;
        im.height = a.height;
        im.channels = 1;
        im.format = PixelFormat::Float32Gray;
        im.pixels.resize(count);
        return im;
    };
    result.calibrated = plane();
    result.valid = plane();
    result.biasOnLight = bool(bias) && (!o.dark || o.dark->biasState == "removed");
    result.biasOnFlat = o.flat && o.flat->biasState == "included";
    std::vector<float> l(tile), b(tile), d(tile), f(tile);
    std::vector<float> badValues(bad ? tile : 0);
    const bool cfa = a.text.at("sampling").starts_with("CFA:");
    const auto phase = [&](std::size_t p) { return cfa ? 2 * ((p / a.width) % 2) + p % a.width % 2 : 0; };
    if (flat) {
        std::array<double, 4> sums{};
        std::array<std::size_t, 4> totals{};
        for (std::size_t start = 0; start < count; start += tile) {
            cancel();
            const auto n = std::min(tile, count - start);
            flat->read(start, {f.data(), n});
            if (result.biasOnFlat)
                bias->read(start, {b.data(), n});
            for (std::size_t p = 0; p < n; ++p) {
                const double v = double(f[p]) - (result.biasOnFlat ? double(b[p]) : 0);
                if (std::isfinite(v) && v > 0) {
                    sums[phase(start + p)] += v;
                    ++totals[phase(start + p)];
                }
            }
        }
        for (unsigned p = 0; p < (cfa ? 4 : 1); ++p) {
            require(totals[p] && std::isfinite(sums[p]) && sums[p] > 0,
                    "Flat has no positive finite samples in a sensor phase");
            result.flatMeans[p] = sums[p] / double(totals[p]);
        }
    }
    detail::ScopedRangeWorkerLimit limit(result.workers);
    for (std::size_t start = 0; start < count; start += tile) {
        cancel();
        const auto n = std::min(tile, count - start);
        light.read(start, {l.data(), n});
        if (bad) {
            bad->read(start, {badValues.data(), n});
            require(std::all_of(badValues.begin(), badValues.begin() + n, [](float v) { return v == 0 || v == 1; }), "Defect map must contain only zero or one");
        }
        if (result.biasOnLight || result.biasOnFlat)
            bias->read(start, {b.data(), n});
        if (dark)
            dark->read(start, {d.data(), n});
        if (flat)
            flat->read(start, {f.data(), n});
        std::atomic_bool overflow{false};
        detail::parallelRanges(n, 1024, [&](std::size_t begin, std::size_t end) noexcept {
            for (std::size_t p = begin; p < end; ++p) {
                double v = double(l[p]) - (result.biasOnLight ? double(b[p]) : 0) - (dark ? double(d[p]) : 0);
                if (flat) {
                    const double response =
                        (double(f[p]) - (result.biasOnFlat ? double(b[p]) : 0)) / result.flatMeans[phase(start + p)];
                    v = std::isfinite(response) && response > 1e-6 ? v / response
                                                                   : std::numeric_limits<double>::quiet_NaN();
                }
                if (std::isfinite(v) && std::abs(v) > std::numeric_limits<float>::max())
                    overflow.store(true, std::memory_order_relaxed);
                if (bad && badValues[p] == 1) v = std::numeric_limits<double>::quiet_NaN();
                const bool valid = std::isfinite(v);
                result.calibrated.pixels[start + p] = valid ? float(v) : std::numeric_limits<float>::quiet_NaN();
                result.valid.pixels[start + p] = valid ? 1 : 0;
            }
        });
        require(!overflow.load(), "Calibrated sensor value exceeds Float32 range");
    }
    for (float value : result.calibrated.pixels) {
        if (!std::isfinite(value))
            ++result.invalidPixels;
        else if (value < 0)
            ++result.negativePixels;
    }
    require(result.invalidPixels < count, "No valid calibrated light pixels");
    const auto unchanged = [&](const SensorMasterSource& s, SensorFitsReader& reader) {
        cancel();
        require(reader.sourceSHA256() == s.sha, "Calibration source changed during computation");
    };
    unchanged(result.input, light);
    if (bad) unchanged(*o.badPixels, *bad);
    for (const auto* m : {&o.bias, &o.dark, &o.flat})
        if (*m)
            unchanged((*m)->source, masters.at((*m)->source.path));
    return result;
}
} // namespace photonstack
