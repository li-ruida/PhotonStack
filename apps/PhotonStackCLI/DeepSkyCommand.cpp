#include "DeepSkyCommand.hpp"
#include "photonstack/StageProfile.hpp"
#include "DeepSkyPreflight.hpp"
#include "photonstack/DeepSkyRecipe.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <thread>
#include <filesystem>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace photonstack;
std::string json(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
            else out << c;
        }
    }
    out << '"'; return out.str();
}
const char* boolean(bool value) { return value ? "true" : "false"; }
std::size_t workerCount(const std::string& value, std::size_t maximum, const std::string& option) {
    std::size_t consumed = 0;
    unsigned long long parsed = 0;
    try { parsed = std::stoull(value, &consumed); }
    catch (...) { throw std::invalid_argument(option + " expects an integer from 0 to " + std::to_string(maximum)); }
    if (consumed != value.size() || parsed > maximum)
        throw std::invalid_argument(option + " expects an integer from 0 to " + std::to_string(maximum));
    return std::size_t(parsed);
}
bool onOff(const std::string& value, const std::string& option) {
    if (value == "on") return true;
    if (value == "off") return false;
    throw std::invalid_argument(option + " expects on or off");
}
void help() {
    std::cout << R"(PhotonStack deep-sky: analyze, select, stack and finish light frames.

Usage:
  photonstack deep-sky --input <light-directory> --output-dir <new-directory> [--analyze-only]
  photonstack deep-sky --recipe <recipe.txt> --output-dir <new-directory> [--analyze-only]
                       [--analysis-workers 0..8] [--stack-workers 0..16]
                       [--gpu-acceleration on|off]
  photonstack deep-sky --recipe <recipe.txt> --output-dir <work-directory> --preflight
  photonstack deep-sky --recipe <recipe.txt> --from-master <linear-RGB.fits> --output-dir <new-directory>
  photonstack deep-sky --write-default-recipe <new-recipe.txt> [--input <light-directory>]
  photonstack deep-sky --inspect-recipe <recipe.txt>
  photonstack deep-sky schema

Options:
  --input                 Read supported light files directly inside a directory.
  --recipe                Load processing settings and optional input paths.
  --output-dir            Write results to a new or empty directory.
  --analyze-only          Measure and select frames without stacking or finishing.
  --analysis-workers      Concurrent frame analysis; 0 is memory-aware automatic (default).
  --stack-workers         Robust stack-combine workers; 0 is automatic (default).
  --gpu-acceleration      Select Metal or CPU when no compute backend is specified.
  --compute-backend       auto, cpu, or metal for calibration, star analysis, resampling and Winsorized stacking.
  --preflight             Inspect headers and estimate resources; writes no outputs.
  --from-master           Refinish an existing linear RGB FITS using a recipe;
                          cannot be combined with --input or --analyze-only.
  --write-default-recipe  Save editable defaults without running processing.
  --inspect-recipe        Validate a recipe and print its normalized JSON form.
  schema                  Print supported settings, defaults and limits as JSON.
  --help, -h, help        Show this help without reading or writing images.

Start here:
  photonstack deep-sky --write-default-recipe recipe.txt --input lights
  photonstack deep-sky --recipe recipe.txt --output-dir analysis --analyze-only
  photonstack deep-sky --recipe recipe.txt --output-dir stack

Input files stay unchanged. Review the analysis before stacking and inspect the
finished image at native scale. Recipe details: docs/deep-sky-recipe.md.
)";
}
void strings(std::ostream& out, const std::vector<std::string>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << json(values[i]); }
    out << ']';
}
void schema() {
    std::cout << "{\"type\":\"complete\",\"command\":\"deep-sky schema\",\"version\":1,\"fields\":[";
    bool first = true;
    for (const auto& f : DeepSkyRecipe::schema()) {
        if (!first) std::cout << ','; first = false;
        std::cout << "{\"key\":" << json(f.key) << ",\"label\":" << json(f.label) << ",\"chineseLabel\":" << json(f.chineseLabel)
            << ",\"group\":" << json(f.group) << ",\"type\":" << json(f.type) << ",\"defaultValue\":" << json(f.defaultValue)
            << ",\"minimum\":" << f.minimum << ",\"maximum\":" << f.maximum << ",\"choices\":";
        strings(std::cout, f.choices); std::cout << '}';
    }
    std::cout << "]}\n";
}
std::string report(const DeepSkyWorkflowResult& r, const DeepSkyRecipe& recipe, bool analysisOnly) {
    std::ostringstream out; out << std::setprecision(10);
    out << "{\"type\":" << json(r.ok ? "complete" : "error") << ",\"command\":\"deep-sky\",\"version\":1,\"success\":" << boolean(r.ok)
        << ",\"masterReused\":" << boolean(r.masterReused) << ",\"analysisOnly\":" << boolean(analysisOnly) << ",\"inputCount\":" << recipe.inputs.size()
        << ",\"selectedCount\":" << r.selectedCount << ",\"referenceIndex\":" << r.referenceIndex
        << ",\"recipe\":" << json(recipe.serialize()) << ",\"errorCode\":" << json(r.errorCode) << ",\"message\":" << json(r.message)
        << ",\"master\":" << json(r.master.string()) << ",\"backgroundMaster\":" << json(r.backgroundMaster.string())
        << ",\"developed\":" << json(r.developed.string()) << ",\"rejectionLow\":" << json(r.rejectionLow.string())
        << ",\"rejectionHigh\":" << json(r.rejectionHigh.string())
        << ",\"sensorPatternReport\":" << json(r.sensorPatternReport.string())
        << ",\"noiseReference\":" << json(r.noiseReference.string())
        << ",\"noiseReferenceManifest\":" << json(r.noiseReferenceManifest.string())
        << ",\"noiseReferenceInputs\":" << json(r.noiseReferenceInputs.string())
        << ",\"independentLuminanceNoise\":" << boolean(r.independentLuminanceNoise)
        << ",\"denoiseStretchLocked\":" << boolean(r.ok && !analysisOnly && r.denoiseStretchLocked)
        << ",\"crop\":{\"x\":" << r.cropX << ",\"y\":" << r.cropY << ",\"width\":" << r.cropWidth << ",\"height\":" << r.cropHeight << '}'
        << ",\"denoiseH\":" << r.denoiseH << ",\"development\":{\"toneScale\":" << r.development.toneScale
        << ",\"colorCleanedStars\":" << r.development.colorCleanedStars
        << ",\"sky\":[" << r.development.sky[0] << ',' << r.development.sky[1] << ',' << r.development.sky[2] << ']'
        << ",\"shadowNoise\":" << r.development.shadowNoise << ",\"whitePoint\":" << r.development.whitePoint << ",\"calibrationStars\":" << r.development.calibrationStars << ",\"gains\":["
        << r.development.gains[0] << ',' << r.development.gains[1] << ',' << r.development.gains[2] << "]},\"channelAlignment\":[";
    for (unsigned c=0;c<3;++c) {
        if(c)out << ',';
        out << "{\"applied\":" << boolean(r.channelAlignment.applied[c])
            << ",\"dx\":" << r.channelAlignment.dx[c] << ",\"dy\":" << r.channelAlignment.dy[c]
            << ",\"scatter\":" << r.channelAlignment.scatter[c] << ",\"stars\":" << r.channelAlignment.stars[c] << '}';
    }
    out << "],\"displayReview\":";
    if (r.ok && !analysisOnly && r.development.displayPixels > 0) {
        out << "{\"version\":1,\"pixels\":" << r.development.displayPixels
            << ",\"nearBlackPixels\":" << r.development.nearBlackPixels
            << ",\"nearWhitePixels\":" << r.development.nearWhitePixels << '}';
    } else out << "null";
    out << ",\"frames\":[";
    for (std::size_t i = 0; i < r.frames.size(); ++i) {
        const auto& f = r.frames[i]; const auto& a = f.assessment;
        if (i) out << ',';
        out << "{\"index\":" << i << ",\"input\":" << json(f.input.string()) << ",\"bytes\":" << f.bytes
            << ",\"sha256\":" << json(f.sha256) << ",\"sensorPatternModel\":" << f.sensorPatternModel
            << ",\"modifiedTicks\":" << f.modifiedTicks << ",\"reference\":" << boolean(f.reference)
            << ",\"usable\":" << boolean(a.usable) << ",\"selected\":" << boolean(a.selected)
            << ",\"recommendedKeep\":" << boolean(a.recommendedKeep) << ",\"temporalAvailable\":" << boolean(a.temporalAvailable)
            << ",\"sky\":" << f.native.sky << ",\"noise\":" << f.native.noise << ",\"noiseRatio\":" << a.noiseRatio
            << ",\"coverage\":" << f.alignedCoverage << ",\"starCount\":" << f.native.starCount
            << ",\"starsMeasured\":" << boolean(f.native.starsMeasured)
            << ",\"starCountEstimator\":\"deduplicated-detection-v2\""
            << ",\"starShapeCount\":" << f.native.starShapeCount << ",\"fwhmEstimator\":\"native-gaussian-v1\""
            << ",\"fwhm\":" << f.native.medianFwhm
            << ",\"eccentricity\":" << f.native.medianEccentricity << ",\"matches\":" << f.registration.matches
            << ",\"inlierRatio\":" << f.registration.inlierRatio << ",\"registrationFallback\":" << boolean(f.registration.usedFallback)
            << ",\"centroidRefined\":" << boolean(f.registration.usedCentroidRefinement)
            << ",\"centroidRms\":" << f.registration.centroidRefinementRms
            << ",\"registrationTransform\":{\"scale\":" << f.registration.transform.scale
            << ",\"rotationRadians\":" << f.registration.transform.rotationRadians
            << ",\"dx\":" << f.registration.transform.dx << ",\"dy\":" << f.registration.transform.dy << '}'
            << ",\"positiveFraction\":" << a.positiveFraction << ",\"negativeFraction\":" << a.negativeFraction
            << ",\"residualRms\":" << a.residualRms << ",\"comparedSamples\":" << a.comparedSamples
            << ",\"rejectedLowSamples\":" << f.rejectedLowSamples << ",\"rejectedHighSamples\":" << f.rejectedHighSamples
            << ",\"stackComparedSamples\":" << f.comparedSamples << ",\"errorCode\":" << json(f.errorCode) << ",\"message\":" << json(f.message)
            << ",\"reasons\":"; strings(out, a.reasons); out << ",\"warnings\":"; strings(out, a.warnings);
        out << ",\"trails\":[";
        for (std::size_t t = 0; t < a.trails.size(); ++t) {
            if (t) out << ',';
            const auto& line = a.trails[t];
            out << "{\"sign\":" << line.sign << ",\"samples\":" << line.samples
                << ",\"x1\":" << line.x1 << ",\"y1\":" << line.y1 << ",\"x2\":" << line.x2 << ",\"y2\":" << line.y2 << '}';
        }
        out << "]}";
    }
    out << "]}\n"; return out.str();
}
std::uint64_t peakResidentBytes() {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#ifdef __APPLE__
        return usage.ru_maxrss;
#else
        return std::uint64_t(usage.ru_maxrss) * 1024;
#endif
    }
#endif
    return 0;
}
std::string performanceReport(const detail::StageProfile& profile, const DeepSkyRecipe& recipe,
                              const DeepSkyWorkflowResult& result, double elapsed, double processCPU) {
    std::ostringstream out;
    out << std::setprecision(10) << R"({"version":1,"success":)" << boolean(result.ok)
        << R"(,"wallSeconds":)" << elapsed << R"(,"cpuSeconds":)" << processCPU
        << R"(,"peakResidentBytes":)" << peakResidentBytes()
        << R"(,"cpuPercent":)" << (elapsed > 0 ? 100 * processCPU / elapsed : 0)
        << R"(,"requestedBackend":)" << json(recipe.options.computeBackend == ComputeBackend::Metal ? "metal" :
            recipe.options.computeBackend == ComputeBackend::CPU ? "cpu" : "auto")
        << R"(,"inputCount":)" << recipe.inputs.size() << R"(,"selectedCount":)" << result.selectedCount
        << R"(,"requestedAnalysisWorkers":)" << recipe.options.analysisWorkers
        << R"(,"requestedStackWorkers":)" << recipe.options.stack.combineWorkers
        << R"(,"peakAnalysisFrames":)" << profile.analysisPeak
        << R"(,"admissionReason":)" << json(profile.admissionReason)
        << R"(,"stages":[)";
    for (unsigned i = 0; i < profile.stages.size(); ++i) {
        const auto& stage = profile.stages[i];
        if (i) out << ',';
        out << R"({"name":)" << json(detail::stageNames[i]) << R"(,"executed":)" << boolean(stage.visits > 0)
            << R"(,"visits":)" << stage.visits << R"(,"wallSeconds":)" << stage.wall << R"(,"threadCPUSeconds":)" << stage.cpu
            << R"(,"peakWorkers":)" << stage.peakWorkers << R"(,"peakResidentBytesSampled":)" << stage.peakResidentBytes
            << R"(,"backend":)" << json(stage.visits ? stage.backend : "none")
            << R"(,"fallback":)" << json(stage.fallback) << R"(,"gpuSeconds":)";
        if (i == unsigned(detail::Stage::RawDecode) && stage.backendReported && std::string_view(stage.backend) != "cpu")
            out << "null"; // Core Image does not expose its command-buffer timestamps.
        else out << stage.gpu;
        out << R"(,"metalOperations":{)";
        bool first = true;
        for (const auto& [operation, count] : stage.metalOperations) {
            if (!first) out << ',';
            first = false;
            out << json(operation) << ':' << count;
        }
        out << '}';
        out << '}';
    }
    out << "]}\n";
    return out.str();
}
void saveNew(const fs::path& path, const std::string& text) {
    if (fs::exists(path)) throw std::invalid_argument("Refusing to replace existing file: " + path.string());
    std::ofstream file(path, std::ios::binary);
    file << text; file.close();
    if (!file) throw std::runtime_error("Cannot save " + path.string());
}
std::vector<fs::path> directoryInputs(const fs::path& path) {
    const std::set<std::string> extensions{".fit", ".fits", ".fts", ".tif", ".tiff", ".nef", ".arw", ".cr2", ".cr3", ".dng", ".orf", ".raf", ".rw2", ".srw"};
    std::vector<fs::path> result;
    if (!fs::is_directory(path)) throw std::invalid_argument("--input requires a directory");
    for (const auto& entry : fs::directory_iterator(path)) {
        if (!entry.is_regular_file()) continue;
        auto extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (extensions.contains(extension)) result.push_back(fs::absolute(entry.path()).lexically_normal());
    }
    std::sort(result.begin(), result.end()); return result;
}
} // namespace

int runDeepSkyCommand(const std::vector<std::string>& args) {
    try {
        if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h" || args[0] == "help")) {
            help(); return 0;
        }
        if (args == std::vector<std::string>{"schema"}) { schema(); return 0; }
        if (args.size() == 2 && args[0] == "--inspect-recipe") {
            const fs::path path = fs::absolute(args[1]);
            if (!fs::is_regular_file(path) || fs::file_size(path) > 4 * 1024 * 1024)
                throw std::invalid_argument("Recipe is missing or too large");
            std::ifstream file(path, std::ios::binary);
            const auto recipe = DeepSkyRecipe::parse(std::string{std::istreambuf_iterator<char>(file), {}}, path.parent_path());
            std::cout << "{\"type\":\"complete\",\"command\":\"deep-sky inspect\",\"version\":1,\"values\":{";
            std::istringstream lines(recipe.serialize()); std::string line;
            std::getline(lines, line); bool first = true;
            while (std::getline(lines, line)) {
                std::istringstream row(line); std::string key, value; row >> key >> std::quoted(value);
                if (key == "input" || key.empty()) continue;
                if (!first) std::cout << ','; first = false;
                std::cout << json(key) << ':' << json(value);
            }
            std::cout << "},\"inputs\":[";
            for (std::size_t i = 0; i < recipe.inputs.size(); ++i) {
                if (i) std::cout << ','; std::cout << json(recipe.inputs[i].string());
            }
            std::cout << "],\"recipe\":" << json(recipe.serialize()) << "}\n";
            return 0;
        }
        fs::path recipePath, output, inputDirectory, defaultPath, fromMaster;
        bool analysisOnly = false, preflight = false;
        std::optional<std::size_t> analysisWorkers, stackWorkers;
        std::optional<bool> gpuAcceleration;
        std::optional<ComputeBackend> computeBackend;
        std::set<std::string> seen;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto key = args[i];
            if (!seen.insert(key).second) throw std::invalid_argument("Duplicate argument: " + key);
            if (key == "--analyze-only") { analysisOnly = true; continue; }
            if (key == "--preflight") { preflight = true; continue; }
            if (i + 1 == args.size()) throw std::invalid_argument("Missing value for " + key);
            const auto value = args[++i];
            if (key == "--recipe") recipePath = value;
            else if (key == "--from-master") fromMaster = value;
            else if (key == "--output-dir") output = value;
            else if (key == "--input") inputDirectory = value;
            else if (key == "--write-default-recipe") defaultPath = value;
            else if (key == "--analysis-workers") analysisWorkers = workerCount(value, 8, key);
            else if (key == "--stack-workers") stackWorkers = workerCount(value, 16, key);
            else if (key == "--compute-backend") {
                if (value == "auto") computeBackend = ComputeBackend::Automatic;
                else if (value == "cpu") computeBackend = ComputeBackend::CPU;
                else if (value == "metal") computeBackend = ComputeBackend::Metal;
                else throw std::invalid_argument("--compute-backend expects auto, cpu or metal");
            }
            else if (key == "--gpu-acceleration") gpuAcceleration = onOff(value, key);
            else throw std::invalid_argument("Unknown deep-sky argument: " + key);
        }
        DeepSkyRecipe recipe;
        if (!recipePath.empty()) {
            if (!fs::is_regular_file(recipePath) || fs::file_size(recipePath) > 4 * 1024 * 1024) throw std::invalid_argument("Recipe is missing or too large");
            std::ifstream file(recipePath, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(file), {}};
            recipe = DeepSkyRecipe::parse(text, fs::absolute(recipePath).parent_path());
        }
        if (!inputDirectory.empty()) {
            if (!recipe.inputs.empty()) throw std::invalid_argument("Inputs must come from either the recipe or --input");
            recipe.inputs = directoryInputs(inputDirectory);
        }
        if (!defaultPath.empty()) {
            if (!recipePath.empty() || !output.empty() || analysisOnly || preflight || !fromMaster.empty() ||
                analysisWorkers.has_value() || stackWorkers.has_value() || (gpuAcceleration.has_value() || computeBackend.has_value()))
                throw std::invalid_argument("Default recipe writing cannot be combined with a run");
            saveNew(defaultPath, recipe.serialize());
            std::cout << "{\"type\":\"complete\",\"command\":\"deep-sky recipe\",\"output\":" << json(fs::absolute(defaultPath).string()) << "}\n";
            return 0;
        }
        if (output.empty()) throw std::invalid_argument("deep-sky requires --output-dir");
        // Re-parse defaults plus directory inputs too, so every entry point uses
        // identical validation, including per-frame vectors and cross-field rules.
        recipe = DeepSkyRecipe::parse(recipe.serialize(), fs::current_path());
        output = fs::absolute(output).lexically_normal();
        if (preflight) {
            if (analysisOnly || !fromMaster.empty() || analysisWorkers.has_value() || stackWorkers.has_value() ||
                (gpuAcceleration.has_value() || computeBackend.has_value()))
                throw std::invalid_argument("--preflight estimates the full light workflow and cannot be combined with run worker overrides");
            std::cout << inspectDeepSkyPlan(recipe, output).json();
            return 0;
        }
        if (fs::exists(output) && (!fs::is_directory(output) || !fs::is_empty(output))) throw std::invalid_argument("Output directory must be new or empty");
        recipe.options.analysisOnly = analysisOnly;
        recipe.options.analysisWorkers = analysisWorkers.value_or(0);
        recipe.options.stack.combineWorkers = stackWorkers.value_or(0);
        recipe.options.computeBackend = computeBackend.value_or(gpuAcceleration.has_value()
            ? (*gpuAcceleration ? ComputeBackend::Metal : ComputeBackend::CPU) : ComputeBackend::Automatic);
        recipe.options.gpuAcceleration = recipe.options.computeBackend != ComputeBackend::CPU && gpuAcceleration.value_or(true);
        if (!fromMaster.empty() && (analysisOnly || !inputDirectory.empty() || recipePath.empty()))
            throw std::invalid_argument("--from-master requires a recipe and cannot analyze or import lights");
        const auto progress = [](const std::string& stage, double fraction, std::size_t frame, std::size_t total) {
                std::cout << "{\"type\":\"progress\",\"task\":\"deep-sky\",\"command\":\"deep-sky\",\"stage\":" << json(stage)
                    << ",\"progress\":" << fraction << ",\"step\":" << frame << ",\"total\":" << total << "}\n" << std::flush;
            };
        detail::StageProfile profile;
        const auto started = std::chrono::steady_clock::now();
        const auto cpuStarted = std::clock();
        const auto result = [&] {
            std::jthread sampler([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    profile.sampleResidentMemory();
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
            const detail::StageScope scope({&profile, detail::Stage::Other});
            return !fromMaster.empty() ? DeepSkyWorkflow().finishMaster(fromMaster, output, recipe.options, progress) :
                DeepSkyWorkflow().run(recipe.inputs, output, recipe.options, progress);
        }();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double cpu = double(std::clock() - cpuStarted) / CLOCKS_PER_SEC;
        const auto text = report(result, recipe, analysisOnly);
        if (fs::is_directory(output)) {
            saveNew(output / "performance.json", performanceReport(profile, recipe, result, elapsed, cpu));
            saveNew(output / "recipe.txt", recipe.serialize());
            saveNew(output / "report.json", text);
        }
        std::cout << text;
        return result.ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "{\"type\":\"error\",\"command\":\"deep-sky\",\"errorCode\":\"RecipeInvalid\",\"message\":" << json(e.what()) << "}\n";
        return 1;
    }
}
