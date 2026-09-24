#include "photonstack/FitsCodec.hpp"
#include "photonstack/Stacker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <source_location>
#include <stdexcept>

using namespace photonstack;
namespace fs = std::filesystem;
void require(bool ok, const std::source_location at = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Accepted group check at line " + std::to_string(at.line()));
}
void unchanged(const StackResult& a, const StackResult& b) {
    require(a.ok && b.ok);
    require(a.image.pixels == b.image.pixels);
    require(a.rejectionLow.pixels == b.rejectionLow.pixels);
    require(a.rejectionHigh.pixels == b.rejectionHigh.pixels);
    require(a.rejectedLowSamples == b.rejectedLowSamples);
    require(a.rejectedHighSamples == b.rejectedHighSamples);
    require(a.comparedSamples == b.comparedSamples);
    require(a.noiseDiagnostic.pixels == b.noiseDiagnostic.pixels);
}
void pool(const StackResult& result) {
    for (std::size_t p = 0; p < result.image.pixelCount(); ++p) {
        for (unsigned c = 0; c < 3; ++c) {
            double numerator = 0, denominator = 0;
            for (unsigned g = 0; g < 2; ++g) {
                const double w = result.acceptedGroupWeights[g].pixels[p * 4 + c];
                numerator += result.acceptedGroupImages[g].pixels[p * 4 + c] * w;
                denominator += w;
            }
            const double value = denominator > 0 ? numerator / denominator : 0;
            const double expected = result.image.pixels[p * 4 + c];
            require(std::abs(value - expected) < 2e-6 * std::max(1., std::abs(expected)));
        }
    }
}
int main() {
    const auto root = fs::temp_directory_path() / ("photonstack-accepted-groups-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    std::vector<fs::path> paths;
    const std::vector<std::uint8_t> labels{0,0,0,1,1,0,0,0};
    std::vector<ImageBuffer> frames;
    StackOptions options;
    options.method = StackMethod::SigmaClip;
    options.collectRejectionMaps = options.collectNoiseDiagnostic = true;
    options.frameNoiseVariances.assign(8, 1);
    for (unsigned f = 0; f < labels.size(); ++f) {
        ImageBuffer a; a.width = 160; a.height = 128; a.pixels.resize(a.sampleCount());
        options.frameWeights.push_back(float(1 + f % 3));
        for (std::size_t p = 0; p < a.pixelCount(); ++p) {
            for (unsigned c = 0; c < 3; ++c) {
                float value = float(10 + c);
                if (p % 7 == 1 && f == 7) value = c == 1 ? -1000 : 1000;
                if (p % 7 == 2 && labels[f] == 1) value = 1000;
                if (p % 7 == 4 && labels[f] == 1 && c == 0) value = 1000;
                if (p % 7 == 5) value = float(-20 - int(c));
                if (p % 7 == 6) value += float(f) * .3F;
                a.pixels[p * 4 + c] = value;
            }
            a.pixels[p * 4 + 3] = p % 7 == 0 ? 0 : p % 7 == 3 && labels[f] == 0 ? 0 : f % 3 == 0 ? .5F : 1;
        }
        paths.push_back(root / (std::to_string(f) + ".fits"));
        require(FitsCodec().write(a, paths.back()).ok); frames.push_back(std::move(a));
    }
    for (bool mad : {false, true}) {
        options.sigma = {1.5F,1.5F,mad,3};
        options.acceptedGroups.clear(); options.combineWorkers = 1;
        const auto plain = Stacker().stack(paths, options);
        require(plain.acceptedGroupImages[0].empty());
        options.acceptedGroups = labels;
        const auto grouped = Stacker().stack(paths, options);
        unchanged(plain, grouped); pool(grouped);
        for (unsigned g = 0; g < 2; ++g) {
            require(grouped.acceptedGroupWeights[g].pixels[3] == 0);
            require(grouped.acceptedGroupImages[g].pixels[5 * 4] == -20);
        }
        require(grouped.acceptedGroupWeights[1].pixels[2 * 4] == 0);
        require(grouped.acceptedGroupImages[1].pixels[2 * 4 + 3] == 0);
        require(grouped.acceptedGroupImages[0].pixels[2 * 4] == 10);
        require(grouped.acceptedGroupWeights[0].pixels[3 * 4] == 0);
        require(grouped.acceptedGroupImages[1].pixels[3 * 4] == 10);
        require(grouped.acceptedGroupImages[1].pixels[4 * 4 + 3] == 0);
        require(grouped.acceptedGroupWeights[1].pixels[4 * 4] == 0);
        require(grouped.acceptedGroupImages[1].pixels[4 * 4 + 1] == 11);
        require(grouped.acceptedGroupWeights[1].pixels[4 * 4 + 1] > 0);
        auto only = options; only.collectRejectionMaps = only.collectNoiseDiagnostic = false;
        only.frameNoiseVariances.clear();
        const auto groupOnly = Stacker().stack(paths, only);
        require(groupOnly.ok && groupOnly.image.pixels == grouped.image.pixels);
        require(groupOnly.rejectionLow.empty() && groupOnly.noiseDiagnostic.empty());
        for (unsigned g = 0; g < 2; ++g) {
            require(groupOnly.acceptedGroupImages[g].pixels == grouped.acceptedGroupImages[g].pixels);
            require(groupOnly.acceptedGroupWeights[g].pixels == grouped.acceptedGroupWeights[g].pixels);
        }
        for (unsigned workers : {2,8,0}) {
            options.combineWorkers = workers;
            const auto parallel = Stacker().stack(paths, options);
            unchanged(grouped, parallel);
            for (unsigned g = 0; g < 2; ++g) {
                require(grouped.acceptedGroupImages[g].pixels == parallel.acceptedGroupImages[g].pixels);
                require(grouped.acceptedGroupWeights[g].pixels == parallel.acceptedGroupWeights[g].pixels);
            }
        }
        for (auto& g : options.acceptedGroups) g = 1 - g;
        const auto swapped = Stacker().stack(paths, options); unchanged(grouped, swapped);
        for (unsigned g = 0; g < 2; ++g) {
            require(grouped.acceptedGroupImages[g].pixels == swapped.acceptedGroupImages[1-g].pixels);
            require(grouped.acceptedGroupWeights[g].pixels == swapped.acceptedGroupWeights[1-g].pixels);
        }
    }
    // Mean/std can reject EVERY sample with very narrow bounds. The master
    // falls back to the mean; group output must use the same global fallback.
    options.collectNoiseDiagnostic = false; options.frameNoiseVariances.clear();
    options.acceptedGroups = labels; options.sigma = {.001F,.001F,false,3};
    const auto fallback = Stacker().stack(paths, options); require(fallback.ok); pool(fallback);
    for (unsigned g = 0; g < 2; ++g) require(fallback.acceptedGroupWeights[g].pixels[6 * 4] > 0);

    // Distortion cache preparation swaps input 0 with 4, which cross group
    // membership. Verify ORIGINAL labels survive that permutation.
    for (unsigned f = 0; f < frames.size(); ++f) {
        auto& a = frames[f];
        for (unsigned y = 0; y < a.height; ++y) for (unsigned x = 0; x < a.width; ++x) {
            double value = 10 * f;
            for (int sy = 16; sy <= 112; sy += 24) for (int sx = 16; sx <= 136; sx += 24)
                value += 2000 * std::exp(-(std::pow(double(x)-sx,2)+std::pow(double(y)-sy,2))/3);
            for (unsigned c = 0; c < 3; ++c) a.pixels[(y*a.width+x)*4+c] = float(value);
            a.pixels[(y*a.width+x)*4+3] = 1;
        }
        require(FitsCodec().write(a, paths[f]).ok);
    }
    options.sigma = {100,100,true,3};
    const auto direct = Stacker().stack(paths, options); require(direct.ok); pool(direct);
    options.alignDistortion = true; options.registration.minimumMatches = 8;
    options.registration.starDetection.minPeak = 0; options.registration.starDetection.sigmaThreshold = 1;
    const auto aligned = Stacker().stack(paths, options);
    require(aligned.ok && aligned.alignedFrames == 7 && aligned.alignmentFallbacks == 0); pool(aligned);
    for (unsigned g = 0; g < 2; ++g) for (unsigned y = 8; y < 120; ++y) for (unsigned x = 8; x < 152; ++x) {
        const auto p = (y * 160 + x) * 4;
        require(std::abs(aligned.acceptedGroupImages[g].pixels[p]-direct.acceptedGroupImages[g].pixels[p]) < .03);
        require(std::abs(aligned.acceptedGroupWeights[g].pixels[p]-direct.acceptedGroupWeights[g].pixels[p]) < 1e-5);
    }
    options.alignDistortion = false;
    for (auto method : {StackMethod::Average,StackMethod::WeightedAverage,StackMethod::Median,
                        StackMethod::WinsorizedSigmaClip,StackMethod::PercentileClip}) {
        auto bad = options; bad.method = method; require(!Stacker().stack(paths,bad).ok);
    }
    for (auto labelsBad : {std::vector<std::uint8_t>(8,0),std::vector<std::uint8_t>(8,1),
                          std::vector<std::uint8_t>{0,1},std::vector<std::uint8_t>{0,0,0,0,0,0,1,2}}) {
        auto bad = options; bad.acceptedGroups = labelsBad; require(!Stacker().stack(paths,bad).ok);
    }
    auto bad = options; bad.fitsDecodeMode = FitsDecodeMode::DisplayNormalized;
    require(!Stacker().stack(paths,bad).ok);
    auto duplicate = paths; duplicate.back() = duplicate.front(); require(!Stacker().stack(duplicate,options).ok);
}
