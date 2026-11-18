#include "SpectralCalibrationCommand.hpp"
#include "SpectralData.hpp"
#include "photonstack/FitsCodec.hpp"
#include <algorithm>
#include <iostream>
#include <optional>
namespace {
using namespace photonstack;
using namespace photonstack::cli;
namespace fs = std::filesystem;
const std::set<std::string> dependencies{"profile.txt", "catalog.txt", "response-source-license.txt"};
const std::set<std::string> allProducts{"before.tiff", "after.tiff", "calibrated.fits", "report.json"};
struct Recipe {
    fs::path source;
    std::string sha;
    SpectralCalibrationOptions options;
    bool valid = false;
    std::array<double, 3> gains{1, 1, 1};
    std::map<std::string, std::string> dependencies, products;
};
std::string bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read spectral product");
    return {std::istreambuf_iterator<char>(in), {}};
}
void save(const fs::path& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary);
    out << value;
    out.close();
    if (!out)
        throw std::runtime_error("Cannot write spectral product");
}
void verifySource(const Recipe& r) {
    if (!r.source.is_absolute() || !channelSHA(r.sha) || fileSHA256(r.source) != r.sha)
        throw std::runtime_error("Spectral calibration source changed");
}
void verify(const fs::path& dir, const std::map<std::string, std::string>& files) {
    for (const auto& [name, sha] : files)
        if (fileSHA256(dir / name) != sha)
            throw std::runtime_error("Spectral dependency or product changed: " + name);
}
void validateOptions(const SpectralCalibrationOptions& o) {
    const auto& a = o.aperture;
    for (double n : {a.radius, a.skyInner, a.skyOuter, a.saturation, a.minSNR, a.maxCenterOffset,
                     o.maxRelativeSpectralError, o.maxApertureColorChange, o.maxValidationLogError})
        if (!std::isfinite(n))
            throw std::invalid_argument("Nonfinite spectral option");
    if (a.radius < 3 || a.radius > 18 || a.skyInner <= a.radius + 1 || a.skyOuter < a.skyInner + 2 || a.skyOuter > 38 ||
        a.saturation < 0 || a.minSNR < 1 || a.maxCenterOffset <= 0 || a.maxCenterOffset > 5 ||
        o.maxRelativeSpectralError <= 0 || o.maxRelativeSpectralError > .5 || o.maxApertureColorChange <= 0 ||
        o.maxApertureColorChange > .2 || o.maxValidationLogError <= 0 || o.maxValidationLogError > 1 ||
        o.excluded.size() > 10000)
        throw std::invalid_argument("Invalid spectral aperture or validation limits");
    for (const auto& id : o.excluded)
        spectralID(id);
}
std::string configuration(const Recipe& r) {
    const auto& o = r.options;
    const auto& a = o.aperture;
    std::ostringstream out;
    out << std::setprecision(17) << std::quoted(r.source.string()) << ' ' << r.sha << '\n'
        << r.valid << ' ' << r.gains[0] << ' ' << r.gains[1] << ' ' << r.gains[2] << '\n'
        << a.radius << ' ' << a.skyInner << ' ' << a.skyOuter << ' ' << a.saturation << ' ' << a.minSNR << ' '
        << a.maxCenterOffset << '\n'
        << o.maxRelativeSpectralError << ' ' << o.maxApertureColorChange << ' ' << o.maxValidationLogError << '\n'
        << o.excluded.size() << '\n';
    for (const auto& id : o.excluded)
        out << std::quoted(id) << '\n';
    for (const auto& [name, sha] : r.dependencies)
        out << "dependency " << name << ' ' << sha << '\n';
    return out.str();
}
void writeRecipe(const fs::path& path, const Recipe& r) {
    std::ostringstream out;
    out << "PhotonStack-SPCC-1\n" << configuration(r);
    for (const auto& [name, sha] : r.products)
        out << "product " << name << ' ' << sha << '\n';
    save(path, out.str());
}
Recipe readRecipe(const fs::path& path) {
    if (fs::file_size(path) > 2 * 1024 * 1024)
        throw std::invalid_argument("Spectral recipe too large");
    Recipe r;
    std::ifstream in(path);
    std::string magic, source;
    auto& o = r.options;
    auto& a = o.aperture;
    unsigned n = 0;
    if (!(in >> magic) || magic != "PhotonStack-SPCC-1" ||
        !(in >> std::quoted(source) >> r.sha >> r.valid >> r.gains[0] >> r.gains[1] >> r.gains[2] >> a.radius >>
          a.skyInner >> a.skyOuter >> a.saturation >> a.minSNR >> a.maxCenterOffset >> o.maxRelativeSpectralError >>
          o.maxApertureColorChange >> o.maxValidationLogError >> n) ||
        n > 10000)
        throw std::invalid_argument("Invalid spectral recipe");
    r.source = source;
    for (unsigned i = 0; i < n; ++i) {
        std::string id;
        if (!(in >> std::quoted(id)) || !o.excluded.insert(id).second)
            throw std::invalid_argument("Invalid spectral exclusions");
    }
    validateOptions(o);
    for (double g : r.gains)
        if (!std::isfinite(g) || g < .05 || g > 20)
            throw std::invalid_argument("Invalid saved spectral gain");
    if ((!r.valid && r.gains != std::array<double, 3>{1, 1, 1}) || r.gains[1] != 1)
        throw std::invalid_argument("Invalid spectral normalization");
    std::string kind, name, sha;
    while (in >> kind) {
        if (!(in >> name >> sha) || !channelSHA(sha))
            throw std::invalid_argument("Invalid spectral product digest");
        if (kind == "dependency" && dependencies.contains(name) && r.dependencies.emplace(name, sha).second)
            continue;
        if (kind == "product" && allProducts.contains(name) && r.products.emplace(name, sha).second)
            continue;
        throw std::invalid_argument("Invalid spectral product record");
    }
    const std::set<std::string> expected = r.valid ? allProducts : std::set<std::string>{"before.tiff", "report.json"};
    if (!in.eof() || !r.dependencies.contains("profile.txt") || !r.dependencies.contains("catalog.txt") ||
        r.products.size() != expected.size())
        throw std::invalid_argument("Incomplete spectral recipe");
    for (const auto& name : expected)
        if (!r.products.contains(name))
            throw std::invalid_argument("Missing spectral product");
    verifySource(r);
    verify(path.parent_path(), r.dependencies);
    verify(path.parent_path(), r.products);
    if (fs::file_size(path.parent_path() / "report.json") > 64 * 1024 * 1024 ||
        !bytes(path.parent_path() / "report.json")
             .starts_with("{\"configuration\":" + channelJSON(configuration(r)) + ","))
        throw std::runtime_error("Spectral report does not match saved source, options and dependencies");
    (void)readSpectralProfile(path.parent_path() / "profile.txt");
    (void)readSpectralCatalog(path.parent_path() / "catalog.txt");
    return r;
}
void number(std::ostream& out, double value) {
    if (std::isfinite(value))
        out << value;
    else
        out << "null";
}
template <class T> void array(std::ostream& out, const T& values) {
    out << '[';
    bool first = true;
    for (auto n : values) {
        if (!first)
            out << ',';
        first = false;
        number(out, double(n));
    }
    out << ']';
}
void photometry(std::ostream& out, const StellarApertureResult& p) {
    out << "{\"valid\":" << (p.valid ? "true" : "false") << ",\"status\":" << channelJSON(p.status) << ",\"x\":" << p.x
        << ",\"y\":" << p.y << ",\"area\":" << p.area << ",\"flux\":";
    array(out, p.flux);
    out << ",\"backgroundNoise\":";
    array(out, p.backgroundNoise);
    out << ",\"skySigma\":";
    array(out, p.skySigma);
    out << ",\"snr\":";
    array(out, p.snr);
    out << ",\"skyUsed\":";
    array(out, p.skyUsed);
    out << ",\"skyPlane\":[";
    for (unsigned c = 0; c < 3; ++c) {
        if (c)
            out << ',';
        array(out, p.skyPlane[c]);
    }
    out << "]}";
}
std::string report(const Recipe& recipe, const SpectralProfile& p, const SpectralCatalog& catalog,
                   const SpectralCalibrationReview& r, double previewLow, double previewHigh) {
    std::ostringstream out;
    out << std::setprecision(17);
    const auto& f = r.fit;
    const auto& o = recipe.options;
    const auto& a = o.aperture;
    out << "{\"configuration\":" << channelJSON(configuration(recipe))
        << ",\"schema\":1,\"algorithm\":\"PhotonStack-SPCC-1\",\"source\":" << channelJSON(recipe.source.string())
        << ",\"sourceSHA\":" << channelJSON(recipe.sha) << ",\"width\":" << r.width << ",\"height\":" << r.height
        << ",\"profile\":{\"name\":" << channelJSON(p.name) << ",\"mode\":" << channelJSON(p.mode)
        << ",\"quality\":" << channelJSON(p.quality) << ",\"provenance\":" << channelJSON(p.provenance)
        << ",\"license\":" << channelJSON(p.license)
        << ",\"sha\":" << channelJSON(recipe.dependencies.at("profile.txt"))
        << ",\"whiteName\":" << channelJSON(p.whiteName) << ",\"whiteProvenance\":" << channelJSON(p.whiteProvenance)
        << ",\"whiteLicense\":" << channelJSON(p.whiteLicense)
        << "},\"catalog\":{\"name\":" << channelJSON(catalog.name) << ",\"epoch\":" << catalog.epoch
        << ",\"provenance\":" << channelJSON(catalog.provenance) << ",\"license\":" << channelJSON(catalog.license)
        << ",\"sha\":" << channelJSON(recipe.dependencies.at("catalog.txt")) << "},\"options\":{\"radius\":" << a.radius
        << ",\"skyInner\":" << a.skyInner << ",\"skyOuter\":" << a.skyOuter << ",\"saturation\":" << a.saturation
        << ",\"minSNR\":" << a.minSNR << ",\"maxCenterOffset\":" << a.maxCenterOffset
        << ",\"maxRelativeSpectralError\":" << o.maxRelativeSpectralError
        << ",\"maxApertureColorChange\":" << o.maxApertureColorChange
        << ",\"maxValidationLogError\":" << o.maxValidationLogError << ",\"excluded\":[";
    bool first = true;
    for (const auto& id : o.excluded) {
        if (!first)
            out << ',';
        first = false;
        out << channelJSON(id);
    }
    out << "]},\"preview\":{\"resampling\":\"area-coverage\",\"low\":" << previewLow << ",\"high\":" << previewHigh
        << ",\"transfer\":\"shared asinh(10*x)/asinh(10), source black reference transformed by the same channel "
           "gains, shared scale, clipped to display range\"},\"detected\":"
        << r.detected << ",\"whiteFlux\":";
    array(out, r.whiteFlux);
    out << ",\"fit\":{\"valid\":" << (f.valid ? "true" : "false") << ",\"reason\":" << channelJSON(f.reason)
        << ",\"training\":" << f.training << ",\"validation\":" << f.validation << ",\"rejected\":" << f.rejected
        << ",\"regions\":" << f.validationRegions << ",\"gains\":";
    array(out, f.gains);
    out << ",\"instrumentGains\":";
    array(out, f.instrumentGains);
    out << ",\"whiteRatios\":";
    array(out, f.whiteRatios);
    out << ",\"trainingScatter\":";
    array(out, f.trainingScatter);
    out << ",\"validationMedian\":";
    array(out, f.validationMedian);
    out << ",\"validationP90\":";
    array(out, f.validationP90);
    out << "},\"stars\":[";
    std::map<std::string, const SpectralColorResidual*> fitted;
    for (const auto& s : f.stars)
        fitted.emplace(s.id, &s);
    for (std::size_t i = 0; i < r.stars.size(); ++i) {
        if (i)
            out << ',';
        const auto& s = r.stars[i];
        out << "{\"id\":" << channelJSON(s.id) << ",\"status\":" << channelJSON(s.status) << ",\"predictedX\":";
        if (s.hasPosition)
            number(out, s.predictedX);
        else
            out << "null";
        out << ",\"predictedY\":";
        if (s.hasPosition)
            number(out, s.predictedY);
        else
            out << "null";
        out << ",\"predictedFlux\":";
        array(out, s.predicted);
        out << ",\"spectralErrorBound\":";
        array(out, s.spectralErrorBound);
        out << ",\"apertureColorChange\":";
        array(out, s.apertureColorChange);
        out << ",\"photometry\":";
        photometry(out, s.photometry);
        out << ",\"expandedPhotometry\":";
        photometry(out, s.expandedPhotometry);
        out << ",\"heldOut\":";
        const auto found = fitted.find(s.id);
        out << (found != fitted.end() && found->second->heldOut ? "true" : "false") << ",\"residual\":";
        if (found != fitted.end())
            array(out, found->second->residual);
        else
            out << "null";
        out << '}';
    }
    out << "],\"limitations\":[\"Response quality is user supplied; residual validation does not establish instrument "
           "response accuracy.\","
        << "\"Catalog coordinates are supplied at the stated epoch; verify compatibility with the image.\","
        << "\"Background noise excludes source shot noise and resampling covariance; undetected companions and "
           "variability may remain.\"]}\n";
    return out.str();
}
ImageBuffer resized(const ImageBuffer& image) {
    const auto width = unsigned(
        std::max(1., std::round(double(image.width) * std::min(1., 1600. / std::max(image.width, image.height)))));
    if (width == image.width)
        return image;
    ImageBuffer output;
    output.width = width;
    output.height = unsigned(std::max(1., std::round(double(image.height) * width / image.width)));
    output.channels = 4;
    output.pixels.resize(output.sampleCount());
    // Area integration avoids choosing just four pixels from an undersampled
    // stellar core. Coverage weights color; output alpha is the covered area.
    for (unsigned y = 0; y < output.height; ++y)
        for (unsigned x = 0; x < output.width; ++x) {
            const double x0 = double(x) * image.width / output.width, x1 = double(x + 1) * image.width / output.width;
            const double y0 = double(y) * image.height / output.height,
                         y1 = double(y + 1) * image.height / output.height;
            std::array<double, 3> sum{};
            double coverage = 0;
            for (unsigned sy = unsigned(std::floor(y0)); sy < std::min(image.height, unsigned(std::ceil(y1))); ++sy)
                for (unsigned sx = unsigned(std::floor(x0)); sx < std::min(image.width, unsigned(std::ceil(x1)));
                     ++sx) {
                    const auto p = (std::size_t(sy) * image.width + sx) * 4;
                    const double area = (std::min(x1, double(sx + 1)) - std::max(x0, double(sx))) *
                                        (std::min(y1, double(sy + 1)) - std::max(y0, double(sy)));
                    const double weight = area * std::clamp(double(image.pixels[p + 3]), 0., 1.);
                    coverage += weight;
                    for (unsigned c = 0; c < 3; ++c)
                        sum[c] += weight * image.pixels[p + c];
                }
            const auto p = (std::size_t(y) * output.width + x) * 4;
            if (coverage > 1e-12)
                for (unsigned c = 0; c < 3; ++c)
                    output.pixels[p + c] = float(sum[c] / coverage);
            output.pixels[p + 3] = float(std::clamp(coverage / ((x1 - x0) * (y1 - y0)), 0., 1.));
        }
    return output;
}
std::pair<double, double> range(const ImageBuffer& image) {
    std::vector<float> values;
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        if (image.channels == 3 || image.pixels[p * image.channels + 3] > .99F)
            for (unsigned c = 0; c < 3; ++c)
                if (std::isfinite(image.pixels[p * image.channels + c]))
                    values.push_back(image.pixels[p * image.channels + c]);
    if (values.empty())
        throw std::runtime_error("No fully covered spectral preview pixels");
    const auto lowIndex = std::size_t(.01 * (values.size() - 1));
    const auto highIndex = std::size_t(.998 * (values.size() - 1));
    std::nth_element(values.begin(), values.begin() + lowIndex, values.end());
    const double low = values[lowIndex];
    if (highIndex > lowIndex)
        std::nth_element(values.begin() + lowIndex + 1, values.begin() + highIndex, values.end());
    return {low, values[highIndex]};
}
void preview(ImageBuffer image, double low, double high, const std::array<double, 3>& gains, const fs::path& output) {
    const double span = std::max(high - low, 1e-12);
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (unsigned c = 0; c < 3; ++c) {
            // Transform the source black reference with the science gains.
            // Both images share one display scale and one source statistic.
            const double value = std::max(0., (image.pixels[p * image.channels + c] - low * gains[c]) / span);
            image.pixels[p * image.channels + c] = float(std::clamp(std::asinh(10 * value) / std::asinh(10.), 0., 1.));
        }
    image.colorEncoding = ColorEncoding::SRGB;
    ImageWriteOptions options;
    options.bitDepth = ImageWriteBitDepth::Sixteen;
    const auto written = ImageCodec().write(image, output, options);
    if (!written.ok)
        throw std::runtime_error(written.message);
}
void print(const fs::path& directory) {
    std::cout << "{\"type\":\"complete\",\"command\":\"spcc\",\"directory\":"
              << channelJSON(fs::absolute(directory).string()) << ",\"report\":" << bytes(directory / "report.json")
              << "}\n";
}
void run(Recipe r, const fs::path& profile, const fs::path& catalog, const fs::path& directory, bool replay) {
    if (directory.empty() || fs::exists(directory))
        throw std::invalid_argument("Choose a new spectral output directory");
    validateOptions(r.options);
    verifySource(r);
    const auto expectedProducts = r.products;
    const auto license = profile.parent_path() / "response-source-license.txt";
    if (r.dependencies.empty()) {
        r.dependencies = {{"profile.txt", fileSHA256(profile)}, {"catalog.txt", fileSHA256(catalog)}};
        if (fs::exists(license)) {
            if (!fs::is_regular_file(license) || fs::is_symlink(license) || fs::file_size(license) > 1024 * 1024)
                throw std::invalid_argument("Invalid or oversized response license file");
            r.dependencies["response-source-license.txt"] = fileSHA256(license);
        }
    }
    if (!fs::create_directory(directory))
        throw std::invalid_argument("Choose a new spectral output directory");
    try {
        fs::copy_file(profile, directory / "profile.txt");
        fs::copy_file(catalog, directory / "catalog.txt");
        if (r.dependencies.contains("response-source-license.txt"))
            fs::copy_file(license, directory / "response-source-license.txt");
        verify(directory, r.dependencies);
        const auto p = readSpectralProfile(directory / "profile.txt");
        const auto c = readSpectralCatalog(directory / "catalog.txt");
        const auto metadata = FitsCodec().inspect(r.source);
        if (!metadata.ok || (metadata.metadata.channels != 3 && metadata.metadata.channels != 4))
            throw std::invalid_argument("Spectral calibration requires a native RGB FITS master, not mono or CFA");
        const auto source = readLinearScienceImage(r.source);
        if (!source.sky)
            throw std::invalid_argument("Spectral calibration requires solved ICRS WCS");
        const auto reviewed =
            reviewSpectralCalibration(source.image, *source.sky, c.stars, p.response, p.white, r.options);
        r.valid = reviewed.fit.valid;
        r.gains = r.valid ? reviewed.fit.gains : std::array<double, 3>{1, 1, 1};
        auto before = resized(source.image);
        const auto [low, high] = range(before);
        preview(std::move(before), low, high, {1, 1, 1}, directory / "before.tiff");
        r.products.clear();
        if (r.valid) {
            const auto calibrated = applySpectralCalibration(source.image, reviewed.fit);
            const auto written = FitsCodec().write(calibrated, directory / "calibrated.fits", &source.sky->grid,
                                                   &source.sky->distortion);
            if (!written.ok)
                throw std::runtime_error(written.message);
            preview(resized(calibrated), low, high, r.gains, directory / "after.tiff");
        }
        save(directory / "report.json", report(r, p, c, reviewed, low, high));
        for (const auto& name : allProducts)
            if (fs::exists(directory / name))
                r.products[name] = fileSHA256(directory / name);
        verifySource(r);
        verify(directory, r.dependencies);
        if (replay && r.products != expectedProducts)
            throw std::runtime_error("Recomputed spectral products differ from saved recipe; create a fresh review");
        writeRecipe(directory / "recipe.txt", r);
        print(directory);
    } catch (...) {
        std::error_code ec;
        for (const auto& name : allProducts)
            fs::remove(directory / name, ec);
        for (const auto& name : dependencies)
            fs::remove(directory / name, ec);
        fs::remove(directory / "recipe.txt", ec);
        fs::remove(directory, ec);
        throw;
    }
}
} // namespace
int runSpectralCalibration(const std::vector<std::string>& args) {
    const bool fresh = args[0] == "spcc", inspect = args[0] == "spcc-inspect", replay = args[0] == "spcc-replay",
               exporting = args[0] == "spcc-export", display = args[0] == "spcc-preview";
    if (!fresh && !inspect && !replay && !exporting && !display)
        throw std::invalid_argument("Unknown spectral command");
    const std::set<std::string> freshKeys{"--input",        "--profile", "--catalog",   "--output-dir",
                                          "--expected-sha", "--radius",  "--sky-inner", "--sky-outer",
                                          "--saturation",   "--min-snr", "--exclude", "--max-center-offset",
                                          "--max-spectral-error", "--max-aperture-color-change", "--max-validation-log-error"};
    const std::set<std::string> savedKeys = inspect ? std::set<std::string>{"--recipe", "--expected-sha"}
                                            : replay
                                                ? std::set<std::string>{"--recipe", "--output-dir", "--expected-sha"}
                                                : display ? std::set<std::string>{"--recipe", "--output", "--expected-sha", "--full-resolution"}
                                                          : std::set<std::string>{"--recipe", "--output", "--expected-sha"};
    std::map<std::string, std::string> values;
    for (std::size_t i = 1; i < args.size(); i += 2)
        if (i + 1 >= args.size() || !(fresh ? freshKeys : savedKeys).contains(args[i]) ||
            !values.emplace(args[i], args[i + 1]).second)
            throw std::invalid_argument("Invalid or duplicate spectral option");
    if (fresh) {
        for (const auto* key : {"--input", "--profile", "--catalog", "--output-dir"})
            if (!values.contains(key))
                throw std::invalid_argument("Specify spectral --input, --profile, --catalog and --output-dir");
        Recipe r;
        r.source = fs::canonical(values.at("--input"));
        r.sha = fileSHA256(r.source);
        if (values.contains("--expected-sha") && r.sha != values.at("--expected-sha"))
            throw std::runtime_error("Spectral source changed");
        auto& a = r.options.aperture;
        const std::map<std::string, double*> numeric{{"--radius", &a.radius},
                                                     {"--sky-inner", &a.skyInner},
                                                     {"--sky-outer", &a.skyOuter},
                                                     {"--saturation", &a.saturation},
                                                     {"--min-snr", &a.minSNR},
                                                     {"--max-center-offset", &a.maxCenterOffset},
                                                     {"--max-spectral-error", &r.options.maxRelativeSpectralError},
                                                     {"--max-aperture-color-change", &r.options.maxApertureColorChange},
                                                     {"--max-validation-log-error", &r.options.maxValidationLogError}};
        for (const auto& [key, ptr] : numeric)
            if (values.contains(key)) {
                std::size_t n = 0;
                const auto& value = values.at(key);
                *ptr = std::stod(value, &n);
                if (n != value.size() || !std::isfinite(*ptr))
                    throw std::invalid_argument("Invalid spectral numeric value");
            }
        if (values.contains("--exclude") && !values.at("--exclude").empty()) {
            if (values.at("--exclude").back() == ',')
                throw std::invalid_argument("Invalid spectral exclusion list");
            std::istringstream ids(values.at("--exclude"));
            std::string id;
            while (std::getline(ids, id, ',')) {
                spectralID(id);
                if (!r.options.excluded.insert(id).second || r.options.excluded.size() > 10000)
                    throw std::invalid_argument("Duplicate or excessive spectral exclusions");
            }
        }
        run(r, fs::canonical(values.at("--profile")), fs::canonical(values.at("--catalog")), values.at("--output-dir"),
            false);
        return 0;
    }
    if (!values.contains("--recipe"))
        throw std::invalid_argument("Specify spectral --recipe");
    const fs::path path = fs::canonical(values.at("--recipe"));
    const auto r = readRecipe(path);
    if (values.contains("--expected-sha") && r.sha != values.at("--expected-sha"))
        throw std::runtime_error("Spectral recipe belongs to another source");
    if (values.contains("--full-resolution") && values.at("--full-resolution") != "0" && values.at("--full-resolution") != "1")
        throw std::invalid_argument("Spectral full-resolution must be 0 or 1");
    if (inspect) {
        print(path.parent_path());
        return 0;
    }
    if (replay) {
        if (!values.contains("--output-dir"))
            throw std::invalid_argument("Specify new spectral --output-dir");
        run(r, path.parent_path() / "profile.txt", path.parent_path() / "catalog.txt", values.at("--output-dir"), true);
        return 0;
    }
    if (!values.contains("--output") || !r.valid)
        throw std::invalid_argument("Export requires a validated spectral result and new --output");
    const fs::path output = values.at("--output");
    if (output.empty() || fs::exists(output) ||
        (display ? output.extension() != ".tiff" && output.extension() != ".tif" : !isFitsPath(output)))
        throw std::invalid_argument(display ? "Choose a new TIFF preview path" : "Choose a new FITS export path");
    const std::string product = display ? "after.tiff" : "calibrated.fits";
    bool copied = false;
    try {
        copied = fs::copy_file(path.parent_path() / product, output);
        if (!copied || fileSHA256(output) != r.products.at(product))
            throw std::runtime_error("Spectral export verification failed");
        if (display && values.contains("--full-resolution") && values.at("--full-resolution") == "1") {
            // Reproduce the saved comparison first. This binds the display
            // statistics and rendering algorithm without trusting new defaults
            // or silently restretching with another black reference.
            const auto bounds = [&] {
                const auto source = readLinearScienceImage(r.source);
                auto small = resized(source.image);
                const auto limits = range(small);
                preview(std::move(small), limits.first, limits.second, {1, 1, 1}, output);
                if (fileSHA256(output) != r.products.at("before.tiff"))
                    throw std::runtime_error("Saved spectral display differs; create a fresh review");
                return limits;
            }();
            auto calibrated = readLinearScienceImage(path.parent_path() / "calibrated.fits");
            preview(std::move(calibrated.image), bounds.first, bounds.second, r.gains, output);
        }
        verifySource(r);
        verify(path.parent_path(), r.dependencies);
        verify(path.parent_path(), r.products);
    } catch (...) {
        if (copied) {
            std::error_code ec;
            fs::remove(output, ec);
        }
        throw;
    }
    std::cout << "{\"type\":\"complete\",\"command\":" << channelJSON(args[0]) << ",\"output\":"
              << channelJSON(fs::absolute(output).string()) << "}\n";
    return 0;
}
