#include "photonstack/FitsCodec.hpp"
#include "photonstack/Stacker.hpp"
#include <cmath>
#include <filesystem>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <vector>
using namespace photonstack;
void require(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok)
        throw std::runtime_error("Stack weights check at line " + std::to_string(where.line()));
}
int main() {
    const FitsCodec codec;
    const Stacker stacker;
    std::vector<std::filesystem::path> paths;
    for (int frame = 0; frame < 3; ++frame) {
        ImageBuffer a;
        a.width = a.height = 8;
        a.channels = 4;
        a.colorEncoding = ColorEncoding::Linear;
        a.pixels.resize(a.sampleCount());
        const float value = 10.F * (1 << frame);
        for (std::size_t p = 0; p < a.pixelCount(); ++p) {
            for (int c = 0; c < 3; ++c)
                a.pixels[p * 4 + c] = c == 2 ? -value : value;
            a.pixels[p * 4 + 3] = (p == 1 && frame == 2) ? 0 : (p == 2 && frame == 1) ? .5F : 1;
        }
        const auto path =
            std::filesystem::temp_directory_path() / ("photonstack-weight-test-" + std::to_string(frame) + ".fits");
        require(codec.write(a, path).ok);
        paths.push_back(path);
    }
    StackOptions options;
    options.frameWeights = {1, 2, 4};
    options.sigma = {10, 10};
    options.winsorizedSigma = {10, 10};
    options.percentile = {0, 1};
    for (auto method : {StackMethod::Average, StackMethod::SigmaClip, StackMethod::WinsorizedSigmaClip,
                        StackMethod::PercentileClip}) {
        options.method = method;
        const auto result = stacker.stack(paths, options);
        require(result.ok);
        require(std::abs(result.image.pixels[0] - 30) < 1e-4);
        require(std::abs(result.image.pixels[2] + 30) < 1e-4);
        require(std::abs(result.image.pixels[4] - 50.F / 3) < 1e-4);
        require(std::abs(result.image.pixels[7] - 3.F / 7) < 1e-5);
        require(std::abs(result.image.pixels[8] - 190.F / 6) < 1e-4);
        require(std::abs(result.image.pixels[11] - 6.F / 7) < 1e-5);
        auto scaled = options;
        scaled.frameWeights = {1e20F, 2e20F, 4e20F};
        const auto again = stacker.stack(paths, scaled);
        require(again.ok);
        for (std::size_t i = 0; i < result.image.sampleCount(); ++i)
            require(std::abs(again.image.pixels[i] - result.image.pixels[i]) < 1e-4);
    }
    options.method = StackMethod::Median;
    const auto median = stacker.stack(paths, options);
    require(median.ok);
    require(std::abs(median.image.pixels[0] - 30) < 1e-4);
    for (const auto& invalid : {std::vector<float>{1, 2}, std::vector<float>{0, 1, 1}, std::vector<float>{1, -1, 1},
                                std::vector<float>{1, std::numeric_limits<float>::quiet_NaN(), 1}}) {
        auto bad = options;
        bad.frameWeights = invalid;
        require(!stacker.stack(paths, bad).ok);
    }
    // A small relative exposure weight is not an invalid coverage mask. When
    // the dominant frame is absent, the faintly weighted valid samples remain.
    auto tinyWeights = options;
    tinyWeights.method = StackMethod::SigmaClip;
    tinyWeights.frameWeights = {1.e-10F, 2.e-10F, 1};
    const auto tinyResult = stacker.stack(paths, tinyWeights);
    require(tinyResult.ok && std::abs(tinyResult.image.pixels[4] - 50.F / 3) < 1.e-4F);
    // Distortion picks the middle input as its reference. Cache positions must
    // reorder weights with frames, never attach input 0's weight to input 1.
    for (int frame = 0; frame < 3; ++frame) {
        ImageBuffer a;
        a.width = a.height = 128;
        a.channels = 4;
        a.colorEncoding = ColorEncoding::Linear;
        a.pixels.resize(a.sampleCount());
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                double signal = 100 * frame;
                for (int sy = 16; sy <= 112; sy += 24)
                    for (int sx = 16; sx <= 112; sx += 24)
                        signal += 2000 * std::exp(-((x - sx) * (x - sx) + (y - sy) * (y - sy)) / 3.0);
                for (int c = 0; c < 3; ++c)
                    a.pixels[(y * 128 + x) * 4 + c] = static_cast<float>(signal);
                a.pixels[(y * 128 + x) * 4 + 3] = 1;
            }
        require(codec.write(a, paths[frame]).ok);
    }
    options.method = StackMethod::SigmaClip;
    options.alignDistortion = true;
    options.registration.minimumMatches = 8;
    options.registration.starDetection.minPeak = 0;
    options.registration.starDetection.sigmaThreshold = 1;
    const auto aligned = stacker.stack(paths, options);
    require(aligned.ok);
    require(aligned.alignedFrames == 2 && aligned.alignmentFallbacks == 0);
    require(std::abs(aligned.image.pixels[(4 * 128 + 4) * 4] - 1000.F / 7) < .05F);
    for (const auto& p : paths)
        std::filesystem::remove(p);
}
