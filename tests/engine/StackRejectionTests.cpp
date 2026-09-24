#include "photonstack/FitsCodec.hpp"
#include "photonstack/Stacker.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <source_location>
#include <stdexcept>

using namespace photonstack;
namespace fs = std::filesystem;
void require(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Stack rejection check at line " + std::to_string(where.line()));
}
int main() {
    const auto root = fs::temp_directory_path() / ("photonstack-rejection-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    std::vector<fs::path> paths;
    for (int frame = 0; frame < 11; ++frame) {
        ImageBuffer image;
        image.width = image.height = 32;
        image.channels = 4;
        image.pixels.resize(image.sampleCount());
        for (unsigned y = 0; y < 32; ++y)
            for (unsigned x = 0; x < 32; ++x) {
                const double nebula = 70 * std::exp(-((x - 16.) * (x - 16.) + (y - 16.) * (y - 16.)) / 25);
                const float persistent = float(100 + nebula);
                const auto p = (y * 32 + x) * 4;
                for (int c = 0; c < 3; ++c)
                    image.pixels[p + c] = persistent + ((frame == 0 && x == 10) ? 3000 : 0) -
                        ((frame == 10 && x == 20) ? 1000 : 0);
                image.pixels[p + 3] = (x == 0 && frame > 2) ? 0 : 1;
            }
        const auto path = root / (std::to_string(frame) + ".fits");
        require(FitsCodec().write(image, path).ok); paths.push_back(path);
    }
    StackOptions options;
    options.method = StackMethod::SigmaClip;
    options.sigma.medianMad = true;
    options.sigma.sigmaLow = options.sigma.sigmaHigh = 3;
    options.collectRejectionMaps = true;
    const auto result = Stacker().stack(paths, options);
    require(result.ok && !result.rejectionLow.empty() && !result.rejectionHigh.empty());
    require(result.rejectedHighSamples[0] == 32 * 3 && result.rejectedLowSamples[10] == 32 * 3);
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x) {
            const float expected = float(100 + 70 * std::exp(-((x - 16.) * (x - 16.) + (y - 16.) * (y - 16.)) / 25));
            const auto p = (y * 32 + x) * 4;
            for (int c = 0; c < 3; ++c) require(std::abs(result.image.pixels[p + c] - expected) < .001);
            require(std::abs(result.rejectionHigh.pixels[p] - (x == 10 ? 1.F / 11 : 0)) < 1.e-6);
            require(std::abs(result.rejectionLow.pixels[p] - (x == 20 ? 1.F / 11 : 0)) < 1.e-6);
            require(std::abs(result.image.pixels[p + 3] - (x == 0 ? 3.F / 11 : 1)) < 1.e-6);
        }
    // Counts attach to input order, not lexical filenames or cache ordering.
    std::swap(paths[0], paths[10]);
    const auto reordered = Stacker().stack(paths, options);
    require(reordered.ok && reordered.rejectedHighSamples[10] == 32 * 3 && reordered.rejectedLowSamples[0] == 32 * 3);
    require(reordered.image.pixels == result.image.pixels);
    // Unsupported diagnostic combinations must fail rather than silently omit maps.
    options.method = StackMethod::Average;
    require(!Stacker().stack(paths, options).ok);
    options.method = StackMethod::SigmaClip;
    options.sigma.iterations = 0;
    require(!Stacker().stack(paths, options).ok);
    options.sigma.iterations = 3;
    paths.resize(1);
    require(!Stacker().stack(paths, options).ok);
}
