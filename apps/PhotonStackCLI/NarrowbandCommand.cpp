#include "NarrowbandCommand.hpp"
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
    rgb.channels = 3;
    rgb.pixels.resize(rgb.sampleCount());
    for (std::size_t i = 0; i < ha.pixelCount(); ++i) {
        const float h = ha.pixels[i * ha.channels], o = oxygen.pixels[i * oxygen.channels];
        rgb.pixels[i * 3] = h;
        rgb.pixels[i * 3 + 1] = fraction == 0 ? o : fraction == 1 ? h : float(fraction * h + (1 - fraction) * o);
        rgb.pixels[i * 3 + 2] = o;
    }
    return rgb;
}

// Publish only in a freshly claimed directory; retries never replace previous masters.
void save(const fs::path& directory, const std::vector<std::pair<const char*, const ImageBuffer*>>& outputs) {
    if (!fs::create_directory(directory))
        throw std::invalid_argument("Choose a new output directory");
    try {
        for (const auto& [name, image] : outputs) {
            const auto result = FitsCodec().write(*image, directory / name);
            if (!result.ok)
                throw std::runtime_error(result.message);
        }
    } catch (...) {
        std::error_code ignored;
        for (const auto& [name, image] : outputs)
            fs::remove(directory / name, ignored);
        fs::remove(directory, ignored);
        throw;
    }
}
ImageReadResult decodeForStack(const fs::path& path) {
    ImageReadResult result;
    try {
        const auto planes = readDualBandPlanes(path);
        auto& rgba = result.image;
        rgba.width = planes.hydrogen.width;
        rgba.height = planes.hydrogen.height;
        rgba.pixels.resize(rgba.sampleCount());
        for (std::size_t i = 0; i < rgba.pixelCount(); ++i) {
            const float h = planes.hydrogen.pixels[i], o = planes.oxygen.pixels[i];
            rgba.pixels[4 * i] = h;
            rgba.pixels[4 * i + 1] = rgba.pixels[4 * i + 2] = o;
            rgba.pixels[4 * i + 3] = std::isfinite(h) && std::isfinite(o) ? 1 : 0;
        }
        result.ok = true;
        result.backend = ImageReadBackend::Fits;
    } catch (const std::exception& e) {
        result.errorCode = "NarrowbandDecodeFailed";
        result.message = e.what();
    }
    return result;
}

void stackLines(const std::vector<std::string>& args) {
    if (args.size() < 6 || args[1] != "--output-dir")
        throw std::invalid_argument("Specify --output-dir followed by at least three dual-band CFA exposures");
    std::vector<fs::path> inputs(args.begin() + 3, args.end());
    std::set<fs::path> paths;
    for (const auto& input : inputs)
        if (!paths.insert(fs::canonical(input)).second)
            throw std::invalid_argument("Duplicate exposure");
    if (fs::exists(args[2]))
        throw std::invalid_argument("Choose a new output directory");
    StackOptions options;
    options.alignSimilarity = true;
    options.method = StackMethod::SigmaClip;
    options.readFrame = decodeForStack;
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
    save(args[2], {{"Ha.fits", &planes.hydrogen}, {"OIII.fits", &planes.oxygen}, {"HOO.fits", &result.image}});
    std::cout << R"({"type":"complete","command":"narrowband-stack","frames":)" << inputs.size() << "}\n";
}

} // namespace

int runNarrowband(const std::vector<std::string>& args) {
    if (args.at(0) == "narrowband-stack") {
        stackLines(args);
        return 0;
    }
    const bool extract = args.at(0) == "narrowband-extract";
    std::map<std::string, std::string> values;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size() || !values.emplace(args[i], args[i + 1]).second)
            throw std::invalid_argument("Missing or duplicate narrowband option");
    }
    if (!values.contains("--output-dir"))
        throw std::invalid_argument("Specify --output-dir");
    if (extract) {
        if (values.size() != 2 || !values.contains("--input"))
            throw std::invalid_argument("Expected --input and --output-dir");
        const auto planes = readDualBandPlanes(values.at("--input"));
        save(values.at("--output-dir"), {{"Ha.fits", &planes.hydrogen}, {"OIII.fits", &planes.oxygen}});
    } else {
        if (values.size() != 4 || !values.contains("--ha") || !values.contains("--oiii") ||
            !values.contains("--green-ha"))
            throw std::invalid_argument("Expected --ha, --oiii, --green-ha and --output-dir");
        const auto& text = values.at("--green-ha");
        double fraction;
        std::size_t used = 0;
        fraction = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(fraction) || fraction < 0 || fraction > 1)
            throw std::invalid_argument("Green-channel H-alpha fraction must be between 0 and 1");
        const auto rgb = compose(values.at("--ha"), values.at("--oiii"), fraction);
        save(values.at("--output-dir"), {{"HOO.fits", &rgb}});
    }
    std::cout << "{\"type\":\"complete\",\"command\":\"" << args[0] << "\"}\n";
    return 0;
}
