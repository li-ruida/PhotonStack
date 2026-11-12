#include "../../engine/src/MetalCompute.hpp"
#include "../../engine/src/RegisteredStackCache.hpp"
#include "photonstack/StageProfile.hpp"
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace photonstack;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {
void check(bool pass, const std::string& message) {
    if (!pass)
        throw std::runtime_error(message);
}
template <class F> auto timed(const char* label, F operation) {
    const auto start = Clock::now();
    auto value = operation();
    std::cout << label << " " << std::chrono::duration<double>(Clock::now() - start).count() << " s\n";
    return value;
}
void compare(const ImageBuffer& cpu, const ImageBuffer& gpu, float colorTolerance) {
    check(cpu.pixels.size() == gpu.pixels.size() && !cpu.empty(), "shape mismatch");
    for (std::size_t i = 0; i < cpu.pixels.size(); ++i) {
        const auto tolerance = i % 4 == 3 ? 2.e-7F : colorTolerance;
        if (!(std::abs(cpu.pixels[i] - gpu.pixels[i]) <= tolerance))
            throw std::runtime_error("sample " + std::to_string(i) + ": " + std::to_string(cpu.pixels[i]) + " vs " +
                                     std::to_string(gpu.pixels[i]));
    }
}
void calibrationAndRegistration(detail::StageProfile& profile) {
    ImageBuffer image;
    image.width = 1024;
    image.height = 1024;
    image.pixels.resize(image.sampleCount());
    ImageBuffer dark = image, bias = image, flat = image;
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        for (unsigned c = 0; c < 3; ++c) {
            image.pixels[p * 4 + c] = float(150 + (p + 7 * c) % 101);
            dark.pixels[p * 4 + c] = float(24 + p % 7);
            bias.pixels[p * 4 + c] = 100;
            flat.pixels[p * 4 + c] = float(95 + p % 381);
        }
        image.pixels[p * 4 + 3] = p % 81 == 0 ? .5F : 1.F;
        flat.pixels[p * 4 + 3] = p % 91 == 0 ? 0.F : 1.F;
        dark.pixels[p * 4 + 3] = bias.pixels[p * 4 + 3] = 1;
    }
    CalibrationOptions cal{&dark, &bias, &flat, CalibrationBiasState::Removed, CalibrationBiasState::Included};
    cal.clampNegativeValues = false;
    const auto cpu = timed("calibrate CPU", [&] { return Calibrator().calibrate(image, cal); });
    const auto gpu = timed("calibrate Metal", [&] {
        const detail::StageScope mode({&profile, detail::Stage::Other, true});
        return Calibrator().calibrate(image, cal);
    });
    check(cpu.ok && gpu.ok && profile.stages[unsigned(detail::Stage::Calibration)].gpu > 0, "calibration backend");
    compare(cpu.image, gpu.image, .001F);
    const Registration native(RegistrationInterpolation::Bicubic, ComputeBackend::CPU);
    const Registration metal(RegistrationInterpolation::Bicubic, ComputeBackend::Metal);
    const AffineTransform affine{1.001F, .002F, -.001F, .998F, .3F, -.7F};
    const detail::StageScope mode({&profile, detail::Stage::Other, true});
    auto reference = timed("affine CPU", [&] { return native.applyAffine(image, affine); });
    auto result = timed("affine Metal", [&] { return metal.applyAffine(image, affine); });
    compare(reference, result, .003F);
    DistortionTransform distortion;
    distortion.usePolynomial = true;
    distortion.polynomialDx[1] = .25F;
    distortion.controlPoints = {{100, 100, .5F, -.25F}, {700, 400, -.3F, .12F}, {600, 800, .2F, .35F}};
    reference = timed("distortion CPU", [&] { return native.applyDistortion(image, distortion); });
    const auto warpBefore = profile.stages[unsigned(detail::Stage::Warp)].gpu;
    result = timed("distortion Metal", [&] { return metal.applyDistortion(image, distortion); });
    check(profile.stages[unsigned(detail::Stage::Warp)].gpu > warpBefore, "distortion backend");
    compare(reference, result, .003F);
}
void weightedSequence(detail::StageProfile& profile, unsigned count) {
    const auto root = fs::temp_directory_path() /
                      ("photonstack-metal-test-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup {
        fs::path root;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    } cleanup{root};
    std::vector<fs::path> paths;
    std::vector<detail::RegisteredFrame> frames(count);
    StackOptions options;
    options.method = StackMethod::WinsorizedSigmaClip;
    options.collectRejectionMaps = true;
    options.combineWorkers = 4;
    std::mt19937 random(1204);
    std::normal_distribution<float> noise;
    ImageBuffer image;
    image.width = 96;
    image.height = 32;
    image.pixels.resize(image.sampleCount());
    for (unsigned f = 0; f < count; ++f) {
        options.frameWeights.push_back(.3F + .7F * float(f % 37) / 36);
        for (std::size_t p = 0; p < image.pixelCount(); ++p) {
            for (unsigned c = 0; c < 3; ++c)
                image.pixels[4 * p + c] = float(p % 63) + noise(random) + ((p + f + c) % 119 == 0 ? 14.F : 0.F);
            image.pixels[4 * p + 3] = p % 83 == 0 ? 0.F : (p + f) % 23 == 0 ? .67F : 1.F;
        }
        paths.push_back(root / (std::to_string(f) + ".f32"));
        check(detail::writeRegisteredFrame(paths.back(), image, false, frames[f]).ok, "fixture write");
    }
    auto cpu = timed("weighted CPU", [&] { return detail::combineRegisteredFrames(paths, frames, options); });
    const detail::StageScope mode({&profile, detail::Stage::Other, true});
    options.preferMetalCombine = true;
    auto gpu = timed("weighted Metal", [&] { return detail::combineRegisteredFrames(paths, frames, options); });
    check(cpu.ok && gpu.ok && profile.stages[unsigned(detail::Stage::Combine)].gpu > 0, "stack backend");
    compare(cpu.image, gpu.image, 2.e-5F);
    compare(cpu.rejectionLow, gpu.rejectionLow, 0);
    compare(cpu.rejectionHigh, gpu.rejectionHigh, 0);
    check(cpu.rejectedLowSamples == gpu.rejectedLowSamples && cpu.rejectedHighSamples == gpu.rejectedHighSamples &&
              cpu.comparedSamples == gpu.comparedSamples,
          "frame rejection counts");
}
} // namespace

int main() {
    try {
        std::vector<float> probe(65536, 1), mask;
        if (!MTLCreateSystemDefaultDevice()) {
            std::cout << "Metal device unavailable\n";
            return 77;
        }
        check(detail::metalPeakMask(probe, 1, mask), "Metal kernel compilation or dispatch failed");
        detail::StageProfile profile;
        calibrationAndRegistration(profile);
        weightedSequence(profile, 711);
        weightedSequence(profile, 1297);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
