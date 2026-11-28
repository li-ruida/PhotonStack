#include "../../engine/src/MetalCompute.hpp"
#include "../../engine/src/RegisteredStackCache.hpp"
#include "photonstack/StageProfile.hpp"
#include "photonstack/DeepSkyWorkflow.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/Curves.hpp"
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
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
void compareExact(const ImageBuffer& cpu, const ImageBuffer& gpu) {
    check(cpu.width == gpu.width && cpu.height == gpu.height && cpu.channels == gpu.channels &&
              cpu.pixels.size() == gpu.pixels.size() && !cpu.empty(), "diagnostic shape mismatch");
    check(std::memcmp(cpu.pixels.data(), gpu.pixels.data(), cpu.pixels.size() * sizeof(float)) == 0,
          "diagnostic pixels differ bitwise");
}
void calibrationAndRegistration(detail::StageProfile& profile) {
    ImageBuffer image;
    // Neither dimension nor byte count is a threadgroup / VM-page multiple.
    // This exercises the last partial 2D group and padded zero-copy allocation.
    image.width = 1025;
    image.height = 1023;
    image.pixels.resize(image.sampleCount());
    check(reinterpret_cast<std::uintptr_t>(image.pixels.data()) % detail::pixelPageSize() == 0,
          "Metal image storage must be page-aligned");
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
    const auto flatMeans = Calibrator::measureFlat(cal);
    const auto masterValidation = Calibrator::validateMasters(cal);
    cal.flatMeans = &flatMeans;
    cal.masterValidation = &masterValidation;
    const auto preparedCPU = timed("calibrate prepared CPU", [&] { return Calibrator().calibrate(image, cal); });
    const auto preparedGPU = timed("calibrate prepared Metal", [&] {
        const detail::StageScope mode({&profile, detail::Stage::Other, true});
        return Calibrator().calibrate(image, cal);
    });
    check(preparedCPU.ok && preparedGPU.ok, "prepared calibration backend");
    compareExact(cpu.image, preparedCPU.image);
    compareExact(gpu.image, preparedGPU.image);
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
    image.width = 97;
    image.height = 33;
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
    // Independent noise and accepted groups still need deterministic CPU
    // variance accumulation, but must reuse the GPU full-stack centers.
    if (count == 711) {
        options.method = StackMethod::SigmaClip;
        options.sigma.medianMad = true;
        options.fitsDecodeMode = FitsDecodeMode::Scientific;
        options.collectNoiseDiagnostic = true;
        options.collectAcceptedWeights = true;
        for (unsigned f = 0; f < count; ++f) {
            options.frameNoiseVariances.push_back(1 + f % 7);
            options.acceptedGroups.push_back(f % 2);
        }
        options.preferMetalCombine = false;
        cpu = timed("diagnostic CPU", [&] { return detail::combineRegisteredFrames(paths, frames, options); });
        const auto previous = profile.stages[unsigned(detail::Stage::Combine)].gpu;
        options.preferMetalCombine = true;
        gpu = timed("diagnostic Metal", [&] { return detail::combineRegisteredFrames(paths, frames, options); });
        check(cpu.ok && gpu.ok && profile.stages[unsigned(detail::Stage::Combine)].gpu > previous,
              "diagnostic centers backend");
        compareExact(cpu.image, gpu.image);
        compareExact(cpu.noiseDiagnostic, gpu.noiseDiagnostic);
        compareExact(cpu.acceptedWeights, gpu.acceptedWeights);
        for (unsigned g = 0; g < 2; ++g) {
            compareExact(cpu.acceptedGroupImages[g], gpu.acceptedGroupImages[g]);
            compareExact(cpu.acceptedGroupWeights[g], gpu.acceptedGroupWeights[g]);
        }
    }
}
void automaticCalibratedFITS() {
    const auto root = fs::temp_directory_path() /
        ("photonstack-auto-metal-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
    ImageBuffer image;
    image.width = image.height = 128;
    image.pixels.assign(image.sampleCount(), .01F);
    for (std::size_t p = 0; p < image.pixelCount(); ++p) image.pixels[4 * p + 3] = 1;
    const auto dark = root / "dark.fits";
    check(FitsCodec{}.write(image, dark).ok, "automatic fixture dark");
    std::vector<fs::path> inputs;
    std::mt19937 random(18923);
    std::normal_distribution<float> noise(0, .003F);
    for (unsigned frame = 0; frame < 16; ++frame) {
        for (std::size_t p = 0; p < image.pixelCount(); ++p)
            for (unsigned c = 0; c < 3; ++c) image.pixels[4 * p + c] = .2F + .01F * c + noise(random);
        inputs.push_back(root / (std::to_string(frame) + ".fits"));
        check(FitsCodec{}.write(image, inputs.back()).ok, "automatic fixture FITS");
    }
    DeepSkyWorkflowOptions options;
    options.darks = {dark};
    options.stack.alignSimilarity = false;
    options.stack.normalizeBackground = false;
    options.quality.measureStars = options.quality.measureTrails = false;
    options.quality.overrides.assign(inputs.size(), FrameSelectionOverride::Keep);
    options.background = DeepSkyBackground::None;
    options.alignChannels = false;
    options.multiscale.luminance = options.multiscale.chroma = 0;
    options.develop.stellarBalance = false;
    options.develop.starColorCleanup = 0;
    detail::StageProfile cpuProfile, autoProfile;
    options.computeBackend = ComputeBackend::CPU;
    DeepSkyWorkflowResult cpu, automatic;
    {
        const detail::StageScope scope({&cpuProfile, detail::Stage::Other, false});
        cpu = DeepSkyWorkflow{}.run(inputs, root / "cpu", options);
    }
    options.computeBackend = ComputeBackend::Automatic;
    {
        const detail::StageScope scope({&autoProfile, detail::Stage::Other, false});
        automatic = DeepSkyWorkflow{}.run(inputs, root / "automatic", options);
    }
    check(cpu.ok, "CPU format workflow: " + cpu.message);
    check(automatic.ok, "automatic format workflow: " + automatic.message);
    check(cpu.selectedCount == inputs.size() && automatic.selectedCount == inputs.size(), "calibrated frame selection");
    check(cpuProfile.stages[unsigned(detail::Stage::Combine)].metalOperations.empty() &&
          !autoProfile.stages[unsigned(detail::Stage::Combine)].metalOperations.empty(), "automatic input-format/calibration backend");
    const auto cpuMaster = FitsCodec{}.read(cpu.master, {.mode = FitsDecodeMode::Scientific});
    const auto autoMaster = FitsCodec{}.read(automatic.master, {.mode = FitsDecodeMode::Scientific});
    check(cpuMaster.ok && autoMaster.ok, "workflow master decoding");
    compare(cpuMaster.image, autoMaster.image, 2.e-5F);
}
void curves(detail::StageProfile& profile) {
    ImageBuffer image;
    image.width = 2049; image.height = 1025;
    image.pixels.resize(image.sampleCount());
    std::mt19937 random(7182);
    std::uniform_real_distribution<float> sample(-.2F, 1.2F);
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        for (unsigned c = 0; c < 3; ++c) image.pixels[4 * p + c] = sample(random);
        image.pixels[4 * p + 3] = float(int(p % 5) - 1) * .5F;
    }
    for (bool preserve : {false, true}) {
        image.pixels[0] = preserve ? std::numeric_limits<float>::quiet_NaN() : .2F;
        for (auto channel : {CurveChannel::RGB, CurveChannel::Red, CurveChannel::Green, CurveChannel::Blue}) {
            CurvesOptions options;
            options.points = {{0, .01F}, {.2F, .35F}, {.6F, .55F}, {1, .99F}};
            options.channel = channel; options.preserveAlpha = preserve;
            const auto cpu = timed("curves CPU", [&] { return Curves{}.apply(image, options); });
            const auto previous = profile.stages[unsigned(detail::Stage::Other)].gpu;
            const detail::StageScope mode({&profile, detail::Stage::Other, true});
            options.preferMetal = true;
            const auto gpu = timed("curves Metal fused validation", [&] { return Curves{}.apply(image, options); });
            check(cpu.ok && gpu.ok && profile.stages[unsigned(detail::Stage::Other)].gpu > previous, "curves backend");
            compareExact(cpu.image, gpu.image);
        }
    }
    CurvesOptions options;
    image.pixels[4] = std::numeric_limits<float>::denorm_min();
    image.pixels[7] = 1;
    auto cpu = Curves{}.apply(image, options);
    options.preferMetal = true;
    auto gpu = Curves{}.apply(image, options);
    check(cpu.ok && gpu.ok, "tiny curve fallback");
    compareExact(cpu.image, gpu.image);
    image.pixels[4] = std::numeric_limits<float>::quiet_NaN();
    check(Curves{}.apply(image, options).errorCode == "ImageBufferInvalid", "visible NaN curve validation");
    image.pixels[3] = std::numeric_limits<float>::infinity();
    check(Curves{}.apply(image, options).errorCode == "ImageBufferInvalid", "alpha curve validation");
    options.points[0].input = std::numeric_limits<float>::quiet_NaN();
    check(Curves{}.apply(image, options).errorCode == "ImageBufferInvalid", "curve error precedence");
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
        curves(profile);
        weightedSequence(profile, 711);
        weightedSequence(profile, 1297);
        automaticCalibratedFITS();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
