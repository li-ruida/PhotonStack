#include "photonstack/DeepSkyWorkflow.hpp"
#include "photonstack/FileDigest.hpp"
#include "photonstack/FitsCodec.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>
using namespace photonstack;
namespace fs = std::filesystem;
void require(bool v, std::source_location l = std::source_location::current()) {
    if (!v)
        throw std::runtime_error("Sensor workflow line " + std::to_string(l.line()));
}
void card(const fs::path& p, const std::string& key, const std::string& value) {
    std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
    std::string c(80, ' ');
    std::streamoff at = 0;
    while (f.read(c.data(), 80)) {
        if (c.substr(0, 8) == "END     ")
            break;
        at += 80;
    }
    std::string s = key;
    s.resize(8, ' ');
    s += "= " + value;
    s.resize(80, ' ');
    f.seekp(at);
    f.write(s.data(), 80);
    f.write(c.data(), 80);
    require(bool(f));
}
int main(int argc, char** argv) {
    const bool fixture = argc == 3 && std::string(argv[1]) == "--fixture";
    require(argc == 1 || fixture);
    auto root =
        fixture
            ? fs::path(argv[2])
            : fs::temp_directory_path() /
                  ("sensor-workflow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(!fs::exists(root));
    fs::create_directories(root);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            if (!p.empty()) {
                std::error_code e;
                fs::remove_all(p, e);
            }
        }
    } cleanup{fixture ? fs::path{} : root};
    constexpr unsigned w = 256, h = 256, n = 36;
    std::mt19937 gen(17421);
    std::normal_distribution<float> noise(0, 1), fixed(0, 4);
    std::vector<float> pattern(w * h);
    for (auto& v : pattern)
        v = fixed(gen);
    std::vector<std::array<double, 3>> stars;
    for (unsigned i = 0; i < 35; ++i)
        stars.push_back({40. + gen() % 176, 40. + gen() % 176, 200. + gen() % 500});
    std::vector<fs::path> paths;
    std::vector<std::string> hashes;
    for (unsigned f = 0; f < n; ++f) {
        double dx = int((f * 17) % 49) - 24, dy = int((f * 29) % 49) - 24;
        ImageBuffer a;
        a.width = w;
        a.height = h;
        a.channels = 1;
        a.format = PixelFormat::Float32Gray;
        a.pixels.resize(w * h);
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                double u = x - dx, v = y - dy,
                       s = 100 + .015 * u + .02 * v +
                           15 * std::exp(-(std::pow(u - 130, 2) + std::pow(v - 130, 2)) / 1800.);
                for (const auto& t : stars)
                    s += t[2] * std::exp(-(std::pow(u - t[0], 2) + std::pow(v - t[1], 2)) / 8.);
                a.pixels[y * w + x] = float(s) + pattern[y * w + x] + noise(gen);
            }
        auto p = root / (std::to_string(f) + ".fits");
        require(FitsCodec().write(a, p).ok);
        card(p, "BAYERPAT", "'GRBG'");
        card(p, "TELESCOP", "'test-camera'");
        card(p, "EXPOSURE", "30.");
        card(p, "GAIN", "200");
        paths.push_back(p);
        hashes.push_back(fileSHA256(p));
    }
    if (fixture) {
        std::cout << "Generated 36 synthetic CFA fixtures in " << root << '\n';
        return 0;
    }
    DeepSkyWorkflowOptions o;
    o.sensorPattern = true;
    o.referenceIndex = 0;
    o.quality.measureStars = false;
    o.quality.measureTrails = false;
    o.quality.applySelection = false;
    o.stack.registration.minimumMatches = 6;
    o.background = DeepSkyBackground::None;
    o.alignChannels = false;
    o.multiscale.luminance = 0;
    o.multiscale.chroma = 0;
    o.develop.stellarBalance = false;
    o.develop.toneScale = 10;
    o.develop.whitePoint = 700;
    o.cropCoverage = 0;
    double last = 0;
    auto r = DeepSkyWorkflow().run(paths, root / "corrected", o,
                                   [&](const std::string&, double p, std::size_t, std::size_t) {
                                       require(p >= last);
                                       last = p;
                                   });
    if (!r.ok)
        throw std::runtime_error(r.errorCode + ": " + r.message);
    require(r.selectedCount == n && last == 1 && fs::exists(r.sensorPatternReport));
    require(!fs::exists(root / "corrected/.aligned-work"));
    for (unsigned i = 0; i < n; ++i) {
        require(r.frames[i].sha256 == hashes[i]);
        require(r.frames[i].sensorPatternModel == int(i % 3));
        require(fileSHA256(paths[i]) == hashes[i]);
    }
    // No sensor correction is allowed to run on an already integrated master.
    const auto finish = DeepSkyWorkflow().finishMaster(r.master, root / "invalid-finish", o);
    require(!finish.ok && finish.errorCode == "SensorPatternConflict");
    // Keep the exact motion/selection configuration; sensor mode alone changes samples.
    o.sensorPattern = false;
    const auto baseline = DeepSkyWorkflow().run(paths, root / "baseline", o);
    require(baseline.ok && baseline.sensorPatternReport.empty());
    require(fileSHA256(r.master) != fileSHA256(baseline.master));
    for (unsigned i = 0; i < n; ++i) {
        require(r.frames[i].registration.transform.dx == baseline.frames[i].registration.transform.dx);
        require(r.frames[i].assessment.selected == baseline.frames[i].assessment.selected);
    }
    o.sensorPattern = true;
    o.biases = {paths[0]};
    auto conflict = DeepSkyWorkflow().run(paths, root / "conflict", o);
    require(!conflict.ok && conflict.errorCode == "SensorPatternConflict" && !fs::exists(root / "conflict"));
    o.biases.clear();
    auto duplicate = paths;
    duplicate.back() = root / "copy.fits";
    fs::copy_file(paths[0], duplicate.back());
    auto dup = DeepSkyWorkflow().run(duplicate, root / "duplicate", o);
    require(!dup.ok && dup.errorCode == "DuplicateInput" && !fs::exists(root / "duplicate/.aligned-work"));
    bool mutated = false;
    auto changed = DeepSkyWorkflow().run(
        paths, root / "changed", o, [&](const std::string& stage, double p, std::size_t, std::size_t) {
            if (stage == "sensor-pattern-estimate" && p > .56 && !mutated) {
                auto time = fs::last_write_time(paths[0]);
                std::fstream f(paths[0], std::ios::in | std::ios::out | std::ios::binary);
                f.seekp(2880 + 100);
                char b = 1;
                f.write(&b, 1);
                f.close();
                fs::last_write_time(paths[0], time);
                mutated = true;
            }
        });
    require(mutated && !changed.ok && changed.errorCode == "InputChanged" &&
            !fs::exists(root / "changed/.aligned-work"));
    std::cout << "sensor workflow replay, source binding and cleanup passed\n";
}
