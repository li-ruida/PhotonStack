#include "photonstack/FrequencyCoadd.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
using namespace photonstack;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
template <class F> void rejects(F f, const char* message) {
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    require(false, message);
}
void close(const std::vector<double>& a, const std::vector<double>& b, double tolerance, const char* message) {
    require(a.size() == b.size(), message);
    for (std::size_t i = 0; i < a.size(); ++i)
        require(std::abs(a[i] - b[i]) <= tolerance, message);
}
void identity(std::size_t w, std::size_t h) {
    FrequencyCoadd acc(w, h);
    std::vector<double> image(w * h);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x)
            image[y * w + x] = -300 + 100 * double(x) + 23 * double(y) + 4000 * std::sin(.23 * x + .17 * y);
    const std::vector<double> delta{1};
    auto P = acc.psfTransfer(delta, 1, 1);
    auto noise = acc.noisePower({});
    acc.add(image, P, 1, 4, noise.values);
    auto result = acc.render(P);
    close(result, image, 1e-9, "identity preserves signed HDR units, even for mixed/odd FFT dimensions");
    const auto before = acc.numerator();
    const auto info = acc.information();
    const auto count = acc.frameCount();
    auto invalid = image;
    invalid[0] = std::numeric_limits<double>::infinity();
    rejects([&] { acc.add(invalid, P, 1, 4, noise.values); }, "nonfinite input rejected");
    rejects([&] { acc.add(image, P, 0, 4, noise.values); }, "zero flux rejected");
    rejects([&] { acc.add(image, P, 1, std::numeric_limits<double>::min(), noise.values); },
            "overflow contribution rejected");
    require(acc.numerator() == before && acc.information() == info && acc.frameCount() == count,
            "failed additions leave exact state unchanged");
    auto badPower = noise.values;
    badPower[0] = 0;
    rejects([&] { acc.add(image, P, 1, 4, badPower); }, "zero power rejected");
    auto target = P;
    target[0] = 2;
    rejects([&] { (void)acc.render(target); }, "output must preserve DC");
    auto response = acc.noiseMatchedResponse(noise.values);
    close(acc.render(response), image, 1e-9, "unit PSF with white noise has identity proper response");
}
} // namespace
int main() {
    identity(1, 1);
    identity(7, 9);
    identity(24, 32);
    identity(31, 17);
    rejects([] { FrequencyCoadd a(0, 3); }, "zero grid rejected");
    rejects([] { FrequencyCoadd a(20000, 2); }, "oversized grid rejected");
    FrequencyCoadd all(24, 18), first(24, 18), second(24, 18);
    auto P = all.psfTransfer(std::vector<double>{1}, 1, 1);
    auto N = all.noisePower({});
    std::vector<double> a(24 * 18), b(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = -100 + double(i);
        b[i] = 4 * a[i] + 20;
    }
    all.add(a, P, 1, 4, N.values);
    all.add(b, P, 2, 9, N.values);
    first.add(a, P, 1, 4, N.values);
    second.add(b, P, 2, 9, N.values);
    first.merge(second);
    require(first.frameCount() == 2 && first.numerator() == all.numerator() && first.information() == all.information(),
            "split merge reproduces sequential accumulation exactly");
    std::vector<double> expected(a.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        expected[i] = (a[i] / 4 + 2 * b[i] / 9) / (1. / 4 + 4. / 9);
    close(all.render(P), expected, 1e-10, "unequal flux and variance recover analytic inverse-variance estimate");
    auto frozen = all.numerator();
    rejects([&] { all.merge(FrequencyCoadd(23, 18)); }, "merge shape mismatch rejected");
    require(all.numerator() == frozen, "failed merge leaves state unchanged");
    // Known integer phase: exposure is shifted right, PSF places the same shift.
    std::vector<double> kernel(25);
    kernel[12] = 1;
    auto shifted = all.psfTransfer(kernel, 5, 5, 1, 0);
    std::vector<double> original(24 * 18), moving(original.size());
    for (std::size_t y = 0; y < 18; ++y)
        for (std::size_t x = 0; x < 24; ++x)
            original[y * 24 + x] =
                std::cos(2 * std::numbers::pi * x / 24) + .3 * std::sin(4 * std::numbers::pi * y / 18);
    for (std::size_t y = 0; y < 18; ++y)
        for (std::size_t x = 0; x < 24; ++x)
            moving[y * 24 + x] = original[y * 24 + (x + 23) % 24];
    FrequencyCoadd phase(24, 18);
    phase.add(moving, shifted, 1, 1, N.values);
    close(phase.render(P), original, 1e-12, "PSF phase convention recovers a known shifted scene");
    auto fractional = phase.psfTransfer(kernel, 5, 5, .25, -.3);
    require(std::abs(fractional[0] - std::complex<double>(1, 0)) < 1e-12, "fractional PSF preserves DC");
    phase.add(moving, fractional, 1, 1, N.values);
    // Missing frequency is reported, never fabricated by a denominator floor.
    FrequencyCoadd missing(24, 18);
    auto zero = P;
    zero[3] = 0;
    missing.add(original, zero, 1, 1, N.values);
    rejects([&] { (void)missing.render(P); }, "unsupported target frequency rejected");
    zero[3] = 0;
    require(missing.render(zero).size() == original.size(), "zero target at unsupported bin allowed");
    std::vector<NoiseCorrelation> correlations{{1, 0, .4}, {0, 1, .3}};
    auto colored = all.noisePower(correlations);
    require(colored.flooredBins > 0 && std::abs(colored.values[0] - 2.4) < 1e-12, "correlated PSD and explicit floor");
    FrequencyCoadd coloredSingle(24, 18, 2);
    auto bright = original;
    for (auto& value : bright) value *= 3;
    coloredSingle.add(bright, P, 3, 9, colored.values);
    const auto matched = coloredSingle.noiseMatchedResponse(colored.values);
    close(coloredSingle.render(matched), original, 1e-12,
          "noise-matched response preserves the scene for a known colored-noise unit PSF");
    auto invalidColored = colored.values;
    invalidColored[13] *= 2;
    rejects([&] { coloredSingle.add(bright, P, 3, 9, invalidColored); },
            "asymmetric real-noise boundary is rejected");
    for (std::size_t y = 0; y < 18; ++y)
        for (std::size_t x = 0; x < 13; ++x) {
            double fy = y < 9 ? double(y) / 18 : (double(y) - 18) / 18;
            double value = std::max(.05, 1 + .8 * std::cos(2 * std::numbers::pi * x / 24) +
                                             .6 * std::cos(2 * std::numbers::pi * fy));
            require(std::abs(colored.values[y * 13 + x] - value) < 1e-12,
                    "noise spectrum matches analytic covariance transform");
        }
    rejects([&] { (void)all.noisePower(std::vector<NoiseCorrelation>{{1, 0, .2}, {-1, 0, .3}}); },
            "duplicate opposite lag rejected");
    auto illegal = P;
    illegal[13] = std::complex<double>(1, 1);
    rejects([&] { all.add(a, illegal, 1, 1, N.values); }, "complex boundary not representing a real image rejected");
    std::cout << "frequency coadd checks passed\n";
}
