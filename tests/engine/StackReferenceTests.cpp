#include "photonstack/FitsCodec.hpp"
#include "photonstack/Stacker.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <source_location>
#include <stdexcept>

using namespace photonstack;

void require(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok)
        throw std::runtime_error("External reference check at line " + std::to_string(where.line()));
}

ImageBuffer field(float background, int dx, int dy) {
    ImageBuffer a;
    a.width = a.height = 128;
    a.channels = 4;
    a.colorEncoding = ColorEncoding::Linear;
    a.pixels.resize(a.sampleCount());
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            double signal = background;
            for (int sy = 20; sy <= 108; sy += 22)
                for (int sx = 20; sx <= 108; sx += 22) {
                    const auto px = x - sx - dx, py = y - sy - dy;
                    signal += (1000 + sx * 7 + sy * 3) * std::exp(-(px * px + py * py) / 3.0);
                }
            for (int c = 0; c < 3; ++c)
                a.pixels[(y * 128 + x) * 4 + c] = static_cast<float>(signal * (1 + .1 * c));
            a.pixels[(y * 128 + x) * 4 + 3] = 1;
        }
    return a;
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("photonstack-external-reference-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{directory};
    const FitsCodec codec;
    const Stacker stacker;
    const auto referencePath = directory / "reference.fits";
    const auto first = directory / "first.fits", second = directory / "second.fits";
    auto reference = field(1000, 0, 0);
    // A reference-only defect must not appear in any output, even at a tiny weight.
    reference.pixels[(8 * 128 + 8) * 4] = 20000;
    require(codec.write(reference, referencePath).ok);
    require(codec.write(field(10, 3, 2), first).ok);
    require(codec.write(field(30, -2, 3), second).ok);
    StackOptions options;
    options.alignTranslation = true;
    options.alignmentReference = referencePath;
    options.frameWeights = {1, 3};
    options.registration.minimumMatches = 8;
    options.registration.starDetection.minPeak = 0;
    options.registration.starDetection.sigmaThreshold = 1;
    options.sigma = {10, 10};
    options.winsorizedSigma = {10, 10};
    options.percentile = {0, 1};
    const auto expected = field(25, 0, 0);
    for (auto method : {StackMethod::Average, StackMethod::SigmaClip, StackMethod::WinsorizedSigmaClip,
                        StackMethod::PercentileClip}) {
        options.method = method;
        const auto result = stacker.stack({first, second}, options);
        require(result.ok);
        require(result.alignedFrames == 2 && result.alignmentFallbacks == 0);
        for (int c = 0; c < 3; ++c) {
            require(std::abs(result.image.pixels[(8 * 128 + 8) * 4 + c] - 25 * (1 + .1 * c)) < .05);
            require(std::abs(result.image.pixels[(42 * 128 + 42) * 4 + c] - expected.pixels[(42 * 128 + 42) * 4 + c]) <
                    1);
        }
        require(result.image.pixels[(127 * 128 + 64) * 4 + 3] == 0);
        require(std::abs(result.image.pixels[(64 * 128 + 1) * 4 + 3] - .25) < .01);
    }
    // Exercise all combining paths with one moving science image: the old
    // single-input shortcut must not return either unregistered pixels or the reference.
    options.frameWeights = {1};
    const auto oneExpected = field(10, 0, 0);
    for (auto method : {StackMethod::Average, StackMethod::WeightedAverage, StackMethod::Median, StackMethod::SigmaClip,
                        StackMethod::WinsorizedSigmaClip, StackMethod::PercentileClip}) {
        options.method = method;
        const auto result = stacker.stack({first}, options);
        require(result.ok && result.alignedFrames == 1 && result.alignmentFallbacks == 0);
        require(std::abs(result.image.pixels[(8 * 128 + 8) * 4] - 10) < .05);
        require(std::abs(result.image.pixels[(42 * 128 + 42) * 4] - oneExpected.pixels[(42 * 128 + 42) * 4]) < 1);
        require(result.image.pixels[(127 * 128 + 64) * 4 + 3] == 0);
    }
    auto bad = options;
    bad.alignTranslation = false;
    require(!stacker.stack({first}, bad).ok);
    bad = options;
    bad.alignmentReference = directory / "missing.fits";
    require(!stacker.stack({first}, bad).ok);
    require(!stacker.stack({}, options).ok);
    auto wrongSize = reference;
    wrongSize.width = 64;
    wrongSize.pixels.resize(wrongSize.sampleCount());
    const auto wrongPath = directory / "wrong-size.fits";
    require(codec.write(wrongSize, wrongPath).ok);
    require(!stacker.stack({wrongPath}, options).ok);
    // Background matching remains optional and uses only a per-channel offset.
    options.normalizeBackground = true;
    const auto normalized = stacker.stack({first}, options);
    require(normalized.ok);
    require(std::abs(normalized.image.pixels[(8 * 128 + 8) * 4] - 1000) < .1);
    // Both streaming and cached stacks report actual refinement use, while the
    // reference-only defect remains excluded from the scientific combination.
    options.normalizeBackground = false;
    options.alignTranslation = false;
    options.alignSimilarity = true;
    options.registration.refineSimilarityCentroids = true;
    options.frameWeights = {1, 3};
    for (auto method : {StackMethod::Average, StackMethod::SigmaClip}) {
        options.method = method;
        const auto refined = stacker.stack({first, second}, options);
        require(refined.ok && refined.centroidRefinedFrames == 2 && refined.centroidRefinementFallbacks == 0);
        require(std::abs(refined.image.pixels[(8 * 128 + 8) * 4] - 25) < .05);
    }
    options.alignSimilarity = false;
    options.alignAffine = true;
    for (auto method : {StackMethod::Average, StackMethod::SigmaClip}) {
        options.method = method;
        const auto refined = stacker.stack({first, second}, options);
        require(refined.ok && refined.centroidRefinedFrames == 2 && refined.centroidRefinementFallbacks == 0);
        require(refined.centroidAffineFrames == 0 && refined.centroidSimilarityRetainedFrames == 2);
        require(std::abs(refined.image.pixels[(8 * 128 + 8) * 4] - 25) < .05);
    }
    options.alignAffine = false;
    options.alignTranslation = true;
    const auto unsupported = stacker.stack({first, second}, options);
    require(!unsupported.ok && unsupported.errorCode == "ArgumentInvalid");
}
