#include "../../engine/src/StreamingStackAccumulator.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/FrameQualityAnalyzer.hpp"
#include "photonstack/Stacker.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <vector>

using namespace photonstack;
namespace fs = std::filesystem;
namespace {
void require(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok)
        throw std::runtime_error("Stack numerics check at line " + std::to_string(where.line()));
}

ImageBuffer frame(float value, float alpha = 1) {
    ImageBuffer image;
    image.width = 3;
    image.height = 2;
    for (int pixel = 0; pixel < 6; ++pixel) {
        image.pixels.insert(image.pixels.end(), {value, -value, value * .5F, alpha});
    }
    return image;
}

void testFileMeans(const fs::path& root) {
    const FitsCodec codec;
    std::vector<fs::path> paths{root / "a.fits", root / "b.fits", root / "c.fits"};
    const float large = std::numeric_limits<float>::max() * .75F;
    // The mean is representable even though the Float32 sum is not.
    for (const auto& path : paths)
        require(codec.write(frame(large), path).ok);
    for (const auto method : {StackMethod::Average, StackMethod::WeightedAverage}) {
        StackOptions options;
        options.method = method;
        const auto result = Stacker().stack(paths, options);
        require(result.ok);
        require(result.image.pixels == frame(large).pixels);
    }
    // Weak residuals must survive signed calibration values and input order.
    const std::array<float, 3> samples{100000000.F, 1.F, -100000000.F};
    for (int i = 0; i < 3; ++i)
        require(codec.write(frame(samples[i]), paths[i]).ok);
    std::sort(paths.begin(), paths.end());
    do {
        const auto result = Stacker().stack(paths, {});
        require(result.ok);
        for (std::size_t pixel = 0; pixel < result.image.pixelCount(); ++pixel) {
            require(result.image.pixels[pixel * 4] == 1.F / 3);
            require(result.image.pixels[pixel * 4 + 1] == -1.F / 3);
            require(result.image.pixels[pixel * 4 + 3] == 1);
        }
    } while (std::next_permutation(paths.begin(), paths.end()));

    // Match an analytical weighted mean, including partial/missing coverage.
    auto a = frame(large, .25F), b = frame(large * .5F), c = frame(-large);
    a.pixels[7] = b.pixels[7] = c.pixels[7] = 0;
    a.pixels[11] = b.pixels[11] = 0;
    a.pixels[8] = std::numeric_limits<float>::quiet_NaN(); // masked color is irrelevant
    const std::array<ImageBuffer, 3> images{a, b, c};
    for (int i = 0; i < 3; ++i)
        require(codec.write(images[i], paths[i]).ok);
    for (const auto method : {StackMethod::Average, StackMethod::WeightedAverage}) {
        StackOptions options;
        options.method = method;
        options.frameWeights = {1, 2, 4};
        const auto result = Stacker().stack(paths, options);
        require(result.ok);
        double sum = 0, coverage = 0, total = 0;
        for (int i = 0; i < 3; ++i) {
            const auto quality = FrameQualityAnalyzer().analyze(images[i]);
            const double weight =
                double(options.frameWeights[i]) / 4 *
                (method == StackMethod::WeightedAverage && quality.ok ? std::max(.01F, quality.score) : 1.F);
            sum += double(images[i].pixels[0]) * images[i].pixels[3] * weight;
            coverage += images[i].pixels[3] * weight;
            total += weight;
        }
        require(result.image.pixels[0] == static_cast<float>(sum / coverage));
        require(result.image.pixels[3] == static_cast<float>(coverage / total));
        require(result.image.pixels[4] == 0 && result.image.pixels[7] == 0);
        require(result.image.pixels[8] == -large);
    }
    StackOptions invalid;
    invalid.frameWeights = {std::numeric_limits<float>::denorm_min(), 1, std::numeric_limits<float>::max()};
    const auto rejected = Stacker().stack(paths, invalid);
    require(!rejected.ok && rejected.errorCode == "ArgumentInvalid");
}

void testRowCoverage() {
    // This is the same row accumulator used by registered and cached frames.
    auto image = frame(100000000.F);
    detail::StreamingStackAccumulator accumulator(image, StackMethod::Average);
    for (const float value : {100000000.F, 1.F, -100000000.F}) {
        const auto input = frame(value);
        for (unsigned row = 0; row < image.height; ++row)
            require(accumulator.addRow(row, input.pixels.data() + row * 12, 12, 1));
    }
    require(!accumulator.addRow(2, image.pixels.data(), 12, 1));
    require(!accumulator.addRow(0, image.pixels.data(), 11, 1));
    accumulator.beginFinalization();
    for (unsigned row = 0; row < image.height; ++row)
        accumulator.finalizeRow(row, 3);
    require(accumulator.takeImage().pixels == frame(1.F / 3).pixels);

    // Non-alpha buffers still need per-pixel denominators for absent samples.
    ImageBuffer gray;
    gray.width = 2;
    gray.height = 1;
    gray.channels = 1;
    gray.format = PixelFormat::Float32Gray;
    gray.pixels = {8, 4};
    detail::StreamingStackAccumulator grayAccumulator(gray, StackMethod::Average);
    const std::array<float, 2> second{std::numeric_limits<float>::quiet_NaN(), 8};
    require(grayAccumulator.addRow(0, gray.pixels.data(), 2, 1));
    require(grayAccumulator.addRow(0, second.data(), 2, 1));
    grayAccumulator.beginFinalization();
    grayAccumulator.finalizeRow(0, 2);
    require(grayAccumulator.takeImage().pixels == std::vector<float>({8, 6}));
}
} // namespace

int main() {
    const auto root =
        fs::temp_directory_path() /
        ("photonstack-numerics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(path, e);
        }
    } cleanup{root};
    testFileMeans(root);
    testRowCoverage();
}
