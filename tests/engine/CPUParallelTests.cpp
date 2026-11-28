#include "../../engine/src/MaskedImageSampling.hpp"
#include "photonstack/AstroDevelop.hpp"
#include "photonstack/CfaDrizzle.hpp"
#include "photonstack/ColorAdjuster.hpp"
#include "photonstack/ComaReducer.hpp"
#include "photonstack/Deconvolution.hpp"
#include "photonstack/FrequencyCoadd.hpp"
#include "photonstack/ParallelRanges.hpp"
#include "photonstack/StarDetector.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>
using namespace photonstack;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> auto serial(F compute) {
    const detail::ScopedRangeWorkerLimit one(1);
    return compute();
}
void sameImage(const ImageBuffer& a, const ImageBuffer& b) {
    require(a.width == b.width && a.height == b.height && a.channels == b.channels &&
                a.pixels.size() == b.pixels.size(),
            "parallel output shape differs");
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        require(std::bit_cast<std::uint32_t>(a.pixels[i]) == std::bit_cast<std::uint32_t>(b.pixels[i]),
                "parallel pixels differ, including masks and NaN payloads");
}
ImageBuffer scene() {
    ImageBuffer image;
    image.width = 768;
    image.height = 512;
    image.pixels.resize(image.sampleCount());
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        for (int c = 0; c < 3; ++c)
            image.pixels[p * 4 + c] = .03F + .001F * float((p * 13 + c * 7) % 17);
        image.pixels[p * 4 + 3] = p % 317 == 0 ? 0.F : p % 101 == 0 ? .5F : 1.F;
    }
    // Equal-flux compact peaks, elongated stars and overlapping footprints.
    for (int y = 20; y < 492; y += 27)
        for (int x = 20; x < 748; x += 29)
            for (int dy = -5; dy <= 5; ++dy)
                for (int dx = -7; dx <= 7; ++dx)
                    for (int c = 0; c < 3; ++c)
                        image.pixels[((y + dy) * image.width + x + dx) * 4 + c] +=
                            .5F * std::exp(-float(dx * dx) / 10.F - float(dy * dy) / 3.F);
    return image;
}
void sameStars(const StarDetectionResult& a, const StarDetectionResult& b) {
    require(a.ok && b.ok && a.detectedCount == b.detectedCount && a.stars.size() == b.stars.size(),
            "parallel star count differs");
    for (std::size_t i = 0; i < a.stars.size(); ++i) {
        const auto fields = [](const Star& s) { return std::tie(s.x, s.y, s.flux, s.peak, s.fwhm, s.eccentricity); };
        require(fields(a.stars[i]) == fields(b.stars[i]), "parallel star order or measurements differ");
    }
}
} // namespace
int main() {
    // Uniform-coverage median selection must retain the weighted API's exact
    // interpolation, including cancellation at an odd-population rank.
    for (const unsigned n : {1U, 2U, 3U, 257U, 4096U}) {
        ImageBuffer plain;
        plain.width = n;
        plain.height = 1;
        plain.pixels.resize(plain.sampleCount());
        std::vector<detail::WeightedValue> reference;
        for (unsigned p = 0; p < n; ++p) {
            const float value = n == 3 ? std::array<float, 3>{-1e30F, 1e-10F, 1.F}[p] : float((p * 17) % 31) / 31.F;
            for (int c = 0; c < 3; ++c)
                plain.pixels[p * 4 + c] = value;
            plain.pixels[p * 4 + 3] = 1;
            reference.push_back({value, 1});
        }
        const auto result = ColorAdjuster().neutralizeBackground(plain);
        require(result.ok && result.redBackground == static_cast<float>(detail::weightedMedian(reference)),
                "uniform median changed weighted interpolation semantics");
    }
    auto image = scene();
    for (bool robust : {false, true}) {
        const StarDetectionOptions options{.maxStars = 97, .robustStatistics = robust, .countAllDetections = true};
        const auto reference = serial([&] { return StarDetector().detect(image, options); });
        for (int repeat = 0; repeat < 3; ++repeat)
            sameStars(reference, StarDetector().detect(image, options));
    }
    // The first raster error wins even when later rows finish earlier.
    auto invalid = image;
    invalid.pixels[100 * 4] = std::numeric_limits<float>::infinity();
    invalid.pixels[300000 * 4 + 3] = std::numeric_limits<float>::quiet_NaN();
    require(StarDetector().detect(invalid).message == "Covered image colors must be finite",
            "invalid pixel precedence changed");
    invalid.pixels[100 * 4 + 3] = std::numeric_limits<float>::quiet_NaN();
    require(StarDetector().detect(invalid).message == "Input image alpha must be finite",
            "same-pixel alpha precedence changed");
    for (int operation = 0; operation < 3; ++operation) {
        auto display = image;
        display.colorEncoding = ColorEncoding::SRGB;
        const auto compute = [&] {
            if (operation == 0)
                return ColorAdjuster().neutralizeBackground(display);
            if (operation == 1)
                return ColorAdjuster().adjustSaturation(display);
            return ColorAdjuster().suppressGreenCast(display, {.averageNeutral = true, .preserveLightness = true});
        };
        const auto a = serial(compute), b = compute();
        require(a.ok && b.ok && a.affectedPixels == b.affectedPixels && a.redBackground == b.redBackground &&
                    a.greenBackground == b.greenBackground && a.blueBackground == b.blueBackground,
                "color statistics differ");
        sameImage(a.image, b.image);
    }
    const auto deconvolve = [&] {
        return Deconvolution().richardsonLucy(image, {.iterations = 3, .clampOutput = false});
    };
    const auto rlA = serial(deconvolve), rlB = deconvolve();
    require(rlA.ok && rlB.ok, "deconvolution failed");
    sameImage(rlA.image, rlB.image);
    const auto coma = [&] { return ComaReducer().reduce(image); };
    const auto comaA = serial(coma), comaB = coma();
    require(comaA.ok && comaB.ok && comaA.correctedStars == comaB.correctedStars &&
                comaA.averageEccentricity == comaB.averageEccentricity,
            "coma statistics differ");
    sameImage(comaA.image, comaB.image);
    const auto develop = [&] { return AstroDevelop().apply(image, {.stellarBalance = false, .starExposure = .7F}); };
    const auto devA = serial(develop), devB = develop();
    require(devA.ok && devB.ok && devA.adjustedStars == devB.adjustedStars && devA.sky == devB.sky &&
                devA.toneScale == devB.toneScale && devA.nearWhitePixels == devB.nearWhitePixels,
            "develop statistics differ");
    sameImage(devA.image, devB.image);
    std::vector<float> samples(image.pixelCount());
    for (std::size_t p = 0; p < samples.size(); ++p)
        samples[p] = image.pixels[p * 4];
    CfaDrizzleFrame frame{
        .width = image.width, .height = image.height, .samples = samples, .transform = {1, .001, -.002, 1, .3, -.2}};
    const auto drizzle = [&] {
        CfaDrizzleAccumulator acc({.width = image.width, .height = image.height});
        acc.add(frame);
        return acc;
    };
    const auto cfaA = serial(drizzle), cfaB = drizzle();
    require(std::equal(cfaA.weightedSums().begin(), cfaA.weightedSums().end(), cfaB.weightedSums().begin()) &&
                std::equal(cfaA.weights().begin(), cfaA.weights().end(), cfaB.weights().begin()),
            "CFA sum order differs");
    sameImage(cfaA.image(), cfaB.image());
    auto unchanged = drizzle();
    const std::vector<double> before(unchanged.weightedSums().begin(), unchanged.weightedSums().end());
    frame.gain.fill(std::numeric_limits<double>::max());
    bool rejected = false;
    try {
        unchanged.add(frame);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && std::equal(before.begin(), before.end(), unchanged.weightedSums().begin()),
            "CFA preflight must be transactional");
    // Auto FFT parallelism must agree with an explicit single-thread transform.
    FrequencyCoadd fftA(768, 512, 1), fftB(768, 512);
    std::vector<double> plane(samples.begin(), samples.end());
    const auto transfer = fftA.psfTransfer(std::vector<double>{1}, 1, 1);
    const auto power = fftA.noisePower({});
    fftA.add(plane, transfer, 1, 2, power.values);
    fftB.add(plane, transfer, 1, 2, power.values);
    require(fftA.numerator() == fftB.numerator() && fftA.information() == fftB.information(),
            "FFT auto changes accumulation");
    require(fftA.render(transfer) == fftB.render(transfer), "FFT auto changes reconstructed pixels");
    std::cout << "CPU serial/parallel scientific parity passed\n";
    return 0;
}
