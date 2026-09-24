#include "photonstack/FitsCodec.hpp"
#include "photonstack/Stacker.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <source_location>
#include <stdexcept>

using namespace photonstack;
namespace fs = std::filesystem;
void require(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Stack noise diagnostic check at line " + std::to_string(where.line()));
}
int main() {
    const auto root = fs::temp_directory_path() / ("photonstack-noise-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    constexpr unsigned width = 160, height = 128, frameCount = 24;
    std::vector<fs::path> paths;
    StackOptions options;
    options.method = StackMethod::SigmaClip;
    options.sigma = {100, 100, true, 3}; // Exact linear-mean variance identity first.
    options.collectRejectionMaps = true;
    std::mt19937 rng(5821);
    std::normal_distribution<double> gaussian;
    std::vector<ImageBuffer> frames;
    std::vector<double> truth(width * height);
    for (unsigned p = 0; p < truth.size(); ++p) {
        const double x = p % width, y = p / width;
        truth[p] = -30 + 20 * std::sin(x * .06) * std::cos(y * .07);
    }
    for (unsigned f = 0; f < frameCount; ++f) {
        options.frameWeights.push_back(float(1 + f % 5));
        options.frameNoiseVariances.push_back(1 + (f % 4) * 2.);
        ImageBuffer a;
        a.width = width; a.height = height; a.pixels.resize(a.sampleCount());
        for (unsigned p = 0; p < a.pixelCount(); ++p) {
            for (unsigned c = 0; c < 3; ++c)
                a.pixels[p * 4 + c] = float(truth[p] * (c + 1) + std::sqrt(options.frameNoiseVariances.back()) * gaussian(rng));
            // Insufficient half coverage at pixel 0; fractional coverage at 1.
            a.pixels[p * 4 + 3] = p == 0 && f >= 4 ? 0 : p == 1 && f % 3 == 0 ? .5F : 1;
        }
        paths.push_back(root / (std::to_string(f) + ".fits"));
        require(FitsCodec().write(a, paths.back()).ok);
        frames.push_back(std::move(a));
    }
    auto baselineOptions = options;
    baselineOptions.frameNoiseVariances.clear();
    const auto baseline = Stacker().stack(paths, baselineOptions);
    options.collectNoiseDiagnostic = true;
    const auto withNoise = Stacker().stack(paths, options);
    require(baseline.ok && withNoise.ok && !withNoise.noiseDiagnostic.empty());
    require(baseline.noiseDiagnostic.empty());
    require(baseline.image.pixels == withNoise.image.pixels);
    require(baseline.rejectionLow.pixels == withNoise.rejectionLow.pixels);
    require(baseline.rejectionHigh.pixels == withNoise.rejectionHigh.pixels);
    require(baseline.comparedSamples == withNoise.comparedSamples);
    require(withNoise.noiseDiagnostic.pixels[3] == 0);
    for (int c = 0; c < 3; ++c) require(withNoise.noiseDiagnostic.pixels[c] == 0);

    // Analytic weighted contrast, including fractional coverage and unequal
    // per-frame noise. Independent implementation from original input arrays.
    for (unsigned p : {1U, 777U}) {
        double sum[2] = {}, weight[2] = {}, moment[2] = {};
        for (unsigned f = 0; f < frameCount; ++f) {
            const double w = double(options.frameWeights[f]) * frames[f].pixels[p * 4 + 3];
            sum[f % 2] += w * frames[f].pixels[p * 4];
            weight[f % 2] += w;
            moment[f % 2] += w * w * options.frameNoiseVariances[f];
        }
        const double vf = (moment[0] + moment[1]) / std::pow(weight[0] + weight[1], 2);
        const double vd = moment[0] / (weight[0] * weight[0]) + moment[1] / (weight[1] * weight[1]);
        const double expected = (sum[0] / weight[0] - sum[1] / weight[1]) * std::sqrt(vf / vd);
        require(std::abs(withNoise.noiseDiagnostic.pixels[p * 4] - expected) < 1.e-5);
        require(withNoise.noiseDiagnostic.pixels[p * 4 + 3] == 1);
    }
    double masterError = 0, noisePower = 0, noiseSum = 0, structureCross = 0, structurePower = 0;
    for (unsigned p = 2; p < width * height; ++p) {
        const double error = withNoise.image.pixels[p * 4] - truth[p];
        const double noise = withNoise.noiseDiagnostic.pixels[p * 4];
        masterError += error * error; noisePower += noise * noise; noiseSum += noise;
        structureCross += noise * (truth[p] + 30); structurePower += std::pow(truth[p] + 30, 2);
    }
    const double varianceRatio = noisePower / masterError;
    std::cout << "unclipped noise/master variance ratio " << varianceRatio << '\n';
    require(varianceRatio > .94 && varianceRatio < 1.06);
    require(std::abs(noiseSum / (width * height - 2)) < .03);
    require(std::abs(structureCross / std::sqrt(noisePower * structurePower)) < .03);
    // Relative units of variance must cancel, even with large scale factors.
    auto scaled = options;
    for (auto& v : scaled.frameNoiseVariances) v *= 1.e200;
    const auto rescaled = Stacker().stack(paths, scaled);
    require(rescaled.ok);
    for (unsigned p = 0; p < width * height; ++p)
        require(std::abs(rescaled.noiseDiagnostic.pixels[p * 4] - withNoise.noiseDiagnostic.pixels[p * 4]) < 1.e-6);

    // Robust halves must independently reject transients. A single value must
    // not enter both halves or affect the full master's rejection accounting.
    for (unsigned f = 0; f < frameCount; ++f) {
        for (unsigned p = 0; p < width * height; ++p)
            for (unsigned c = 0; c < 3; ++c)
                frames[f].pixels[p * 4 + c] += (p % width == 20 && f == 0) ? 1000 : 0;
        require(FitsCodec().write(frames[f], paths[f]).ok);
    }
    options.sigma = {3, 3, true, 3};
    const auto clipped = Stacker().stack(paths, options);
    baselineOptions.sigma = options.sigma;
    const auto clippedBaseline = Stacker().stack(paths, baselineOptions);
    require(clipped.ok && clippedBaseline.ok);
    require(clipped.image.pixels == clippedBaseline.image.pixels);
    require(clipped.rejectedHighSamples == clippedBaseline.rejectedHighSamples);
    double clippedError = 0, clippedNoise = 0; unsigned clippedPixels = 0;
    for (unsigned p = 2; p < width * height; ++p) {
        if (p % width == 20 || clipped.noiseDiagnostic.pixels[p * 4 + 3] == 0) continue;
        clippedError += std::pow(clipped.image.pixels[p * 4] - truth[p], 2);
        clippedNoise += std::pow(clipped.noiseDiagnostic.pixels[p * 4], 2);
        ++clippedPixels;
    }
    const double clippedRatio = clippedNoise / clippedError;
    std::cout << "clipped diagnostic/master variance ratio " << clippedRatio
              << "; valid pixels " << clippedPixels << '\n';
    // Clipping is nonlinear and separately estimated in each half. This is a
    // coarse sanity bound, deliberately not an exact-noise calibration claim.
    require(clippedPixels > width * height * .9 && clippedRatio > .7 && clippedRatio < 1.4);
    double artifactPower = 0; unsigned valid = 0;
    for (unsigned y = 0; y < height; ++y) {
        const auto p = (y * width + 20) * 4;
        if (clipped.noiseDiagnostic.pixels[p + 3] == 0) continue;
        artifactPower += std::pow(clipped.noiseDiagnostic.pixels[p], 2); ++valid;
    }
    require(valid > 100 && std::sqrt(artifactPower / valid) < 2);

    // Distortion registration swaps input 0 with the middle input in its
    // cache. Six inputs make that swap cross half membership (0 <-> 3).
    // Reference coordinates are identical here, so the diagnostic must match
    // unregistered stacking despite reordered weights, variances and parity.
    auto alignmentPaths = paths; alignmentPaths.resize(6);
    auto alignmentOptions = options;
    alignmentOptions.frameWeights.resize(6);
    alignmentOptions.frameNoiseVariances.resize(6);
    alignmentOptions.sigma = {100, 100, true, 3};
    for (unsigned f = 0; f < 6; ++f) {
        auto& a = frames[f];
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            double value = 10 * f;
            for (int sy = 16; sy <= 112; sy += 24) for (int sx = 16; sx <= 136; sx += 24)
                value += 2000 * std::exp(-(std::pow(double(x) - sx, 2) + std::pow(double(y) - sy, 2)) / 3);
            for (unsigned c = 0; c < 3; ++c) a.pixels[(y * width + x) * 4 + c] = float(value);
            a.pixels[(y * width + x) * 4 + 3] = 1;
        }
        require(FitsCodec().write(a, alignmentPaths[f]).ok);
    }
    const auto unaligned = Stacker().stack(alignmentPaths, alignmentOptions);
    alignmentOptions.alignDistortion = true;
    alignmentOptions.registration.minimumMatches = 8;
    alignmentOptions.registration.starDetection.minPeak = 0;
    alignmentOptions.registration.starDetection.sigmaThreshold = 1;
    const auto aligned = Stacker().stack(alignmentPaths, alignmentOptions);
    require(unaligned.ok && aligned.ok && aligned.alignedFrames == 5 && aligned.alignmentFallbacks == 0);
    for (unsigned y = 8; y < height - 8; ++y) for (unsigned x = 8; x < width - 8; ++x) {
        const auto p = (y * width + x) * 4;
        require(aligned.noiseDiagnostic.pixels[p + 3] == 1);
        require(std::abs(aligned.noiseDiagnostic.pixels[p] - unaligned.noiseDiagnostic.pixels[p]) < .03);
    }

    // Invalid or unsupported noise requests fail before any input decode.
    for (auto method : {StackMethod::Average, StackMethod::WeightedAverage, StackMethod::Median,
                         StackMethod::WinsorizedSigmaClip, StackMethod::PercentileClip}) {
        auto bad = options; bad.method = method; require(!Stacker().stack(paths, bad).ok);
    }
    for (double v : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto bad = options; bad.frameNoiseVariances[0] = v; require(!Stacker().stack(paths, bad).ok);
    }
    auto bad = options; bad.frameNoiseVariances.pop_back(); require(!Stacker().stack(paths, bad).ok);
    bad = options; bad.collectNoiseDiagnostic = false; require(!Stacker().stack(paths, bad).ok);
    bad = options; bad.fitsDecodeMode = FitsDecodeMode::DisplayNormalized; require(!Stacker().stack(paths, bad).ok);
    auto duplicates = paths; duplicates.back() = duplicates.front(); require(!Stacker().stack(duplicates, options).ok);
    auto small = paths; small.resize(5); bad = options; bad.frameNoiseVariances.resize(5);
    require(!Stacker().stack(small, bad).ok);
}
