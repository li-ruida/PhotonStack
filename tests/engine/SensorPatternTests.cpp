#include "photonstack/SensorPatternBuilder.hpp"
#include "photonstack/FileDigest.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <source_location>
#include <stdexcept>
using namespace photonstack;
namespace fs = std::filesystem;
void require(bool v, std::source_location l = std::source_location::current()) {
    if (!v) throw std::runtime_error("Sensor pattern test line " + std::to_string(l.line()));
}
void card(const fs::path& path, const std::string& key, const std::string& value) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    std::string c(80, ' '); std::streamoff at = 0;
    while (f.read(c.data(), 80)) { if (c.substr(0, 8) == "END     ") break; at += 80; }
    require(bool(f)); std::string added = key; added.resize(8, ' '); added += "= " + value; added.resize(80, ' ');
    f.seekp(at); f.write(added.data(), 80); f.write(c.data(), 80);
}
void write(const ImageBuffer& a, const fs::path& p) {
    require(FitsCodec().write(a, p).ok);
    card(p, "BAYERPAT", "'GRBG'"); card(p, "TELESCOP", "'synthetic-camera'");
    card(p, "EXPOSURE", "30."); card(p, "GAIN", "200");
}
int main() {
    const auto root = fs::temp_directory_path() / ("sensor-pattern-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup {fs::path p; ~Cleanup(){std::error_code e;fs::remove_all(p,e);}} cleanup{root};
    const auto digestTest=root/"digest.txt";
    { std::ofstream f(digestTest); }
    require(fileSHA256(digestTest)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    { std::ofstream f(digestTest); f << "abc"; }
    require(fileSHA256(digestTest)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    constexpr unsigned w = 128, h = 128, frames = 36;
    SensorPatternOptions options; options.minimumGroupSamples = 8; options.border = 12; options.temporaryDirectory = root;
    std::vector<fs::path> paths; std::vector<ImageBuffer> raws; std::vector<std::vector<float>> truth;
    std::mt19937 gen(67832); std::normal_distribution<float> noise(0, 2), patternNoise(0, 6);
    std::vector<float> pattern(w * h); for (auto& v : pattern) v = patternNoise(gen);
    for (unsigned f = 0; f < frames; ++f) {
        const double dx = int((f * 17) % 81) - 40, dy = int((f * 29) % 81) - 40;
        options.sensorPositions.push_back({dx, dy});
        ImageBuffer a; a.width = w; a.height = h; a.channels = 1; a.format = PixelFormat::Float32Gray; a.pixels.resize(w * h);
        std::vector<float> scene(w * h);
        for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
            const double u = x - dx, v = y - dy;
            scene[y*w+x] = float(-30 + .1 * u + .2 * v + 20 * std::exp(-(std::pow(u-60,2)+std::pow(v-60,2))/1200) +
                150 * std::exp(-(std::pow(u-50,2)+std::pow(v-50,2))/8));
            a.pixels[y*w+x] = scene[y*w+x] + pattern[y*w+x] + noise(gen);
        }
        paths.push_back(root/(std::to_string(f)+".fits"));write(a,paths.back());raws.push_back(a);truth.push_back(std::move(scene));
    }
    const auto sensor = FitsCodec().inspectSensor(paths[0]);
    require(sensor.ok && sensor.bayer == "GRBG" && sensor.gain == 200 && sensor.exposure == 30);
    card(paths[0], "XBAYROFF", "-2"); card(paths[0], "YBAYROFF", "4");
    require(sensor.sameAcquisition(FitsCodec().inspectSensor(paths[0])));
    double previous = 0;
    options.progress = [&](double p) {require(p >= previous && p <= 1);previous=p;};
    const auto result = SensorPatternBuilder().build(paths, options);
    if (!result.ok) throw std::runtime_error(result.message);
    require(previous == 1 && result.sources.size() == frames);
    double original = 0, corrected = 0; unsigned samples = 0;
    for (unsigned f = 0; f < frames; ++f) {
        const auto& model = result.models[f % 3];
        require(model.excludedFold == f % 3 && model.trainingIndices.size() == 24);
        for (auto i : model.trainingIndices) require(i % 3 != f % 3);
        for (unsigned y = 20; y < h-20; ++y) for (unsigned x = 20; x < w-20; ++x) {
            const auto p = y*w+x;if(model.validity.pixels[p]!=1)continue;
            const double error = raws[f].pixels[p] - truth[f][p];
            original += error*error;corrected += std::pow(error-model.correction.pixels[p],2);++samples;
        }
    }
    std::cout << "held-out synthetic error ratio " << corrected/original << " samples " << samples << '\n';
    require(samples > 150000 && corrected/original < .4);
    for (const auto& model : result.models) {
        require(model.correction.channels == 1 && model.correction.colorEncoding == ColorEncoding::Linear);
        require(model.correction.pixels[0] == 0 && model.validity.pixels[0] == 0);
        for (auto c : model.correlation) require(c > .7);
    }
    // Named sensor correction applies signed physical samples before debayering;
    // it must not be disguised as bias calibration or clipped to display range.
    FitsDecodeOptions decode; decode.mode = FitsDecodeMode::Scientific; decode.debayer = false;
    const auto read = FitsCodec().read(paths[0], decode, nullptr, &result.models[0].correction);
    require(read.ok);
    for (unsigned p = 0; p < w*h; ++p) {
        require(read.image.pixels[p*4] == raws[0].pixels[p] - result.models[0].correction.pixels[p]);
        require(read.image.pixels[p*4+3] == 1);
    }
    auto invalid = result.models[0].correction; invalid.colorEncoding = ColorEncoding::SRGB;
    require(!FitsCodec().read(paths[0], decode, nullptr, &invalid).ok);
    CalibrationOptions other; other.bias = &read.image;
    require(!FitsCodec().read(paths[0], decode, &other, &result.models[0].correction).ok);
    // Compare against an independently corrected raw CFA file: this catches
    // accidentally applying the model after interpolation instead of before it.
    auto correctedRaw = raws[0];
    for (unsigned p = 0; p < w*h; ++p)
        correctedRaw.pixels[p] -= result.models[0].correction.pixels[p];
    const auto expectedPath = root / "expected-corrected.fits";
    write(correctedRaw, expectedPath);
    decode.debayer = true;
    const auto expectedRGB = FitsCodec().read(expectedPath, decode);
    const auto correctedRGB = FitsCodec().read(paths[0], decode, nullptr, &result.models[0].correction);
    require(expectedRGB.ok && correctedRGB.ok);
    require(expectedRGB.image.pixels == correctedRGB.image.pixels);
    invalid = result.models[0].correction;
    invalid.pixels[w*30+30] = std::numeric_limits<float>::quiet_NaN();
    require(!FitsCodec().read(paths[0], decode, nullptr, &invalid).ok);
    // Exactly the documented minimum remains useful: smoothing support must
    // not silently turn a valid small acquisition into an all-zero correction.
    auto exactMinimum = options;
    exactMinimum.minimumGroupSamples = frames / 3;
    exactMinimum.progress = {};
    const auto minimumModel = SensorPatternBuilder().build(paths, exactMinimum);
    require(minimumModel.ok);
    bool correctedAtMinimum = false;
    for (const auto& model : minimumModel.models)
        for (float value : model.correction.pixels) correctedAtMinimum |= value != 0;
    require(correctedAtMinimum);
    // Changing every sample of the excluded fold must not alter its model.
    for (unsigned f = 0; f < frames; f += 3) {
        for (unsigned p = 0; p < w*h; ++p) raws[f].pixels[p] += (p % 7 < 3 ? 13 : -13);
        write(raws[f],paths[f]);
    }
    options.progress = {};
    const auto changed = SensorPatternBuilder().build(paths, options);require(changed.ok);
    require(changed.models[0].correction.pixels == result.models[0].correction.pixels);
    require(changed.models[0].validity.pixels == result.models[0].validity.pixels);
    require(changed.models[0].retention == result.models[0].retention);
    require(changed.models[1].correction.pixels != result.models[1].correction.pixels);
    // Acquisition mismatches, repeated samples and insufficient motion fail.
    card(paths.back(),"GAIN","201");require(!SensorPatternBuilder().build(paths,options).ok);write(raws.back(),paths.back());
    auto dup=paths;dup.back()=dup.front();require(!SensorPatternBuilder().build(dup,options).ok);
    auto bad=options;bad.sensorPositions.assign(frames,{0,0});require(!SensorPatternBuilder().build(paths,bad).ok);
    bad=options;bad.progress=[](double p){if(p>.01)throw std::runtime_error("cancelled");};
    const auto cancelled=SensorPatternBuilder().build(paths,bad);require(!cancelled.ok && cancelled.models[0].correction.empty());
    for(const auto& entry:fs::directory_iterator(root))require(!entry.is_directory());
    // No common pattern: independent random noise must not produce a correction.
    for(unsigned f=0;f<frames;++f){for(auto&v:raws[f].pixels)v=100+noise(gen);write(raws[f],paths[f]);}
    const auto null=SensorPatternBuilder().build(paths,options);require(null.ok);
    for(const auto&m:null.models)for(float v:m.correction.pixels)require(v==0);
    // Missing metadata is not silently inferred, and odd sensor geometry is rejected.
    require(FitsCodec().write(raws[0],paths[0]).ok);require(!FitsCodec().inspectSensor(paths[0]).ok);
    require(!SensorPatternBuilder().build(paths,options).ok);
    auto odd = raws[0]; odd.width = w-1; odd.pixels.resize(odd.width*odd.height);
    for (const auto& p : paths) write(odd,p);
    require(!SensorPatternBuilder().build(paths,options).ok);
}
