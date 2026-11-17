#include "BackgroundSurface.hpp"
#include "photonstack/BackgroundExtractor.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace photonstack {
namespace {
using Basis = std::array<double, 15>;
struct Sample {
    Basis basis;
    std::array<double, 3> color{};
    std::size_t measurement = 0;
};
double median(std::vector<double> v) {
    auto m = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), m, v.end());
    return *m;
}
Basis basisAt(double x, double y, const ImageBuffer& im) {
    x = 2 * x / im.width - 1;
    y = 2 * y / im.height - 1;
    return {1,
            x,
            y,
            x * x,
            x * y,
            y * y,
            x * x * x,
            x * x * y,
            x * y * y,
            y * y * y,
            x * x * x * x,
            x * x * x * y,
            x * x * y * y,
            x * y * y * y,
            y * y * y * y};
}
double evaluate(const Basis& b, const Basis& c) {
    return std::inner_product(b.begin(), b.end(), c.begin(), 0.0);
}
bool fit(const std::vector<Sample>& samples, int channel, int terms, Basis& coefficients, std::vector<bool>& keep) {
    coefficients.fill(0);
    keep.assign(samples.size(), true);
    for (int iteration = 0; iteration < 4; ++iteration) {
        double system[15][16] = {};
        std::size_t count = 0;
        for (std::size_t n = 0; n < samples.size(); ++n)
            if (keep[n]) {
                ++count;
                const auto& s = samples[n];
                for (int i = 0; i < terms; ++i) {
                    for (int j = 0; j < terms; ++j)
                        system[i][j] += s.basis[i] * s.basis[j];
                    system[i][terms] += s.basis[i] * s.color[channel];
                }
            }
        if (count < std::size_t(std::max(12, 2 * terms)))
            return false;
        for (int i = 0; i < terms; ++i) {
            int pivot = i;
            for (int j = i + 1; j < terms; ++j)
                if (std::abs(system[j][i]) > std::abs(system[pivot][i]))
                    pivot = j;
            if (std::abs(system[pivot][i]) < 1e-10)
                return false;
            for (int j = 0; j <= terms; ++j)
                std::swap(system[i][j], system[pivot][j]);
            const double divisor = system[i][i];
            for (int j = i; j <= terms; ++j)
                system[i][j] /= divisor;
            for (int k = 0; k < terms; ++k)
                if (k != i) {
                    const double factor = system[k][i];
                    for (int j = i; j <= terms; ++j)
                        system[k][j] -= factor * system[i][j];
                }
        }
        for (int i = 0; i < terms; ++i)
            coefficients[i] = system[i][terms];
        if (iteration == 3)
            break;
        std::vector<double> residuals;
        for (const auto& s : samples)
            residuals.push_back(s.color[channel] - evaluate(s.basis, coefficients));
        const double center = median(residuals);
        std::vector<double> deviations;
        for (double v : residuals)
            deviations.push_back(std::abs(v - center));
        const double threshold = std::max(1e-10, 4.4478 * median(deviations));
        for (std::size_t n = 0; n < samples.size(); ++n)
            keep[n] = std::abs(residuals[n] - center) <= threshold;
    }
    return true;
}
} // namespace
BackgroundExtractionResult BackgroundExtractor::extractPolynomial(const ImageBuffer& im,
                                                                  const BackgroundPolynomialOptions& opt) const {
    auto result = detail::measureBackgroundSamples(im, opt);
    if (!result.ok)
        return result;
    const auto fail = detail::backgroundSurfaceError;
    const int channels = std::min<int>(3, im.channels);
    std::vector<Sample> samples;
    for (std::size_t i = 0; i < result.measurements.size(); ++i) {
        const auto& m = result.measurements[i];
        if (m.status == "sampled")
            samples.push_back({basisAt(std::lround(m.point.x), std::lround(m.point.y), im), m.measured, i});
    }
    std::array<Basis, 3> coefficients;
    const int terms = int((opt.degree + 1) * (opt.degree + 2) / 2);
    if (samples.size() < std::size_t(std::max(12, 2 * terms)))
        return fail("BackgroundSamplesInsufficient",
                    "Need at least twelve sky samples and twice as many samples as polynomial coefficients");
    std::array<std::vector<bool>, 3> kept;
    for (int c = 0; c < channels; ++c)
        if (!fit(samples, c, terms, coefficients[c], kept[c]))
            return fail("BackgroundSamplesInsufficient",
                        "Sky samples do not constrain the selected polynomial background surface");
    for (std::size_t n = 0; n < samples.size(); ++n)
        for (int c = 0; c < channels; ++c)
            result.measurements[samples[n].measurement].used[c] = kept[c][n];
    return detail::applyBackgroundSurface(im, opt, std::move(result), [&](double x, double y) noexcept {
        std::array<double, 3> out{};
        const auto basis = basisAt(x, y, im);
        for (int c = 0; c < channels; ++c)
            out[c] = evaluate(basis, coefficients[c]);
        return out;
    });
}
} // namespace photonstack
