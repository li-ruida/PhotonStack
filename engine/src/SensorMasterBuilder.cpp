#include "photonstack/SensorMasterBuilder.hpp"
#include "photonstack/FileDigest.hpp"
#include "photonstack/ParallelRanges.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace photonstack {
namespace {
constexpr float missing = std::numeric_limits<float>::quiet_NaN();
void require(bool ok, const std::string& why) {
    if (!ok)
        throw std::invalid_argument(why);
}
CalibrationMasterRole role(const std::string& s) {
    if (s == "bias")
        return CalibrationMasterRole::Bias;
    if (s == "dark")
        return CalibrationMasterRole::Dark;
    if (s == "dark-flat")
        return CalibrationMasterRole::DarkFlat;
    if (s == "flat")
        return CalibrationMasterRole::Flat;
    throw std::invalid_argument("Unknown sensor master role");
}
void compatible(CalibrationAcquisition a, const CalibrationAcquisition& b, CalibrationMasterRole r, double tolerance) {
    // A raw calibration frame is the input here, not a light frame. Only the
    // matching function's input-role constraint is adapted, never its metadata.
    a.text["frameType"] = r == CalibrationMasterRole::DarkFlat ? "Flat" : "Light";
    const auto match = matchCalibrationAcquisition(a, b, r, tolerance);
    if (match.status != "compatible") {
        std::string why = "Sensor acquisition is not verified:";
        for (const auto& e : match.evidence)
            if (e.status == "missing" || e.status == "conflict" || e.status == "invalid")
                why += " " + e.field + " (" + e.status + ")";
        throw std::invalid_argument(why);
    }
}
float finiteFloat(double value) {
    if (!std::isfinite(value))
        return missing;
    if (std::abs(value) > std::numeric_limits<float>::max())
        throw std::invalid_argument("Corrected sensor value exceeds Float32 range");
    return static_cast<float>(value);
}
} // namespace
SensorMasterResult buildSensorMaster(std::vector<SensorMasterSource> sources, const SensorMasterOptions& o) {
    const auto masterRole = role(o.role);
    require(sources.size() >= 2 && sources.size() <= 256, "Build a master from 2..256 distinct raw frames");
    require(o.method == "mean" || o.method == "median", "Sensor master method must be mean or median");
    require(o.memoryBytes >= 4 * 1024 * 1024 && o.memoryBytes <= 8ULL * 1024 * 1024 * 1024 && o.tilePixels >= 1 &&
                o.tilePixels <= 1048576,
            "Invalid sensor master memory or tile limit");
    require(o.workers >= 1 && o.workers <= 32 && o.minimumSamples >= 2 && o.minimumSamples <= sources.size(),
            "Invalid worker or minimum sample count");
    require(std::isfinite(o.temperatureTolerance) && o.temperatureTolerance >= 0 && o.temperatureTolerance <= 10,
            "Invalid temperature tolerance");
    require(o.role != "bias" || (!o.bias && !o.darkFlat), "Bias masters must be built from uncorrected bias frames");
    require(!o.darkFlat || o.role == "flat", "Dark-flat subtraction is only used when constructing a flat");
    require(o.role != "flat" || o.bias || o.darkFlat, "Flat construction requires bias or dark-flat correction");
    require(!o.darkFlat || o.darkFlatBiasState == "included" || o.darkFlatBiasState == "removed",
            "Declare dark-flat bias state");
    require(!o.darkFlat || (o.darkFlatBiasState == "removed") == bool(o.bias),
            "A bias-removed dark-flat requires bias; a bias-included dark-flat must not subtract bias again");
    require(o.darkFlat || o.darkFlatBiasState.empty(), "Dark-flat bias state requires a dark-flat source");
    const auto cancel = [&] {
        if (o.checkCancelled)
            o.checkCancelled();
    };
    std::set<std::filesystem::path> paths;
    std::set<std::string> hashes;
    auto snapshot = [&](SensorMasterSource source) {
        cancel();
        source.path = std::filesystem::canonical(source.path);
        require(std::filesystem::is_regular_file(source.path), "Sensor source must be a regular file");
        const auto sha = fileSHA256(source.path);
        require(source.sha.empty() || source.sha == sha, "Sensor source SHA-256 changed");
        source.sha = sha;
        require(paths.insert(source.path).second && hashes.insert(sha).second,
                "Duplicate sensor source path or content");
        return source;
    };
    for (auto& s : sources)
        s = snapshot(s);
    std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    std::optional<SensorMasterSource> bias, darkFlat;
    if (o.bias)
        bias = snapshot(*o.bias);
    if (o.darkFlat)
        darkFlat = snapshot(*o.darkFlat);
    std::vector<SensorFitsReader> readers;
    for (const auto& s : sources)
        readers.emplace_back(s.path);
    std::unique_ptr<SensorFitsReader> biasReader, darkReader;
    if (bias)
        biasReader = std::make_unique<SensorFitsReader>(bias->path);
    if (darkFlat)
        darkReader = std::make_unique<SensorFitsReader>(darkFlat->path);
    SensorMasterResult result;
    result.sources = sources;
    result.role = o.role;
    result.method = o.method;
    result.biasState = (o.bias || o.role == "flat") ? "removed" : "included";
    result.acquisition = readers.front().metadata();
    auto& a = result.acquisition;
    double minTemperature = std::numeric_limits<double>::infinity(), maxTemperature = -minTemperature;
    for (const auto& reader : readers) {
        const auto& b = reader.metadata();
        const auto type = calibrationFrameType(b);
        require(type == o.role || (o.role == "dark-flat" && type == "dark"),
                "Raw frame IMAGETYP contradicts the requested master role");
        auto label = b.text.at("frameType");
        std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return std::toupper(c); });
        require(!label.starts_with("MASTER") && !b.text.contains("biasState") && !b.text.contains("normalization") &&
                    !b.numeric.contains("sensorTemperatureMin") && !b.numeric.contains("sensorTemperatureMax"),
                "Input must be raw calibration exposures, not preprocessed masters");
        compatible(a, b, masterRole, o.temperatureTolerance);
        require(a.numeric.contains("exposure") && b.numeric.contains("exposure") &&
                    std::abs(a.numeric.at("exposure") - b.numeric.at("exposure")) <= 1e-6,
                "All master input exposure times must match");
        if (o.role == "flat")
            require(b.numeric.at("exposure") > 0, "Flat exposures must be positive");
        if (b.numeric.contains("sensorTemperature")) {
            minTemperature = std::min(minTemperature, b.numeric.at("sensorTemperature"));
            maxTemperature = std::max(maxTemperature, b.numeric.at("sensorTemperature"));
        }
        if (biasReader)
            compatible(b, biasReader->metadata(), CalibrationMasterRole::Bias, o.temperatureTolerance);
        if (darkReader)
            compatible(b, darkReader->metadata(), CalibrationMasterRole::DarkFlat, o.temperatureTolerance);
    }
    if (o.role != "flat")
        require(maxTemperature - minTemperature <= o.temperatureTolerance,
                "Master temperature spread exceeds the tolerance");
    for (const auto* reader : {biasReader.get(), darkReader.get()})
        if (reader && reader->metadata().text.contains("biasState"))
            require(reader->metadata().text.at("biasState") ==
                        (reader == biasReader.get() ? "included" : o.darkFlatBiasState),
                    "Declared preparation contradicts the master's PSBIAS header");
    if (std::isfinite(minTemperature)) {
        a.numeric["sensorTemperatureMin"] = minTemperature;
        a.numeric["sensorTemperatureMax"] = maxTemperature;
        a.numeric["sensorTemperature"] = (minTemperature + maxTemperature) / 2;
    }
    // A master does not inherit the first exposure's target, date or setpoint.
    for (const auto* k : {"object", "observedAt", "timeSystem"})
        a.text.erase(k);
    a.numeric.erase("setTemperature");
    a.numeric["scale"] = 1;
    a.numeric["zero"] = 0;
    a.bitpix = -32;
    if (o.role != "flat") {
        a.text.erase("filter");
        a.text.erase("opticalTrain");
        a.numeric.erase("focalLength");
    }
    const std::size_t count = std::size_t(a.width) * a.height;
    // Two full output planes; all source and correction pixels are tiled. The
    // budget includes reader buffers and worker scratch but not OS page cache.
    require(count <= o.memoryBytes / 8, "Sensor outputs exceed memory budget");
    const std::size_t fixed = count * 8 + (readers.size() + 2) * 65536 + o.workers * 4096 + 1048576;
    require(fixed < o.memoryBytes, "Sensor outputs exceed memory budget; raise --memory-mib");
    const auto tile = std::min({o.tilePixels, count, (o.memoryBytes - fixed) / (4 * (readers.size() + 2))});
    require(tile > 0, "No memory left for sensor tiles");
    result.tilePixels = tile;
    result.workingBytes = fixed + tile * 4 * (readers.size() + 2);
    result.workers =
        std::max<std::size_t>(1, std::min<std::size_t>({o.workers, std::thread::hardware_concurrency(), tile / 1024}));
    auto plane = [&] {
        ImageBuffer im;
        im.width = a.width;
        im.height = a.height;
        im.channels = 1;
        im.format = PixelFormat::Float32Gray;
        im.pixels.resize(count);
        return im;
    };
    result.master = plane();
    result.accepted = plane();
    std::vector<float> samples(readers.size() * tile), biasTile(tile), darkTile(tile);
    const bool cfa = a.text.at("sampling").starts_with("CFA:");
    auto phase = [&](std::size_t p) { return cfa ? 2 * ((p / a.width) % 2) + p % a.width % 2 : 0; };
    auto corrections = [&](std::size_t start, std::size_t n) {
        cancel();
        if (biasReader)
            biasReader->read(start, {biasTile.data(), n});
        if (darkReader)
            darkReader->read(start, {darkTile.data(), n});
    };
    auto corrected = [&](float value, std::size_t p) {
        const double v =
            double(value) - (biasReader ? double(biasTile[p]) : 0) - (darkReader ? double(darkTile[p]) : 0);
        return o.role == "flat" && v <= 0 ? missing : finiteFloat(v);
    };
    result.normalization.resize(readers.size(), {1, 1, 1, 1});
    if (o.role == "flat") {
        std::vector<std::array<double, 4>> sums(readers.size());
        std::vector<std::array<std::size_t, 4>> totals(readers.size());
        for (std::size_t start = 0; start < count; start += tile) {
            const auto n = std::min(tile, count - start);
            corrections(start, n);
            for (std::size_t f = 0; f < readers.size(); ++f) {
                readers[f].read(start, {samples.data(), n});
                for (std::size_t p = 0; p < n; ++p) {
                    const double v = corrected(samples[p], p);
                    const auto q = phase(start + p);
                    if (std::isfinite(v)) {
                        sums[f][q] += v;
                        ++totals[f][q];
                    }
                }
            }
        }
        for (std::size_t f = 0; f < readers.size(); ++f)
            for (unsigned q = 0; q < (cfa ? 4 : 1); ++q) {
                require(totals[f][q] > 0 && std::isfinite(sums[f][q]) && sums[f][q] > 0,
                        "Flat phase has no valid positive corrected signal");
                sums[f][q] /= double(totals[f][q]);
            }
        for (std::size_t f = 0; f < readers.size(); ++f)
            for (unsigned q = 0; q < (cfa ? 4 : 1); ++q)
                result.normalization[f][q] = sums[0][q] / sums[f][q];
    }
    detail::ScopedRangeWorkerLimit limit(result.workers);
    for (std::size_t start = 0; start < count; start += tile) {
        const auto n = std::min(tile, count - start);
        corrections(start, n);
        for (std::size_t f = 0; f < readers.size(); ++f) {
            auto* row = samples.data() + f * tile;
            readers[f].read(start, {row, n});
            for (std::size_t p = 0; p < n; ++p)
                row[p] = finiteFloat(double(corrected(row[p], p)) * result.normalization[f][phase(start + p)]);
        }
        detail::parallelRanges(n, 1024, [&](std::size_t begin, std::size_t end) noexcept {
            std::array<float, 256> values{};
            for (std::size_t p = begin; p < end; ++p) {
                unsigned valid = 0;
                double sum = 0;
                for (std::size_t f = 0; f < readers.size(); ++f) {
                    const float v = samples[f * tile + p];
                    if (std::isfinite(v)) {
                        values[valid++] = v;
                        sum += v;
                    }
                }
                result.accepted.pixels[start + p] = float(valid);
                float value = missing;
                if (valid >= o.minimumSamples) {
                    if (o.method == "mean")
                        value = float(sum / valid);
                    else {
                        auto mid = values.begin() + valid / 2;
                        std::nth_element(values.begin(), mid, values.begin() + valid);
                        value = valid % 2 ? *mid : float((double(*mid) + *std::max_element(values.begin(), mid)) / 2);
                    }
                }
                result.master.pixels[start + p] = value;
            }
        });
    }
    for (std::size_t p = 0; p < count; ++p) {
        if (!std::isfinite(result.master.pixels[p]))
            ++result.invalidPixels;
        result.missingSamples += sources.size() - std::size_t(result.accepted.pixels[p]);
    }
    require(result.invalidPixels < count, "No sensor pixel reaches the required valid sample count");
    auto unchanged = [&](const SensorMasterSource& s) {
        cancel();
        require(fileSHA256(s.path) == s.sha, "Sensor source changed during construction");
    };
    for (const auto& s : sources)
        unchanged(s);
    if (bias)
        unchanged(*bias);
    if (darkFlat)
        unchanged(*darkFlat);
    return result;
}
} // namespace photonstack
