#include "photonstack/FitsCodec.hpp"
#include "photonstack/DeepSkyWorkflow.hpp"
#include "photonstack/Stacker.hpp"

#include <array>
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

void testSparseColourExcursions(const fs::path& root) {
    // Rounded, zero-mean residuals from one fully covered, registered M31 sky
    // pixel in 20 exposures. Mirror their signs and rotate channels so a fix
    // that merely suppresses blue or clamps only the high tail cannot pass.
    const std::array<int, 20> noise{-197,106,22,114,64,165,63,100,125,208,
        167,-168,-359,-306,150,132,-165,84,-90,-215};
    std::vector<fs::path> paths;
    for (std::size_t i = 0; i < noise.size(); ++i) {
        ImageBuffer image;
        image.width = 8; image.height = 1; image.channels = 4;
        image.pixels.assign(image.sampleCount(), 2000);
        for (std::size_t p = 0; p < image.pixelCount(); ++p) image.pixels[p * 4 + 3] = 1;
        for (int c = 0; c < 3; ++c) {
            image.pixels[c * 4 + c] += static_cast<float>(noise[i]);
            image.pixels[(c + 3) * 4 + c] -= static_cast<float>(noise[i]);
        }
        // Constant blue and red source signals, each with a single transient.
        image.pixels[6 * 4 + 2] = 5000 + (i == 0 ? 10000 : 0);
        image.pixels[7 * 4] = 5000 - (i == 1 ? 9000 : 0);
        const auto path = root / ("colour-tails-" + std::to_string(i) + ".fits");
        require(FitsCodec().write(image, path).ok);
        paths.push_back(path);
    }

    auto options = DeepSkyWorkflowOptions{}.stack;
    options.alignSimilarity = false;
    options.normalizeBackground = false;
    for (auto method : {StackMethod::WinsorizedSigmaClip, StackMethod::SigmaClip}) {
        options.method = method;
        options.sigma.medianMad = method == StackMethod::SigmaClip;
        const auto result = Stacker().stack(paths, options);
        require(result.ok);
        // Recomputing MAD after clipping used to move +/-101 ADU away from
        // the known neutral mean. Allow explicit hard rejection more latitude
        // than Winsorization, which retains the bounded tail's weight.
        const double tolerance = options.sigma.medianMad ? 50 : 5;
        for (int p = 0; p < 6; ++p)
            for (int c = 0; c < 3; ++c)
                require(std::abs(result.image.pixels[p * 4 + c] - 2000) < tolerance);
        for (int c = 0; c < 3; ++c) {
            require(std::abs(result.image.pixels[6 * 4 + c] - (c == 2 ? 5000 : 2000)) < .001);
            require(std::abs(result.image.pixels[7 * 4 + c] - (c == 0 ? 5000 : 2000)) < .001);
        }
        require(std::abs(result.rejectionHigh.pixels[6 * 4 + 2] - 1.F / 20) < 1.e-6F);
        require(std::abs(result.rejectionLow.pixels[7 * 4] - 1.F / 20) < 1.e-6F);
    }
}

int main() {
    const auto root = fs::temp_directory_path() / ("photonstack-rejection-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    testSparseColourExcursions(root);
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
