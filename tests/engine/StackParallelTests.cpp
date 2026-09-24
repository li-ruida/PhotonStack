#include "photonstack/FitsCodec.hpp"
#include "photonstack/Stacker.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>
#include <thread>

using namespace photonstack;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
void require(bool ok, const std::source_location at = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Parallel stack line " + std::to_string(at.line()));
}
void equal(const ImageBuffer& a, const ImageBuffer& b) {
    require(a.width == b.width && a.height == b.height && a.channels == b.channels);
    require(a.pixels.size() == b.pixels.size());
    require(a.pixels.empty() || std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size() * sizeof(float)) == 0);
}
void equal(const StackResult& a, const StackResult& b) {
    require(a.ok && b.ok);
    equal(a.image, b.image); equal(a.rejectionLow, b.rejectionLow); equal(a.rejectionHigh, b.rejectionHigh);
    equal(a.noiseDiagnostic, b.noiseDiagnostic);
    require(a.rejectedLowSamples == b.rejectedLowSamples);
    require(a.rejectedHighSamples == b.rejectedHighSamples);
    require(a.comparedSamples == b.comparedSamples);
}
int main(int argc, char**) {
    const bool benchmark = argc > 1;
    const auto root = fs::temp_directory_path() / ("photonstack-parallel-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    const auto cache = root / "cache";
    fs::create_directory(cache);
    const unsigned frameCount = benchmark ? 329 : 19, width = benchmark ? 320 : 131, height = benchmark ? 128 : 83;
    std::mt19937 rng(90231);
    std::normal_distribution<float> gaussian;
    std::vector<fs::path> paths;
    StackOptions options;
    options.temporaryDirectory = cache;
    options.method = StackMethod::SigmaClip;
    for (unsigned f = 0; f < frameCount; ++f) {
        ImageBuffer a;
        a.width = width; a.height = height; a.pixels.resize(a.sampleCount());
        for (std::size_t p = 0; p < a.pixelCount(); ++p) {
            for (unsigned c = 0; c < 3; ++c)
                a.pixels[p * 4 + c] = float(100 + p % 97) + gaussian(rng) * (1 + c) +
                    (f == 0 && p % 11 == 0 ? 1000 : 0) - (f == 3 && p % 13 == 0 ? 500 : 0);
            // Fully absent pixels, sparse edges and fractional coverage.
            a.pixels[p * 4 + 3] = p % 137 == 0 ? 0 : p % 19 == 0 && f > 3 ? 0 : f % 3 == 0 ? .5F : 1;
        }
        paths.push_back(root / (std::to_string(f) + ".fits"));
        require(FitsCodec().write(a, paths.back()).ok);
        options.frameWeights.push_back(float(1 + f % 5));
    }
    const auto caller = std::this_thread::get_id();
    double combineSeconds = 0;
    Clock::time_point combining;
    options.progress = [&](const StackProgress& p) {
        require(std::this_thread::get_id() == caller);
        if (p.stage == StackProgressStage::Combining) {
            if (p.row == 0) combining = Clock::now();
            if (p.row == p.rowCount) combineSeconds = std::chrono::duration<double>(Clock::now() - combining).count();
        }
    };
    if (benchmark) {
        options.sigma = {3, 3, true, 3};
        options.collectRejectionMaps = true;
        StackResult baseline;
        for (unsigned workers : {1, 8, 8, 1}) {
            options.combineWorkers = workers;
            const auto start = Clock::now();
            const auto result = Stacker().stack(paths, options);
            require(result.ok && fs::is_empty(cache));
            if (!baseline.ok) baseline = result; else equal(baseline, result);
            std::cout << "workers=" << workers << " combine=" << combineSeconds
                      << " total=" << std::chrono::duration<double>(Clock::now() - start).count()
                      << " bitwise_equal=true\n" << std::flush;
        }
        return 0;
    }
    for (const auto method : {StackMethod::Median, StackMethod::SigmaClip, StackMethod::WinsorizedSigmaClip, StackMethod::PercentileClip}) {
        options.method = method;
        for (unsigned mode = 0; mode < (method == StackMethod::SigmaClip ? 4U : 2U); ++mode) {
            options.sigma.medianMad = mode >= 2;
            options.collectRejectionMaps = mode % 2;
            options.collectNoiseDiagnostic = method == StackMethod::SigmaClip;
            options.frameNoiseVariances.clear();
            if (options.collectNoiseDiagnostic) for (unsigned f = 0; f < frameCount; ++f)
                options.frameNoiseVariances.push_back(1 + f % 7);
            options.combineWorkers = 1;
            const auto serial = Stacker().stack(paths, options);
            for (unsigned workers : {2, 8, 0}) {
                options.combineWorkers = workers;
                equal(serial, Stacker().stack(paths, options));
                require(fs::is_empty(cache));
            }
        }
    }
    // A UI cancellation/progress exception happens on its original caller and
    // cleans the cache after workers are joined; it must not strand a thread.
    options.method = StackMethod::SigmaClip;
    options.sigma.medianMad = true;
    options.progress = [&](const StackProgress& p) {
        require(std::this_thread::get_id() == caller);
        if (p.stage == StackProgressStage::Combining && p.row > 0) throw std::runtime_error("cancel-test");
    };
    bool cancelled = false;
    try { (void)Stacker().stack(paths, options); }
    catch (const std::runtime_error& e) { cancelled = std::string(e.what()) == "cancel-test"; }
    require(cancelled && fs::is_empty(cache));
    for (const auto& path : paths) require(fs::exists(path));

    // More than 64 MiB forces consecutive tile reads plus a short last tile.
    // Check against an analytic per-row signal, not another stack execution:
    // identical cursor mistakes in serial and parallel must not pass.
    std::vector<fs::path> largePaths;
    for (unsigned f = 0; f < 6; ++f) {
        ImageBuffer a;
        a.width = 1024; a.height = 701; a.pixels.resize(a.sampleCount());
        for (unsigned y = 0; y < a.height; ++y) for (unsigned x = 0; x < a.width; ++x) {
            const auto p = (std::size_t(y) * a.width + x) * 4;
            for (unsigned c = 0; c < 3; ++c) a.pixels[p + c] = float(y * 1000 + x) + .25F * c + f;
            a.pixels[p + 3] = 1;
        }
        largePaths.push_back(root / ("rows-" + std::to_string(f) + ".fits"));
        require(FitsCodec().write(a, largePaths.back()).ok);
    }
    StackOptions rowsOptions;
    rowsOptions.method = StackMethod::SigmaClip;
    rowsOptions.sigma = {100, 100, false, 3};
    rowsOptions.collectRejectionMaps = true;
    rowsOptions.temporaryDirectory = cache;
    const auto rowResult = Stacker().stack(largePaths, rowsOptions);
    require(rowResult.ok && fs::is_empty(cache));
    for (unsigned y = 0; y < 701; ++y) for (unsigned x = 0; x < 1024; ++x) {
        const auto p = (std::size_t(y) * 1024 + x) * 4;
        for (unsigned c = 0; c < 3; ++c) {
            require(rowResult.image.pixels[p + c] == float(y * 1000 + x) + .25F * c + 2.5F);
            require(rowResult.rejectionLow.pixels[p + c] == 0 && rowResult.rejectionHigh.pixels[p + c] == 0);
        }
        require(rowResult.image.pixels[p + 3] == 1);
    }
    for (const auto count : rowResult.comparedSamples) require(count == 1024 * 701 * 3);
}
