#include "StarLayersCommand.hpp"
#include "photonstack/FileDigest.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/ImageCodec.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace photonstack;
using Arguments = std::map<std::string, std::string>;

std::string required(const Arguments& args, const std::string& key) {
    if (!args.contains(key))
        throw std::invalid_argument("Missing " + key);
    return args.at(key);
}
double number(const Arguments& args, const std::string& key, double initial, double low, double high) {
    if (!args.contains(key))
        return initial;
    const auto& s = args.at(key);
    std::size_t end = 0;
    const double v = std::stod(s, &end);
    if (end != s.size() || !std::isfinite(v) || v < low || v > high)
        throw std::invalid_argument("Invalid " + key);
    return v;
}
void check(const ImageWriteResult& r) {
    if (!r.ok)
        throw std::runtime_error(r.message);
}
ImageBuffer readLinear(const fs::path& path) {
    ImageReadOptions options;
    options.fits.mode = FitsDecodeMode::Scientific;
    options.fits.debayer = false;
    auto result = ImageCodec().read(path, options);
    if (!result.ok)
        throw std::runtime_error(result.message);
    auto& image = result.image;
    if (image.channels != 4 || image.empty())
        throw std::invalid_argument("Expected RGBA input");
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        auto* s = &image.pixels[p * 4];
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(s[c]))
                throw std::invalid_argument("Nonfinite samples require a validity mask");
        if (image.colorEncoding == ColorEncoding::SRGB)
            for (int c = 0; c < 3; ++c)
                s[c] = s[c] <= .04045F ? s[c] / 12.92F : std::pow((s[c] + .055F) / 1.055F, 2.4F);
    }
    image.colorEncoding = ColorEncoding::Linear;
    return std::move(image);
}
void compatible(const ImageBuffer& a, const ImageBuffer& b) {
    if (a.width != b.width || a.height != b.height)
        throw std::invalid_argument("Layers must have identical dimensions and alignment");
    for (std::size_t p = 0; p < a.pixelCount(); ++p)
        if (std::abs(a.pixels[4 * p + 3] - b.pixels[4 * p + 3]) > 1e-5)
            throw std::invalid_argument("Layer coverage differs; use uncropped images from the same source");
}
void display(const ImageBuffer& image, const fs::path& path) {
    ImageWriteOptions options;
    options.bitDepth = ImageWriteBitDepth::Sixteen;
    options.colorSpace = ImageWriteColorSpace::SRGB;
    check(ImageCodec().write(image, path, options));
}
// Only the directory successfully claimed by this invocation may be rolled back.
struct OutputDirectory {
    fs::path path;
    bool committed = false;
    explicit OutputDirectory(const fs::path& p) : path(p) {
        if (!fs::create_directory(path))
            throw std::invalid_argument("Choose a new output directory");
    }
    ~OutputDirectory() {
        if (!committed) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};
void prepare(const Arguments& args) {
    if (args.size() != 2)
        throw std::invalid_argument("Expected --input and --output-dir");
    auto input = readLinear(required(args, "--input"));
    for (std::size_t p = 0; p < input.pixelCount(); ++p) {
        if (input.pixels[4 * p + 3] <= 0)
            for (int c = 0; c < 3; ++c)
                input.pixels[4 * p + c] = 0;
        input.pixels[4 * p + 3] = 1;
    }
    OutputDirectory out(required(args, "--output-dir"));
    display(input, out.path / "input.tiff");
    out.committed = true;
    std::cout << R"({"type":"complete","command":"star-layers"})" << std::endl;
}
void split(const Arguments& args) {
    for (const auto& [key, v] : args)
        if (!std::set<std::string>{"--input", "--starless", "--output-dir"}.contains(key))
            throw std::invalid_argument("Unknown split option: " + key);
    const fs::path original = required(args, "--input"), removed = required(args, "--starless");
    // A display-stage tool cannot remove stars from an arbitrary linear master.
    const auto isTiff = [](const fs::path& p) {
        auto e = p.extension().string();
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
        return e == ".tif" || e == ".tiff";
    };
    if (!isTiff(original) || !isTiff(removed))
        throw std::invalid_argument("Star layer separation requires developed TIFF images");
    auto stars = readLinear(original);
    auto starless = readLinear(removed);
    if (stars.width != starless.width || stars.height != starless.height)
        throw std::invalid_argument("Layers must have identical dimensions and alignment");
    for (std::size_t p = 0; p < stars.pixelCount(); ++p) {
        const auto i = p * 4;
        if (starless.pixels[i + 3] + 1e-5 < stars.pixels[i + 3])
            throw std::invalid_argument("The starless image is missing covered source pixels");
        starless.pixels[i + 3] = stars.pixels[i + 3];
        if (stars.pixels[i + 3] <= 0)
            for (int c = 0; c < 3; ++c)
                stars.pixels[i + c] = starless.pixels[i + c] = 0;
    }
    double reconstructionError = 0;
    std::size_t negative = 0;
    for (std::size_t p = 0; p < stars.pixelCount(); ++p) {
        for (int c = 0; c < 3; ++c) {
            auto& v = stars.pixels[4 * p + c];
            const float source = v;
            v -= starless.pixels[4 * p + c];
            if (v < 0)
                ++negative;
            reconstructionError =
                std::max(reconstructionError, std::abs(double(v) + starless.pixels[4 * p + c] - source));
        }
    }
    OutputDirectory out(required(args, "--output-dir"));
    check(FitsCodec().write(stars, out.path / "stars.fits"));
    check(FitsCodec().write(starless, out.path / "starless.fits"));
    display(stars, out.path / "stars-preview.tiff");
    display(starless, out.path / "starless-preview.tiff");
    std::ofstream manifest(out.path / "layers.txt");
    manifest << "PhotonStack-StarLayers-1\n"
             << fileSHA256(out.path / "starless.fits") << "\n"
             << fileSHA256(out.path / "stars.fits") << "\n"
             << fileSHA256(original) << "\n"
             << fileSHA256(removed) << "\n";
    manifest.close();
    if (!manifest)
        throw std::runtime_error("Could not save star-layer provenance");
    out.committed = true;
    std::cout << "{\"type\":\"complete\",\"command\":\"star-layers\",\"mode\":\"split\",\"width\":" << stars.width
              << ",\"height\":" << stars.height << ",\"negativeSamples\":" << negative
              << ",\"reconstructionError\":" << std::setprecision(10) << reconstructionError << "}\n";
}
void adjust(float* color, double exposure, double gamma, double saturation) {
    if (exposure == 0 && gamma == 1 && saturation == 1)
        return;
    for (int c = 0; c < 3; ++c)
        color[c] =
            float(std::copysign(std::pow(std::abs(double(color[c])), 1 / gamma), color[c]) * std::exp2(exposure));
    const double l = .2126 * color[0] + .7152 * color[1] + .0722 * color[2];
    for (int c = 0; c < 3; ++c)
        color[c] = float(l + saturation * (color[c] - l));
}
void render(const Arguments& args) {
    for (const auto& [key, v] : args)
        if (!std::set<std::string>{"--layers", "--output-dir", "--stars-ev", "--nebula-ev", "--nebula-gamma",
                                   "--stars-saturation", "--nebula-saturation"}
                 .contains(key))
            throw std::invalid_argument("Unknown render option: " + key);
    const fs::path input = required(args, "--layers");
    const double starsEV = number(args, "--stars-ev", 0, -8, 4), nebulaEV = number(args, "--nebula-ev", 0, -4, 4);
    const double gamma = number(args, "--nebula-gamma", 1, .25, 4);
    const double starSat = number(args, "--stars-saturation", 1, 0, 3),
                 nebulaSat = number(args, "--nebula-saturation", 1, 0, 3);
    std::ifstream manifest(input / "layers.txt");
    std::string version, h1, h2;
    if (!std::getline(manifest, version) || version != "PhotonStack-StarLayers-1" || !std::getline(manifest, h1) ||
        !std::getline(manifest, h2) || h1 != fileSHA256(input / "starless.fits") ||
        h2 != fileSHA256(input / "stars.fits"))
        throw std::invalid_argument("Layer files changed or the layer manifest is invalid");
    auto nebula = readLinear(input / "starless.fits"), stars = readLinear(input / "stars.fits");
    compatible(nebula, stars);
    for (std::size_t p = 0; p < nebula.pixelCount(); ++p) {
        adjust(&nebula.pixels[4 * p], nebulaEV, gamma, nebulaSat);
        adjust(&stars.pixels[4 * p], starsEV, 1, starSat);
    }
    OutputDirectory out(required(args, "--output-dir"));
    display(nebula, out.path / "starless-preview.tiff");
    display(stars, out.path / "stars-preview.tiff");
    std::size_t below = 0, above = 0;
    for (std::size_t p = 0; p < nebula.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c) {
            auto& v = nebula.pixels[p * 4 + c];
            v += stars.pixels[p * 4 + c];
            if (!std::isfinite(v))
                throw std::invalid_argument("Layer adjustment exceeds float range");
            if (v < 0)
                ++below;
            if (v > 1)
                ++above;
        }
    check(FitsCodec().write(nebula, out.path / "combined.fits"));
    display(nebula, out.path / "combined.tiff");
    std::ofstream recipe(out.path / "recipe.json");
    recipe << std::setprecision(17) << "{\"version\":1,\"starsEV\":" << starsEV << ",\"nebulaEV\":" << nebulaEV
           << ",\"nebulaGamma\":" << gamma << ",\"starsSaturation\":" << starSat
           << ",\"nebulaSaturation\":" << nebulaSat << "}\n";
    recipe.close();
    if (!recipe)
        throw std::runtime_error("Could not save layer recipe");
    out.committed = true;
    std::cout << "{\"type\":\"complete\",\"command\":\"star-layers\",\"mode\":\"render\",\"belowBlackSamples\":"
              << below << ",\"aboveWhiteSamples\":" << above << "}\n";
}
} // namespace
int runStarLayers(const std::vector<std::string>& args) {
    if (args.size() < 2)
        throw std::invalid_argument("Expected star-layers split or render");
    Arguments values;
    for (std::size_t i = 2; i < args.size(); i += 2) {
        if (i + 1 >= args.size() || !values.emplace(args[i], args[i + 1]).second)
            throw std::invalid_argument("Missing or duplicate star layer option");
    }
    if (args[1] == "prepare")
        prepare(values);
    else if (args[1] == "split")
        split(values);
    else if (args[1] == "render")
        render(values);
    else
        throw std::invalid_argument("Expected star-layers split or render");
    return 0;
}
