#include "DeepSkyCommand.hpp"
#include "photonstack/DeepSkyRecipe.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
        << ",\"crop\":{\"x\":" << r.cropX << ",\"y\":" << r.cropY << ",\"width\":" << r.cropWidth << ",\"height\":" << r.cropHeight << '}'
        << ",\"denoiseH\":" << r.denoiseH << ",\"development\":{\"toneScale\":" << r.development.toneScale
        << ",\"shadowNoise\":" << r.development.shadowNoise << ",\"whitePoint\":" << r.development.whitePoint << ",\"calibrationStars\":" << r.development.calibrationStars << ",\"gains\":["
        << r.development.gains[0] << ',' << r.development.gains[1] << ',' << r.development.gains[2] << "]},\"channelAlignment\":[";
    for (unsigned c=0;c<3;++c) {
        if(c)out << ',';
        out << "{\"applied\":" << boolean(r.channelAlignment.applied[c])
            << ",\"dx\":" << r.channelAlignment.dx[c] << ",\"dy\":" << r.channelAlignment.dy[c]
            << ",\"scatter\":" << r.channelAlignment.scatter[c] << ",\"stars\":" << r.channelAlignment.stars[c] << '}';
    }
    out << "],\"frames\":[";
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
        bool analysisOnly = false;
        std::set<std::string> seen;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto key = args[i];
            if (!seen.insert(key).second) throw std::invalid_argument("Duplicate argument: " + key);
            if (key == "--analyze-only") { analysisOnly = true; continue; }
            if (i + 1 == args.size()) throw std::invalid_argument("Missing value for " + key);
            const auto value = args[++i];
            if (key == "--recipe") recipePath = value;
            else if (key == "--from-master") fromMaster = value;
            else if (key == "--output-dir") output = value;
            else if (key == "--input") inputDirectory = value;
            else if (key == "--write-default-recipe") defaultPath = value;
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
            if (!recipePath.empty() || !output.empty() || analysisOnly || !fromMaster.empty()) throw std::invalid_argument("Default recipe writing cannot be combined with a run");
            saveNew(defaultPath, recipe.serialize());
            std::cout << "{\"type\":\"complete\",\"command\":\"deep-sky recipe\",\"output\":" << json(fs::absolute(defaultPath).string()) << "}\n";
            return 0;
        }
        if (output.empty()) throw std::invalid_argument("deep-sky requires --output-dir");
        // Re-parse defaults plus directory inputs too, so every entry point uses
        // identical validation, including per-frame vectors and cross-field rules.
        recipe = DeepSkyRecipe::parse(recipe.serialize(), fs::current_path());
        output = fs::absolute(output).lexically_normal();
        if (fs::exists(output) && (!fs::is_directory(output) || !fs::is_empty(output))) throw std::invalid_argument("Output directory must be new or empty");
        recipe.options.analysisOnly = analysisOnly;
        if (!fromMaster.empty() && (analysisOnly || !inputDirectory.empty() || recipePath.empty()))
            throw std::invalid_argument("--from-master requires a recipe and cannot analyze or import lights");
        const auto progress = [](const std::string& stage, double fraction, std::size_t frame, std::size_t total) {
                std::cout << "{\"type\":\"progress\",\"task\":\"deep-sky\",\"command\":\"deep-sky\",\"stage\":" << json(stage)
                    << ",\"progress\":" << fraction << ",\"step\":" << frame << ",\"total\":" << total << "}\n" << std::flush;
            };
        const auto result = !fromMaster.empty() ? DeepSkyWorkflow().finishMaster(fromMaster, output, recipe.options, progress) :
            DeepSkyWorkflow().run(recipe.inputs, output, recipe.options, progress);
        const auto text = report(result, recipe, analysisOnly);
        if (fs::is_directory(output)) {
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
