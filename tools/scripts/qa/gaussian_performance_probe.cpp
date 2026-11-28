#include "photonstack/LocalContrast.hpp"
#include "photonstack/ReconstructionProtection.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

using namespace photonstack;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::filesystem::create_directories(argv[1]);
    ImageBuffer image;
    image.width = 769; image.height = 513;
    image.pixels.resize(image.sampleCount());
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        for (unsigned c = 0; c < 3; ++c)
            image.pixels[4 * p + c] = 200 * std::sin(float(p % image.width) * .02F + c) + float(p % 103);
        image.pixels[4 * p + 3] = p % 53 == 0 ? 0 : p % 41 == 0 ? .25F : 1;
        if (p % 53 == 0) image.pixels[4 * p] = std::numeric_limits<float>::quiet_NaN();
    }
    auto reference = image;
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        for (unsigned c = 0; c < 3; ++c) reference.pixels[4 * p + c] += float(int(p % 7) - 3);
    ImageBuffer mask;
    mask.width = image.width; mask.height = image.height; mask.channels = 1; mask.format = PixelFormat::Float32Gray;
    mask.pixels.assign(mask.sampleCount(), .7F);
    const auto run = [&](const std::string& name, auto compute) {
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            const auto start = std::chrono::steady_clock::now();
            auto result = compute();
            if (!result.ok) { std::cerr << result.message << '\n'; std::exit(1); }
            std::cout << name << ',' << repeat << ','
                      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                      << '\n' << std::flush;
            if (repeat == 2) {
                std::ofstream output(std::filesystem::path(argv[1]) / (name + ".f32"), std::ios::binary);
                output.write(reinterpret_cast<const char*>(result.image.pixels.data()), result.image.sampleCount() * sizeof(float));
            }
        }
    };
    for (unsigned radius : {32U, 64U}) {
        LocalContrastOptions options;
        options.radius = radius; options.clampOutput = false;
        run("local-" + std::to_string(radius), [&] { return LocalContrast{}.apply(image, options); });
    }
    for (double sigma : {16., 64.}) {
        ReconstructionProtectionOptions options;
        options.backgroundSigma = sigma;
        run("protection-" + std::to_string(unsigned(sigma)), [&] {
            return ReconstructionProtection{}.apply(image, reference, mask, options);
        });
    }
}
