#include "CalibrationMetadataCommand.hpp"
#include "ChannelRecipe.hpp"
#include "NewProductDirectory.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/SensorQuality.hpp"
#include <cmath>
#include <iostream>

int runSensorQuality(const std::vector<std::string>& args) {
    using namespace photonstack;
    using namespace photonstack::cli;
    std::map<std::string, std::string> options;
    const std::set<std::string> allowed{"--input", "--output-dir", "--sigma", "--absolute-tolerance", "--flat-min", "--flat-max", "--expected-sha"};
    for (std::size_t i = 1; i < args.size(); i += 2)
        if (i + 1 >= args.size() || !allowed.contains(args[i]) || !options.emplace(args[i], args[i+1]).second)
            throw std::invalid_argument("Invalid sensor quality option");
    if (!options.contains("--input") || !options.contains("--output-dir")) throw std::invalid_argument("Choose a master and new output directory");
    SensorQualityOptions settings;
    const auto number = [&](const char* name, double& value) {
        if (!options.contains(name)) return;
        std::size_t used = 0; value = std::stod(options.at(name), &used);
        if (used != options.at(name).size() || !std::isfinite(value)) throw std::invalid_argument("Invalid quality threshold");
    };
    number("--sigma", settings.sigma); number("--absolute-tolerance", settings.absoluteTolerance);
    number("--flat-min", settings.minimumFlatResponse); number("--flat-max", settings.maximumFlatResponse);
    const auto result = inspectSensorQuality({options.at("--input"), options.contains("--expected-sha") ? options.at("--expected-sha") : ""}, settings);
    NewProductDirectory product(options.at("--output-dir"));
    writeSensorDefectMap(result, product.stage / "bad-pixels.fits");
    const auto preview = FitsCodec().write(result.badPixels, product.stage / "preview.fits");
    if (!preview.ok) throw std::runtime_error(preview.message);
    ImageBuffer display;
    const auto factor = std::max<std::uint32_t>(1, (std::max(result.badPixels.width, result.badPixels.height) + 1023) / 1024);
    display.width = (result.badPixels.width + factor - 1) / factor;
    display.height = (result.badPixels.height + factor - 1) / factor;
    display.channels = 4; display.colorEncoding = ColorEncoding::SRGB; display.pixels.resize(display.sampleCount(), 0);
    for (std::size_t p = 0; p < display.pixelCount(); ++p) display.pixels[p * 4 + 3] = 1;
    for (std::uint32_t y = 0; y < result.badPixels.height; ++y) for (std::uint32_t x = 0; x < result.badPixels.width; ++x)
        if (result.badPixels.pixels[std::size_t(y) * result.badPixels.width + x] == 1) {
            const auto p = (std::size_t(y / factor) * display.width + x / factor) * 4;
            display.pixels[p] = display.pixels[p + 1] = display.pixels[p + 2] = 1;
        }
    const auto png = ImageCodec().write(display, product.stage / "preview.png");
    if (!png.ok) throw std::runtime_error(png.message);
    std::ostringstream report; report << std::setprecision(17);
    report << "{\"schema\":1,\"type\":\"complete\",\"command\":\"sensor-quality\",\"source\":" << channelJSON(result.source.path.string())
           << ",\"sourceSHA\":" << channelJSON(result.source.sha) << ",\"role\":" << channelJSON(result.role)
           << ",\"width\":" << result.acquisition.width << ",\"height\":" << result.acquisition.height
           << ",\"rejected\":" << result.rejected << ",\"sigma\":" << settings.sigma
           << ",\"absoluteTolerance\":" << settings.absoluteTolerance << ",\"flatMin\":" << settings.minimumFlatResponse
           << ",\"flatMax\":" << settings.maximumFlatResponse << ",\"candidateOnly\":true,\"backend\":\"cpu\",\"phases\":[";
    for (std::size_t i = 0; i < result.phases.size(); ++i) {
        const auto& p = result.phases[i];
        report << (i ? "," : "") << "{\"median\":" << p.median << ",\"madSigma\":" << p.madSigma
               << ",\"minimum\":" << p.minimum << ",\"maximum\":" << p.maximum << ",\"finite\":" << p.finite
               << ",\"invalid\":" << p.invalid << ",\"low\":" << p.low << ",\"high\":" << p.high << '}';
    }
    report << "],\"products\":{\"bad-pixels.fits\":" << channelJSON(fileSHA256(product.stage / "bad-pixels.fits"))
           << ",\"preview.fits\":" << channelJSON(fileSHA256(product.stage / "preview.fits"))
           << ",\"preview.png\":" << channelJSON(fileSHA256(product.stage / "preview.png")) << "}}\n";
    std::ofstream file(product.stage / "report.json"); file << report.str(); file.close();
    if (!file || fileSHA256(result.source.path) != result.source.sha) throw std::runtime_error("Cannot bind quality report to source");
    product.publish(); std::cout << report.str(); return 0;
}
