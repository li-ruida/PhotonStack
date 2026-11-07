#include "DeepSkyPreflight.hpp"
#include "photonstack/ImageInspector.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace photonstack;
constexpr std::uint64_t mib = 1024 * 1024;
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) throw std::overflow_error("Resource estimate exceeds supported size");
    return a + b;
}
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (b && a > std::numeric_limits<std::uint64_t>::max() / b) throw std::overflow_error("Resource estimate exceeds supported size");
    return a * b;
}
std::string quoted(const std::string& s) {
    std::ostringstream out; out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}
void number(std::ostream& out, const std::optional<std::uint64_t>& value) {
    if (value) out << *value; else out << "null";
}
}

DeepSkyPreflight inspectDeepSkyPlan(const photonstack::DeepSkyRecipe& recipe, const std::filesystem::path& destination) {
    DeepSkyPreflight result;
    result.lightCount = recipe.inputs.size();
    const auto& o = recipe.options;
    result.calibrationCount = o.darks.size() + o.biases.size() + o.flats.size();
    result.destination = fs::absolute(destination).lexically_normal().string();
    result.additionalSensorStorage = o.sensorPattern;
    auto existing = fs::path(result.destination);
    std::error_code error;
    // Resolve the filesystem without creating a run directory. Existing,
    // nonempty work roots are valid destinations for this planning command.
    while (!existing.empty() && !fs::exists(existing, error)) {
        const auto parent = existing.parent_path();
        if (error || parent == existing) break;
        existing = parent;
    }
    if (!error && fs::is_directory(existing, error)) {
        const auto space = fs::space(existing, error);
        if (!error) result.availableBytes = space.available;
    }
    if (!result.availableBytes) result.issues.push_back({"DestinationUnavailable", result.destination, "Could not inspect the destination filesystem"});

    bool complete = !recipe.inputs.empty();
    if (!complete) result.issues.push_back({"InputMissing", "", "No light frames supplied"});
    std::uint64_t largestFrame = 0;
    std::set<fs::path> identities;
    const auto inspect = [&](const std::vector<fs::path>& paths, bool lights) {
        for (const auto& path : paths) {
            std::error_code ec;
            if (!fs::is_regular_file(path, ec) || ec) {
                complete = false;
                result.issues.push_back({"InputUnavailable", path.string(), "Input is not a readable regular file"});
                continue;
            }
            const auto canonical = fs::canonical(path, ec);
            if (ec || !identities.insert(canonical).second) {
                complete = false;
                result.issues.push_back({"DuplicateInput", path.string(), "An input path is repeated across light or calibration groups"});
                continue;
            }
            const auto bytes = fs::file_size(path, ec);
            if (ec) { complete = false; result.issues.push_back({"InputUnavailable", path.string(), ec.message()}); continue; }
            result.sourceBytes = add(result.sourceBytes, bytes);
            const auto inspected = ImageInspector().inspect(path);
            if (!inspected.ok || inspected.metadata.width == 0 || inspected.metadata.height == 0) {
                complete = false;
                result.issues.push_back({"HeaderUnavailable", path.string(), inspected.message});
                continue;
            }
            ++result.inspectedCount;
            const auto& m = inspected.metadata;
            // Scientific intermediates are float RGBA even for a mono/CFA source.
            largestFrame = std::max(largestFrame, multiply(multiply(m.width, m.height), 16));
            if (lights && !result.width) { result.width = m.width; result.height = m.height; }
            if (result.width && (m.width != result.width || m.height != result.height)) {
                complete = false;
                result.issues.push_back({"GeometryMismatch", path.string(), "Header dimensions differ; review geometry before processing"});
            }
        }
    };
    try {
        inspect(recipe.inputs, true);
        inspect(o.darks, false); inspect(o.biases, false); inspect(o.flats, false);
        if (!complete) return result;
        const auto n = std::uint64_t(recipe.inputs.size());
        const auto frame = largestFrame;
        const auto fitsFrame = add(multiply((add(frame, 2879) / 2880), 2880), 5760);
        const auto calibrationGroups = std::uint64_t(!o.darks.empty()) + !o.biases.empty() + !o.flats.empty();
        const auto calibrationFrames = std::max({o.darks.size(), o.biases.size(), o.flats.size()});
        const bool robust = o.stack.method == StackMethod::Median || o.stack.method == StackMethod::SigmaClip ||
            o.stack.method == StackMethod::WinsorizedSigmaClip || o.stack.method == StackMethod::PercentileClip;
        const auto calibrationCache = o.masterMethod == MasterFrameMethod::Median ? multiply(calibrationFrames, frame) : 0;
        // Each consumed aligned FITS is released after caching. At most one
        // additional decoded frame is on disk during conversion to f32 cache.
        result.temporaryBytes = std::max(multiply(add(n, 1), fitsFrame), calibrationCache);
        const bool noise = o.noiseModel == DeepSkyNoiseModel::IndependentLuminance;
        const auto rejectionMaps = o.stack.collectRejectionMaps ? 2ULL : 0ULL;
        std::uint64_t savedStages = 0;
        if (o.saveDenoiseStages) {
            savedStages = (o.denoiseH != 0) + (o.multiscale.luminance > 0 || o.multiscale.chroma > 0) + (o.backgroundDenoiseStrength > 0);
        }
        // Use uncropped dimensions and uncompressed TIFF storage. Includes
        // calibration masters and explicitly requested diagnostic products.
        const auto fitsProducts = add(add(2 + rejectionMaps + noise, calibrationGroups), savedStages);
        result.productBytes = add(multiply(fitsProducts, fitsFrame), add(frame / 2, mib));
        const auto combined = add(*result.temporaryBytes, *result.productBytes);
        result.plannedDiskBytes = add(add(combined, combined / 10), 64 * mib);
        // These are the main stack buffers, not a process RSS or a memory cap.
        // Image decoding, registration, background fitting and denoising have
        // additional algorithm-specific working sets.
        const auto stackFrames = calibrationGroups + (robust ? 1 : 3) + rejectionMaps + noise;
        result.stackBufferBytes = add(multiply(stackFrames, frame), robust ? add(o.stack.combineTileBytes, 64 * mib) : 0);
        const auto g = multiply(std::min(o.quality.sampleSide, result.width), std::min(o.quality.sampleSide, result.height));
        result.assessmentSampleBytes = multiply(multiply(n, g), 8);
    } catch (const std::overflow_error& e) {
        result.temporaryBytes.reset(); result.productBytes.reset(); result.plannedDiskBytes.reset();
        result.stackBufferBytes.reset(); result.assessmentSampleBytes.reset();
        result.issues.push_back({"EstimateOverflow", "", e.what()});
    }
    if (o.sensorPattern) {
        // Cross-fitted sensor estimation has its own caches. Show the normal
        // pipeline estimate, but never represent it as the total requirement.
        result.plannedDiskBytes.reset();
        result.issues.push_back({"SensorStorageUnestimated", "", "Sensor pattern correction requires additional working storage"});
    }
    return result;
}

std::string DeepSkyPreflight::json() const {
    std::ostringstream out;
    out << "{\"type\":\"complete\",\"command\":\"deep-sky preflight\",\"version\":1"
        << ",\"lightCount\":" << lightCount << ",\"calibrationCount\":" << calibrationCount
        << ",\"inspectedCount\":" << inspectedCount << ",\"width\":" << width << ",\"height\":" << height
        << ",\"sourceBytes\":" << sourceBytes << ",\"destination\":" << quoted(destination)
        << ",\"additionalSensorStorage\":" << (additionalSensorStorage ? "true" : "false");
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::optional<std::uint64_t>>>{
        {"temporaryBytes", temporaryBytes}, {"productBytes", productBytes}, {"plannedDiskBytes", plannedDiskBytes},
        {"stackBufferBytes", stackBufferBytes}, {"assessmentSampleBytes", assessmentSampleBytes}, {"availableBytes", availableBytes}}) {
        out << ',' << quoted(name) << ':'; number(out, value);
    }
    out << ",\"issues\":[";
    for (std::size_t i = 0; i < issues.size(); ++i) {
        if (i) out << ',';
        out << "{\"code\":" << quoted(issues[i].code) << ",\"path\":" << quoted(issues[i].path) << ",\"message\":" << quoted(issues[i].message) << '}';
    }
    out << "]}\n"; return out.str();
}
