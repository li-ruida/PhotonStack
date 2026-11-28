#include "photonstack/Deconvolution.hpp"
#include "photonstack/FrameNormalizer.hpp"
#include "photonstack/GeneralizedStretch.hpp"
#include "photonstack/MosaicBuilder.hpp"
#include "photonstack/MultiscaleDenoiser.hpp"
#include "photonstack/NonlocalDenoiser.hpp"
#include "photonstack/Registration.hpp"
#include "photonstack/Sharpen.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace photonstack;
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3)
        return 2;
    std::filesystem::create_directories(argv[1]);
    ImageBuffer source;
    source.width = 768;
    source.height = 512;
    source.pixels.resize(source.sampleCount());
    for (std::size_t p = 0; p < source.pixelCount(); ++p) {
        for (unsigned c = 0; c < 3; ++c)
            source.pixels[p * 4 + c] = .15f + .1f * std::sin(float(p % source.width) * .02f + c) +
                                       .015f * float((p * 719 + c * 313) % 101) / 101;
        source.pixels[p * 4 + 3] = 1;
    }
    ImageBuffer blend;
    blend.width = source.width;
    blend.height = source.height;
    blend.channels = 1;
    blend.format = PixelFormat::Float32Gray;
    blend.pixels.assign(blend.sampleCount(), .6f);
    auto run = [&](const char* name, auto compute) {
        if (argc == 3 && std::string(argv[2]) != name)
            return;
        for (int repeat = 0; repeat < 3; ++repeat) {
            const auto start = std::chrono::steady_clock::now();
            auto result = compute();
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (result.image.empty()) {
                std::cerr << name << " failed\n";
                std::exit(1);
            }
            std::cout << name << "," << repeat << "," << ms << "\n" << std::flush;
            if (repeat == 2) {
                std::ofstream out(std::filesystem::path(argv[1]) / (std::string(name) + ".f32"), std::ios::binary);
                out.write(reinterpret_cast<const char*>(result.image.pixels.data()),
                          result.image.pixels.size() * sizeof(float));
            }
        }
    };
    run("nonlocal", [&] { return NonlocalDenoiser{}.apply(source, blend, {.h = .02}); });
    run("multiscale", [&] { return MultiscaleDenoiser{}.apply(source, blend, {.luminance = .5, .chroma = .4}); });
    run("deconvolution", [&] { return Deconvolution{}.richardsonLucy(source, {.iterations = 16}); });
    run("sharpen", [&] { return Sharpen{}.unsharpMask(source, {.radius = 8}); });
    run("stretch", [&] { return generalizedStretch(source, {}); });
    run("normalization", [&] { return FrameNormalizer{}.matchReference(source, source); });
    run("mosaic", [&] {
        return MosaicBuilder{}.stitchHorizontal(std::vector<ImageBuffer>{source, source},
                                                {.overlapPixels = 128,
                                                 .projection = MosaicProjection::Cylindrical,
                                                 .alignment = MosaicAlignment::Manual,
                                                 .blendMode = MosaicBlendMode::Multiband,
                                                 .exposureMatching = false});
    });
    DistortionTransform distortion;
    distortion.usePolynomial = true;
    distortion.polynomialDx[0] = .12345F;
    distortion.polynomialDy[0] = -.23456F;
    for (unsigned i = 0; i < 12; ++i)
        distortion.controlPoints.push_back(
            {float(i % 4) * 240.F, float(i / 4) * 240.F, float(i % 3) * .171F - .2F, float(i % 5) * .09F - .1F});
    run("distortion", [&] {
        struct Result {
            ImageBuffer image;
        };
        return Result{Registration(RegistrationInterpolation::Bicubic).applyDistortion(source, distortion)};
    });
}
