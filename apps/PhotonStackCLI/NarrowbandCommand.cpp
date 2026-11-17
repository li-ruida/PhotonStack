#include "NarrowbandCommand.hpp"
#include "NarrowbandRecipe.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/Stacker.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace photonstack;
using namespace photonstack::cli;

ImageReadResult readMono(const fs::path& path) {
    FitsCodec codec;
    const auto info = codec.inspect(path);
    if (!info.ok || info.metadata.channels != 1)
        throw std::invalid_argument("Select a single-channel linear FITS for each emission line");
    if (info.metadata.colorProfile != "Linear (assumed)" && info.metadata.colorProfile != "Linear" &&
        info.metadata.colorProfile != "LINEAR-SRGB")
        throw std::invalid_argument("HOO composition requires linear intensities");
    FitsDecodeOptions options;
    options.mode = FitsDecodeMode::Scientific;
    auto result = codec.read(path, options);
    if (!result.ok)
        throw std::runtime_error(result.message);
    return result;
}

ImageBuffer compose(const fs::path& haPath, const fs::path& oxygenPath, double fraction) {
    auto ha = readMono(haPath).image;
    const auto oxygen = readMono(oxygenPath).image;
    if (ha.width != oxygen.width || ha.height != oxygen.height)
        throw std::invalid_argument("Provide registered line masters on the same pixel grid");
    ImageBuffer rgb;
    rgb.width = ha.width;
    rgb.height = ha.height;
    rgb.channels = 4;
    rgb.pixels.resize(rgb.sampleCount());
    for (std::size_t i = 0; i < ha.pixelCount(); ++i) {
        const float h = ha.pixels[i * ha.channels], o = oxygen.pixels[i * oxygen.channels];
        const bool valid = std::isfinite(h) && std::isfinite(o);
        rgb.pixels[i * 4] = valid ? h : 0;
        rgb.pixels[i * 4 + 1] = valid ? (fraction == 0 ? o : fraction == 1 ? h : float(fraction * h + (1 - fraction) * o)) : 0;
        rgb.pixels[i * 4 + 2] = valid ? o : 0;
        rgb.pixels[i * 4 + 3] = valid ? 1 : 0;
    }
    return rgb;
}

ImageBuffer scalarPlane(const ImageBuffer& source, unsigned channel, bool validity = false) {
    ImageBuffer plane;
    plane.width = source.width; plane.height = source.height;
    plane.channels = 1; plane.format = PixelFormat::Float32Gray;
    plane.pixels.resize(plane.pixelCount());
    for (std::size_t i = 0; i < plane.pixelCount(); ++i) {
        const float value = source.pixels[i * source.channels + channel];
        plane.pixels[i] = validity ? (std::isfinite(value) ? 1 : 0) : value;
    }
    return plane;
}
// The new directory owns these exact files. Failed publication removes only
// files created by this invocation, never an existing result or input.
void save(const fs::path& directory, const std::vector<std::pair<const char*, const ImageBuffer*>>& outputs,
          NarrowbandRecipe recipe) {
    if (!fs::create_directory(directory)) throw std::invalid_argument("Choose a new output directory");
    try {
        recipe.outputs.clear();
        for (const auto& [name, image] : outputs) {
            const auto result = FitsCodec().write(*image, directory / name);
            if (!result.ok) throw std::runtime_error(result.message);
            recipe.outputs.push_back({name, fileSHA256(directory / name)});
        }
        verifyNarrowbandInputs(recipe);
        writeNarrowbandRecipe(directory / "recipe.txt", recipe);
        printNarrowbandRecipe(std::cout, recipe, directory);
    } catch (...) {
        std::error_code ignored;
        for (const auto& [name, image] : outputs) fs::remove(directory / name, ignored);
        fs::remove(directory / "recipe.txt", ignored); fs::remove(directory, ignored);
        throw;
    }
}
ImageReadResult decodeForStack(const fs::path& path, const CfaCalibration* calibration, DualBandSampling sampling) {
    ImageReadResult result;
    try {
        const auto planes = readDualBandPlanes(path, calibration, sampling);
        auto& rgba = result.image;
        rgba.width = planes.hydrogen.width;
        rgba.height = planes.hydrogen.height;
        rgba.pixels.resize(rgba.sampleCount());
        for (std::size_t i = 0; i < rgba.pixelCount(); ++i) {
            const float h = planes.hydrogen.pixels[i], o = planes.oxygen.pixels[i];
            const bool valid = std::isfinite(h) && std::isfinite(o);
            // Registration requires finite storage even for masked samples.
            // Preserve missing support in alpha, never as unmasked black data.
            rgba.pixels[4 * i] = valid ? h : 0;
            rgba.pixels[4 * i + 1] = rgba.pixels[4 * i + 2] = valid ? o : 0;
            rgba.pixels[4 * i + 3] = valid ? 1 : 0;
        }
        result.ok = true;
        result.backend = ImageReadBackend::Fits;
    } catch (const std::exception& e) {
        result.errorCode = "NarrowbandDecodeFailed";
        result.message = e.what();
    }
    return result;
}

void stackLines(const NarrowbandRecipe& recipe, const fs::path& directory, const CfaCalibration* calibration) {
    std::vector<fs::path> inputs;
    for (const auto& input : recipe.inputs) inputs.push_back(input.path);
    StackOptions options;
    options.alignSimilarity = true;
    options.method = StackMethod::SigmaClip;
    const auto sampling = narrowbandSampling(recipe.sampling);
    options.readFrame = [calibration, sampling](const auto& path) { return decodeForStack(path, calibration, sampling); };
    options.collectAcceptedWeights = true;
    const auto result = Stacker().stack(inputs, options);
    if (!result.ok)
        throw std::runtime_error(result.message);
    DualBandPlanes planes;
    for (auto* line : {&planes.hydrogen, &planes.oxygen}) {
        line->width = result.image.width;
        line->height = result.image.height;
        line->channels = 1;
        line->format = PixelFormat::Float32Gray;
        line->pixels.resize(line->pixelCount());
    }
    for (std::size_t i = 0; i < result.image.pixelCount(); ++i) {
        const float* pixel = &result.image.pixels[i * result.image.channels];
        const bool valid = result.image.channels < 4 || pixel[3] > 0;
        planes.hydrogen.pixels[i] = valid ? pixel[0] : std::numeric_limits<float>::quiet_NaN();
        planes.oxygen.pixels[i] = valid ? pixel[1] : std::numeric_limits<float>::quiet_NaN();
    }
    const auto haWeight = scalarPlane(result.acceptedWeights, 0);
    const auto oxygenWeight = scalarPlane(result.acceptedWeights, 1);
    const auto coverage = scalarPlane(result.image, 3);
    save(directory, {{"Ha.fits", &planes.hydrogen}, {"OIII.fits", &planes.oxygen}, {"HOO.fits", &result.image},
                     {"Ha-weight.fits", &haWeight}, {"OIII-weight.fits", &oxygenWeight}, {"geometric-coverage.fits", &coverage}}, recipe);
}

} // namespace

int runNarrowband(const std::vector<std::string>& args) {
    NarrowbandRecipe recipe;
    fs::path directory;
    const auto parseFraction = [](const std::string& text) {
        std::size_t used = 0; const double value = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(value) || value < 0 || value > 1)
            throw std::invalid_argument("Green-channel H-alpha fraction must be between 0 and 1");
        return value;
    };
    bool samplingSpecified = false;
    const auto calibrationOption = [&](const std::string& option, const std::string& value) {
        if (option == "--sampling") {
            if (samplingSpecified) throw std::invalid_argument("Duplicate sampling option");
            narrowbandSampling(value); recipe.sampling = value; samplingSpecified = true;
            return true;
        }
        if (option == "--bias" || option == "--dark" || option == "--flat") {
            if (!recipe.calibration.emplace(option.substr(2), NarrowbandFile{fs::canonical(value), {}}).second)
                throw std::invalid_argument("Duplicate calibration master");
        } else if (option == "--dark-bias" || option == "--flat-bias") {
            narrowbandBiasState(value);
            auto& state = option == "--dark-bias" ? recipe.darkBiasState : recipe.flatBiasState;
            if (state != "unknown" || value == "unknown") throw std::invalid_argument("Specify each bias state exactly once");
            state = value;
        } else return false;
        return true;
    };
    if (args.at(0) == "narrowband-stack") {
        if (args.size() < 6 || args[1] != "--output-dir")
            throw std::invalid_argument("Specify --output-dir followed by at least three dual-band CFA exposures");
        directory = args[2]; recipe.operation = "stack";
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i].starts_with("--")) {
                if (i + 1 >= args.size() || !calibrationOption(args[i], args[i + 1])) throw std::invalid_argument("Unknown or incomplete stack option");
                ++i;
            } else recipe.inputs.push_back({fs::canonical(args[i]), {}});
        }
        if (recipe.inputs.size() < 3) throw std::invalid_argument("At least three CFA exposures are required");
    } else {
        std::map<std::string, std::string> values;
        for (std::size_t i = 1; i < args.size(); i += 2) {
            if (i + 1 >= args.size()) throw std::invalid_argument("Missing narrowband option value");
            if (args[0] == "narrowband-extract" && calibrationOption(args[i], args[i + 1])) continue;
            if (!values.emplace(args[i], args[i + 1]).second) throw std::invalid_argument("Duplicate narrowband option");
        }
        if (args[0] == "narrowband-inspect") {
            if (values.size() != 1 || !values.contains("--recipe")) throw std::invalid_argument("Expected --recipe");
            recipe = readNarrowbandRecipe(values.at("--recipe"), true);
            printNarrowbandRecipe(std::cout, recipe, fs::path(values.at("--recipe")).parent_path());
            return 0;
        }
        if (!values.contains("--output-dir")) throw std::invalid_argument("Specify --output-dir");
        directory = values.at("--output-dir");
        if (args[0] == "narrowband-replay") {
            if (!values.contains("--recipe") || values.size() != 2U + values.contains("--green-ha") + values.contains("--sampling"))
                throw std::invalid_argument("Expected --recipe, --output-dir and optional --green-ha or --sampling");
            recipe = readNarrowbandRecipe(values.at("--recipe"), false);
            if (values.contains("--sampling")) {
                if (recipe.operation == "compose") throw std::invalid_argument("Sampling can only change during CFA extraction or stacking");
                narrowbandSampling(values.at("--sampling")); recipe.sampling = values.at("--sampling");
            }
            if (values.contains("--green-ha")) {
                if (recipe.operation != "compose") throw std::invalid_argument("Green mixing requires a compose recipe");
                recipe.green = parseFraction(values.at("--green-ha"));
            }
        } else if (args[0] == "narrowband-extract") {
            if (values.size() != 2 || !values.contains("--input")) throw std::invalid_argument("Expected --input and --output-dir");
            recipe.operation = "extract"; recipe.inputs.push_back({fs::canonical(values.at("--input")), {}});
        } else {
            if (values.size() != 4 || !values.contains("--ha") || !values.contains("--oiii") || !values.contains("--green-ha"))
                throw std::invalid_argument("Expected --ha, --oiii, --green-ha and --output-dir");
            recipe.operation = "compose"; recipe.sampling = "provided"; recipe.green = parseFraction(values.at("--green-ha"));
            recipe.inputs = {{fs::canonical(values.at("--ha")), {}}, {fs::canonical(values.at("--oiii")), {}}};
        }
    }
    if (fs::exists(directory)) throw std::invalid_argument("Choose a new output directory");
    std::set<fs::path> distinct;
    for (auto& file : recipe.inputs) {
        if (!distinct.insert(fs::canonical(file.path)).second) throw std::invalid_argument("Duplicate narrowband input");
        if (file.sha.empty()) file.sha = fileSHA256(file.path);
    }
    for (auto& [role, file] : recipe.calibration) {
        if (!distinct.insert(fs::canonical(file.path)).second) throw std::invalid_argument("Calibration master duplicates an input");
        if (file.sha.empty()) file.sha = fileSHA256(file.path);
    }
    std::optional<CfaCalibration> calibration;
    if (!recipe.calibration.empty()) {
        CfaCalibrationOptions options;
        if (recipe.calibration.contains("bias")) options.bias = recipe.calibration.at("bias").path;
        if (recipe.calibration.contains("dark")) options.dark = recipe.calibration.at("dark").path;
        if (recipe.calibration.contains("flat")) options.flat = recipe.calibration.at("flat").path;
        options.darkBiasState = narrowbandBiasState(recipe.darkBiasState);
        options.flatBiasState = narrowbandBiasState(recipe.flatBiasState);
        if ((options.dark.empty() && recipe.darkBiasState != "unknown") || (options.flat.empty() && recipe.flatBiasState != "unknown"))
            throw std::invalid_argument("Bias state requires its calibration master");
        calibration = prepareCfaCalibration(recipe.inputs[0].path, options);
        for (const auto& input : recipe.inputs) calibration->validate(inspectCfaSensor(input.path));
    } else if (recipe.darkBiasState != "unknown" || recipe.flatBiasState != "unknown") {
        throw std::invalid_argument("Bias state requires its calibration master");
    }
    const auto* prepared = calibration ? &*calibration : nullptr;
    if (recipe.operation == "stack") stackLines(recipe, directory, prepared);
    else if (recipe.operation == "extract") {
        const auto planes = readDualBandPlanes(recipe.inputs[0].path, prepared, narrowbandSampling(recipe.sampling));
        const auto haValid = scalarPlane(planes.hydrogen, 0, true), oxygenValid = scalarPlane(planes.oxygen, 0, true);
        save(directory, {{"Ha.fits", &planes.hydrogen}, {"OIII.fits", &planes.oxygen},
                         {"Ha-valid.fits", &haValid}, {"OIII-valid.fits", &oxygenValid}}, recipe);
    } else {
        const auto rgb = compose(recipe.inputs[0].path, recipe.inputs[1].path, recipe.green);
        const auto valid = scalarPlane(rgb, 3);
        save(directory, {{"HOO.fits", &rgb}, {"validity.fits", &valid}}, recipe);
    }
    return 0;
}
