#include "photonstack/Curves.hpp"
#include "photonstack/StageProfile.hpp"
#include <chrono>
#include <cstring>
#include <iostream>

using namespace photonstack;
int main(int argc, char**) {
    ImageBuffer image;
    image.width = argc > 1 ? 6001 : 2049;
    image.height = argc > 1 ? 4001 : 1025;
    image.pixels.resize(image.sampleCount());
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        for (unsigned c = 0; c < 3; ++c) image.pixels[4 * p + c] = float((p * 719 + c * 313) % 65536) / 65535;
        image.pixels[4 * p + 3] = 1;
    }
    CurvesOptions options;
    options.points = {{0, .01F}, {.2F, .35F}, {.6F, .55F}, {1, .99F}};
    detail::StageProfile profile;
    ImageBuffer reference;
    for (unsigned run = 0; run < 6; ++run) {
        options.preferMetal = run % 2;
        const detail::StageScope context({&profile, detail::Stage::Other, options.preferMetal});
        const auto start = std::chrono::steady_clock::now();
        auto result = Curves{}.apply(image, options);
        std::cout << (options.preferMetal ? "gpu" : "cpu") << ',' << run / 2 << ','
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                  << '\n' << std::flush;
        if (!result.ok) return 1;
        if (!options.preferMetal) reference = std::move(result.image);
        else if (std::memcmp(reference.pixels.data(), result.image.pixels.data(), image.sampleCount() * sizeof(float))) return 2;
    }
    std::cout << "gpu_seconds," << profile.stages[0].gpu << '\n';
    return profile.stages[0].gpu > 0 ? 0 : 77;
}
