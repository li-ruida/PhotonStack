#include "BackgroundReviewCommand.hpp"
#include "photonstack/BackgroundExtractor.hpp"
#include "photonstack/FileDigest.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/ImageResizer.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace photonstack;
void check(const ImageWriteResult& value) {
    if (!value.ok)
        throw std::runtime_error(value.message);
}
struct Directory {
    fs::path path;
    bool committed = false;
    explicit Directory(fs::path p) : path(std::move(p)) {
        if (!fs::create_directory(path))
            throw std::invalid_argument("Choose a new review output directory");
    }
    ~Directory() {
        if (!committed) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};
double parse(const std::string& text, double low, double high) {
    std::size_t n = 0;
    const double value = std::stod(text, &n);
    if (n != text.size() || !std::isfinite(value) || value < low || value > high)
        throw std::invalid_argument("Invalid background review option");
    return value;
}
ImageBuffer proxy(const ImageBuffer& im) {
    const unsigned width =
        std::min<unsigned>(im.width, std::max(1.0, std::round(1200.0 * im.width / std::max(im.width, im.height))));
    auto r = ImageResizer().resizeToWidth(im, width);
    if (!r.ok)
        throw std::runtime_error(r.message);
    return std::move(r.image);
}
double percentile(std::vector<double> values, double fraction) {
    if (values.empty())
        throw std::invalid_argument("The image has no covered pixels");
    const auto rank = std::size_t(fraction * (values.size() - 1));
    std::nth_element(values.begin(), values.begin() + rank, values.end());
    return values[rank];
}
std::pair<double, double> limits(const ImageBuffer& image) {
    std::vector<double> v;
    const auto step = std::max<std::size_t>(1, image.pixelCount() / 100000);
    for (std::size_t p = 0; p < image.pixelCount(); p += step)
        if (image.pixels[p * 4 + 3] > .99)
            for (int c = 0; c < 3; ++c)
                if (std::isfinite(image.pixels[p * 4 + c]))
                    v.push_back(image.pixels[p * 4 + c]);
    return {percentile(v, .005), percentile(v, .995)};
}
void preview(const ImageBuffer& im, const fs::path& path, double black, double white, int mode = 0) {
    auto small = proxy(im);
    const double scale = std::max(white - black, 1e-12);
    for (std::size_t p = 0; p < small.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c) {
            const auto i = p * 4 + c;
            const double v = mode == 2 ? .5 + .45 * small.pixels[i] / std::max(white, 1e-12)
                             : mode == 1
                                 ? (small.pixels[i] - black) / scale
                                 : std::asinh(10 * std::max(0.0, (small.pixels[i] - black) / scale)) / std::asinh(10.);
            small.pixels[i] = float(std::clamp(v, 0., 1.));
        }
    // A signed residual uses display-code midgray for exactly zero.
    if (mode == 2)
        small.colorEncoding = ColorEncoding::SRGB;
    ImageWriteOptions opts;
    opts.bitDepth = ImageWriteBitDepth::Sixteen;
    check(ImageCodec().write(small, path, opts));
}
void array(std::ostream& out, const std::array<double, 3>& values) {
    out << "[" << values[0] << "," << values[1] << "," << values[2] << "]";
}
} // namespace
int runBackgroundReview(const std::vector<std::string>& args) {
    std::map<std::string, std::string> opts;
    for (std::size_t i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size() ||
            !std::set<std::string>{"--input", "--output-dir", "--samples", "--degree", "--strength",
                                   "--preserve-brightness", "--expected-sha", "--inspect-only", "--model",
                                   "--rbf-smoothing", "--rbf-reject"}
                 .contains(args[i]) ||
            !opts.emplace(args[i], args[i + 1]).second)
            throw std::invalid_argument("Unknown, duplicate or incomplete review option");
    }
    if (!opts.contains("--input") || !opts.contains("--output-dir"))
        throw std::invalid_argument("Specify --input and a new --output-dir");
    const auto value = [&](const char* key, const char* initial) {
        return opts.contains(key) ? opts.at(key) : std::string(initial);
    };
    const auto model = value("--model", "polynomial");
    if (model != "polynomial" && model != "rbf")
        throw std::invalid_argument("--model must be polynomial or rbf");
    const double smoothing = parse(value("--rbf-smoothing", "0.001"), 0, 1);
    const auto reject = value("--rbf-reject", "on");
    if (reject != "on" && reject != "off")
        throw std::invalid_argument("--rbf-reject requires on or off");
    if (model == "polynomial" && (opts.contains("--rbf-smoothing") || opts.contains("--rbf-reject")))
        throw std::invalid_argument("RBF options require --model rbf");
    BackgroundPolynomialOptions opt;
    opt.degree = unsigned(parse(value("--degree", "2"), 1, 4));
    if (parse(value("--degree", "2"), 1, 4) != opt.degree)
        throw std::invalid_argument("Polynomial degree must be an integer");
    opt.extraction.strength = float(parse(value("--strength", "0.35"), 0, 1));
    const auto preserve = value("--preserve-brightness", "on"), inspect = value("--inspect-only", "off");
    if ((preserve != "on" && preserve != "off") || (inspect != "on" && inspect != "off"))
        throw std::invalid_argument("Expected on or off");
    opt.extraction.preserveBrightness = preserve == "on";
    opt.extraction.clampOutput = false;
    opt.diagnostics = true;
    opt.manualSamples = true;
    const fs::path source = opts.at("--input");
    const auto sha = fileSHA256(source);
    if (sha.empty() || (opts.contains("--expected-sha") && opts.at("--expected-sha") != sha))
        throw std::invalid_argument("The source image changed since this background recipe was created");
    ImageReadOptions read;
    read.fits.mode = FitsDecodeMode::Scientific;
    read.fits.debayer = true;
    auto decoded = ImageCodec().read(source, read);
    if (!decoded.ok)
        throw std::runtime_error(decoded.message);
    auto& im = decoded.image;
    if (im.channels != 4 || im.width < 16 || im.height < 16)
        throw std::invalid_argument("Expected an image at least 16 by 16 pixels");
    for (std::size_t p = 0; p < im.pixelCount(); ++p) {
        auto* v = &im.pixels[p * 4];
        if (!std::isfinite(v[3]))
            throw std::invalid_argument("Invalid image coverage");
        for (int c = 0; c < 3; ++c) {
            if (v[3] <= 0)
                v[c] = 0;
            if (!std::isfinite(v[c]))
                throw std::invalid_argument("Nonfinite covered image sample");
            if (im.colorEncoding == ColorEncoding::SRGB)
                v[c] = v[c] <= .04045F ? v[c] / 12.92F : std::pow((v[c] + .055F) / 1.055F, 2.4F);
        }
    }
    im.colorEncoding = ColorEncoding::Linear;
    if (opts.contains("--samples")) {
        if (fs::file_size(opts.at("--samples")) > 262144)
            throw std::invalid_argument("Sample file exceeds 256 KiB");
        std::ifstream file(opts.at("--samples"));
        std::string line;
        if (!std::getline(file, line) || line != "PhotonStack-BackgroundSamples-1")
            throw std::invalid_argument("Invalid sample file version");
        while (std::getline(file, line)) {
            if (line.empty())
                continue;
            std::istringstream row(line);
            BackgroundSamplePoint p;
            int enabled;
            std::string extra;
            if (!(row >> p.x >> p.y >> p.radius >> enabled) || (row >> extra) || (enabled != 0 && enabled != 1) ||
                opt.samples.size() >= 4096)
                throw std::invalid_argument("Invalid sample row: expected x y radius enabled");
            p.enabled = enabled != 0;
            opt.samples.push_back(p);
        }
    } else {
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 12; ++x)
                opt.samples.push_back(
                    {std::floor((x + .5) * im.width / 12), std::floor((y + .5) * im.height / 8), 4, true});
    }
    for (const auto& point : opt.samples) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0 ||
            point.x > im.width - 1 || point.y > im.height - 1 || point.radius < 1 || point.radius > 64)
            throw std::invalid_argument("Sample centers must be inside the image and radius must be 1...64 pixels");
    }
    const auto [black, white] = limits(im);
    BackgroundExtractionResult result;
    if (inspect == "on") {
        for (const auto& p : opt.samples) {
            BackgroundSampleMeasurement m;
            m.point = p;
            m.status = "pending";
            result.measurements.push_back(m);
        }
    } else {
        if (model == "rbf") {
            BackgroundRBFOptions rbf;
            rbf.sampling = opt;
            rbf.smoothing = smoothing;
            rbf.rejectOutliers = reject == "on";
            result = BackgroundExtractor().extractRBF(im, rbf);
        } else
            result = BackgroundExtractor().extractPolynomial(im, opt);
        if (!result.ok)
            throw std::invalid_argument(result.errorCode + ": " + result.message);
    }
    Directory out(opts.at("--output-dir"));
    preview(im, out.path / "source.tiff", black, white);
    bool wcsPreserved = false;
    std::optional<SolvedPanel> sky;
    if (isFitsPath(source)) {
        try {
            sky = readSolvedPanel(source);
        } catch (const std::exception&) {
        }
    }
    if (inspect != "on") {
        const auto write = [&](const ImageBuffer& image, const char* name) {
            check(FitsCodec().write(image, out.path / name, sky ? &sky->grid : nullptr,
                                    sky ? &sky->distortion : nullptr));
        };
        write(result.image, "corrected.fits");
        write(result.modelImage, "model.fits");
        write(result.differenceImage, "difference.fits");
        wcsPreserved = sky.has_value();
        preview(result.image, out.path / "corrected.tiff", black, white);
        const auto [lo, hi] = limits(result.modelImage);
        preview(result.modelImage, out.path / "model.tiff", lo, hi, 1);
        double extent = 0;
        for (std::size_t p = 0; p < im.pixelCount(); ++p)
            if (im.pixels[p * 4 + 3] > 0)
                for (int c = 0; c < 3; ++c)
                    extent = std::max(extent, std::abs(double(result.differenceImage.pixels[p * 4 + c])));
        preview(result.differenceImage, out.path / "difference.tiff", 0, extent, 2);
    }
    std::ofstream report(out.path / "report.json");
    report << std::setprecision(17) << "{\"version\":2,\"width\":" << im.width << ",\"height\":" << im.height
           << ",\"sourceSHA\":\"" << sha << "\",\"degree\":" << opt.degree
           << ",\"strength\":" << opt.extraction.strength
           << ",\"preserveBrightness\":" << (opt.extraction.preserveBrightness ? "true" : "false")
           << ",\"wcsPreserved\":" << (wcsPreserved ? "true" : "false") << ",\"residualRMS\":";
    array(report, result.residualRMS);
    report << ",\"model\":\"" << model << "\",\"rbfSmoothing\":" << smoothing
           << ",\"rbfRejectOutliers\":" << (reject == "on" ? "true" : "false") << ",\"validationRMS\":";
    array(report, result.validationRMS);
    report << ",\"samples\":[";
    for (std::size_t i = 0; i < result.measurements.size(); ++i) {
        if (i)
            report << ",";
        const auto& m = result.measurements[i];
        const auto& p = m.point;
        report << "{\"x\":" << p.x << ",\"y\":" << p.y << ",\"radius\":" << p.radius
               << ",\"enabled\":" << (p.enabled ? "true" : "false") << ",\"status\":\"" << m.status
               << "\",\"measured\":";
        array(report, m.measured);
        report << ",\"fitted\":";
        array(report, m.fitted);
        report << ",\"validationResidual\":";
        array(report, m.validationResidual);
        report << ",\"used\":[" << (m.used[0] ? "true" : "false") << "," << (m.used[1] ? "true" : "false") << ","
               << (m.used[2] ? "true" : "false") << "]}";
    }
    report << "]}\n";
    report.close();
    if (!report)
        throw std::runtime_error("Could not save background review report");
    out.committed = true;
    std::cout << "{\"type\":\"complete\",\"command\":\"background-review\"}\n";
    return 0;
}
