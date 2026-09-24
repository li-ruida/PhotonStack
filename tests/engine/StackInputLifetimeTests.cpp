#include "photonstack/FitsCodec.hpp"
#include "photonstack/Stacker.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <source_location>
#include <stdexcept>

using namespace photonstack;
namespace fs = std::filesystem;
void require(bool ok, const std::source_location at = std::source_location::current()) {
    if (!ok)
        throw std::runtime_error("Input lifetime line " + std::to_string(at.line()));
}

int main() {
    const auto root =
        fs::temp_directory_path() /
        ("photonstack-input-lifetime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{root};
    const auto cacheRoot = root / "cache";
    fs::create_directory(cacheRoot);
    std::vector<fs::path> paths;
    std::vector<ImageBuffer> frames;
    for (unsigned i = 0; i < 6; ++i) {
        ImageBuffer a;
        a.width = 160;
        a.height = 128;
        a.channels = 4;
        a.colorEncoding = ColorEncoding::Linear;
        a.pixels.resize(a.sampleCount());
        for (unsigned y = 0; y < a.height; ++y)
            for (unsigned x = 0; x < a.width; ++x) {
                double v = 10 * i;
                for (int sy = 16; sy <= 112; sy += 24)
                    for (int sx = 16; sx <= 136; sx += 24)
                        v += 2000 * std::exp(-(std::pow(double(x) - sx, 2) + std::pow(double(y) - sy, 2)) / 3);
                const auto p = (std::size_t(y) * a.width + x) * 4;
                for (int c = 0; c < 3; ++c)
                    a.pixels[p + c] = float(v * (1 + .1 * c));
                a.pixels[p + 3] = 1;
            }
        paths.push_back(root / (std::to_string(i) + ".fits"));
        frames.push_back(std::move(a));
    }
    auto restore = [&] {
        for (unsigned i = 0; i < paths.size(); ++i)
            require(FitsCodec().write(frames[i], paths[i]).ok);
    };
    restore();
    const auto external = root / "reference.fits";
    require(FitsCodec().write(frames.front(), external).ok);
    const std::vector methods{StackMethod::Average,   StackMethod::WeightedAverage,     StackMethod::Median,
                              StackMethod::SigmaClip, StackMethod::WinsorizedSigmaClip, StackMethod::PercentileClip};
    for (const auto method : methods)
        for (int referenceMode = 0; referenceMode < 3; ++referenceMode) {
            restore();
            StackOptions options;
            options.method = method;
            options.temporaryDirectory = cacheRoot;
            options.frameWeights = {1, 2, 3, 4, 5, 6};
            options.normalizeBackground = true;
            options.alignDistortion = referenceMode != 0;
            if (referenceMode == 2)
                options.alignmentReference = external;
            options.registration.minimumMatches = 8;
            options.registration.starDetection.minPeak = 0;
            options.registration.starDetection.sigmaThreshold = 1;
            const bool robust = method != StackMethod::Average && method != StackMethod::WeightedAverage;
            options.collectRejectionMaps = robust;
            if (method == StackMethod::SigmaClip) {
                options.collectNoiseDiagnostic = true;
                options.frameNoiseVariances = {1, 2, 1, 3, 1, 2};
            }
            const auto baseline = Stacker().stack(paths, options);
            require(baseline.ok);
            require(fs::is_empty(cacheRoot));
            std::vector<std::size_t> consumed;
            options.inputConsumed = [&](std::size_t index) {
                require(index < paths.size() && fs::exists(paths[index]));
                if (robust) {
                    std::size_t cached = 0;
                    for (const auto& item : fs::recursive_directory_iterator(cacheRoot))
                        if (item.is_regular_file()) {
                            require(item.file_size() == frames[0].sampleCount() * sizeof(float));
                            ++cached;
                        }
                    // Notification happens after the cache file is fully closed.
                    require(cached == consumed.size() + 1);
                }
                consumed.push_back(index);
                require(fs::remove(paths[index]));
            };
            const auto released = Stacker().stack(paths, options);
            require(released.ok && consumed.size() == paths.size());
            require(consumed.front() == (referenceMode == 1 ? 3 : 0));
            require(released.image.pixels == baseline.image.pixels);
            require(released.rejectionLow.pixels == baseline.rejectionLow.pixels);
            require(released.rejectionHigh.pixels == baseline.rejectionHigh.pixels);
            require(released.noiseDiagnostic.pixels == baseline.noiseDiagnostic.pixels);
            require(released.rejectedLowSamples == baseline.rejectedLowSamples);
            require(released.rejectedHighSamples == baseline.rejectedHighSamples);
            require(released.comparedSamples == baseline.comparedSamples);
            require(fs::exists(external) && fs::is_empty(cacheRoot));
        }
    // Single-input fast paths also deliver the index only after decoding.
    for (const auto method : methods) {
        restore();
        StackOptions o;
        o.method = method;
        unsigned calls = 0;
        o.inputConsumed = [&](std::size_t index) {
            require(index == 0);
            ++calls;
            require(fs::remove(paths[0]));
        };
        const auto result = Stacker().stack({paths[0]}, o);
        require(result.ok && calls == 1 && result.image.pixels == frames[0].pixels);
    }
    // Ambiguous repeated input paths must fail before a destructive callback.
    restore();
    StackOptions o;
    o.method = StackMethod::SigmaClip;
    o.temporaryDirectory = cacheRoot;
    unsigned calls = 0;
    o.inputConsumed = [&](std::size_t) { ++calls; };
    require(Stacker().stack({paths[0], paths[0]}, o).errorCode == "ArgumentInvalid" && calls == 0);
    const auto alias = root / "alias.fits";
    fs::create_symlink(paths[0], alias);
    require(Stacker().stack({paths[0], alias}, o).errorCode == "ArgumentInvalid" && calls == 0);
    // Failed decodes never report a consumed input. Earlier owned inputs may
    // already be gone, but both cancellation and errors clean the private cache.
    o.inputConsumed = [&](std::size_t index) {
        ++calls;
        require(index == 0);
        require(fs::remove(paths[index]));
    };
    const auto failed = Stacker().stack({paths[0], root / "missing.fits"}, o);
    require(!failed.ok && calls == 1 && fs::is_empty(cacheRoot));
    restore();
    calls = 0;
    o.inputConsumed = [&](std::size_t index) {
        ++calls;
        require(fs::remove(paths[index]));
        if (calls == 2)
            throw std::runtime_error("release error");
    };
    const auto callbackFailed = Stacker().stack(paths, o);
    require(callbackFailed.errorCode == "InputReleaseFailed" && calls == 2 && fs::is_empty(cacheRoot));
    require(fs::exists(paths[2]) && fs::exists(external));
    restore();
    calls = 0;
    o.inputConsumed = [&](std::size_t index) {
        ++calls;
        require(fs::remove(paths[index]));
    };
    bool obstructed = false;
    o.progress = [&](const StackProgress& p) {
        if (!obstructed && p.stage == StackProgressStage::Reading && p.frame == 2) {
            for (const auto& item : fs::directory_iterator(cacheRoot))
                if (item.is_directory())
                    require(fs::create_directory(item.path() / "frame-1.f32"));
            obstructed = true;
        }
    };
    const auto writeFailed = Stacker().stack(paths, o);
    require(obstructed && writeFailed.errorCode == "TemporaryStorageWriteFailed" && calls == 1);
    require(fs::exists(paths[1]) && fs::is_empty(cacheRoot));
    restore();
    calls = 0;
    o.inputConsumed = [&](std::size_t index) {
        ++calls;
        require(fs::remove(paths[index]));
    };
    o.progress = [&](const StackProgress& p) {
        if (p.stage == StackProgressStage::Reading && p.frame == 2)
            throw std::runtime_error("cancel");
    };
    bool cancelled = false;
    try {
        (void)Stacker().stack(paths, o);
    } catch (const std::runtime_error&) {
        cancelled = true;
    }
    require(cancelled && calls == 1 && fs::is_empty(cacheRoot) && fs::exists(paths[1]));
}
