#include "photonstack/StarColorCalibrator.hpp"
#include "MaskedImageSampling.hpp"
#include <algorithm>
#include <cmath>

namespace photonstack {
namespace {
double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    const double p = q * (v.size() - 1);
    const auto lo = static_cast<std::size_t>(p), hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (p - lo);
}
bool validImage(const ImageBuffer& im) {
    return !im.empty() && (im.channels == 3 || im.channels == 4) && im.pixels.size() == im.sampleCount() &&
           im.colorEncoding == ColorEncoding::Linear;
}
bool solve(double a[3][4], std::array<double, 3>& out) {
    for (int c = 0; c < 3; ++c) {
        int p = c;
        for (int r = c + 1; r < 3; ++r) if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
        if (std::abs(a[p][c]) < 1e-10) return false;
        for (int k = c; k < 4; ++k) std::swap(a[p][k], a[c][k]);
        const double d = a[c][c];
        for (int k = c; k < 4; ++k) a[c][k] /= d;
        for (int r = 0; r < 3; ++r) if (r != c) {
            const double f = a[r][c];
            for (int k = c; k < 4; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int c = 0; c < 3; ++c) out[c] = a[c][3];
    return true;
}
bool aperture(const ImageBuffer& im, double x, double y, double saturation, std::array<double, 3>& flux) {
    if (!std::isfinite(x) || !std::isfinite(y) || x < 17 || y < 17 ||
        x >= static_cast<double>(im.width) - 17 || y >= static_cast<double>(im.height) - 17) return false;
    const int cx = std::lround(x), cy = std::lround(y);
    struct Sample { std::array<double, 3> basis, rgb; bool sky, source; };
    std::vector<Sample> samples;
    int area = 0;
    for (int dy = -16; dy <= 16; ++dy) for (int dx = -16; dx <= 16; ++dx) {
        const double rx = cx + dx - x, ry = cy + dy - y, rr = rx * rx + ry * ry;
        const bool sky = rr >= 121 && rr <= 225, source = rr <= 64;
        if (!sky && !source) continue;
        const auto p = static_cast<std::size_t>(cy + dy) * im.width + cx + dx;
        if (!detail::pixelHasValidColor(im, p) || detail::pixelCoverage(im, p) < .999999F) return false;
        Sample s{{1, rx, ry}, {}, sky, source};
        for (int c = 0; c < 3; ++c) {
            s.rgb[c] = im.pixels[p * im.channels + c];
            if (s.rgb[c] >= saturation) return false;
        }
        samples.push_back(s);
        area += source;
    }
    for (int c = 0; c < 3; ++c) {
        double matrix[3][4]{};
        for (const auto& s : samples) if (s.sky)
            for (int a = 0; a < 3; ++a) {
                matrix[a][3] += s.basis[a] * s.rgb[c];
                for (int b = 0; b < 3; ++b) matrix[a][b] += s.basis[a] * s.basis[b];
            }
        std::array<double, 3> beta{};
        if (!solve(matrix, beta)) return false;
        std::vector<double> residuals;
        flux[c] = 0;
        for (const auto& s : samples) {
            const double value = s.rgb[c] - beta[0] - beta[1] * s.basis[1] - beta[2] * s.basis[2];
            if (s.sky) residuals.push_back(value);
            if (s.source) flux[c] += value;
        }
        const double center = quantile(residuals, .5);
        for (auto& v : residuals) v = std::abs(v - center);
        const double sigma = 1.4826 * quantile(residuals, .5);
        if (!(flux[c] > 30 * sigma * std::sqrt(static_cast<double>(area))) || !std::isfinite(flux[c]))
            return false;
    }
    return true;
}
} // namespace

StarColorCalibrationResult StarColorCalibrator::estimate(const ImageBuffer& input, const ImageBuffer& reference,
    const std::vector<StarColorPair>& pairs, const StarColorCalibrationOptions& options) const {
    StarColorCalibrationResult result;
    auto fail = [&](const char* code, const char* message) {
        result.errorCode = code; result.message = message; return result;
    };
    if (!validImage(input) || !validImage(reference))
        return fail("ImageBufferInvalid", "Color calibration requires two non-empty linear RGB images");
    if (!(options.inputSaturation > 0) || !(options.referenceSaturation > 0))
        return fail("ArgumentInvalid", "Saturation limits must be positive");
    auto sorted = pairs;
    for (const auto& p : sorted) for (float v : p) if (!std::isfinite(v))
        return fail("ArgumentInvalid", "Matched star coordinates must be finite");
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a[1] < b[1] || (a[1] == b[1] && a[0] < b[0]);
    });
    std::vector<StarColorPair> accepted;
    std::array<std::vector<double>, 3> training, validation;
    for (const auto& p : sorted) {
        // Prevent duplicated sources and overlapping source/sky measurements.
        const bool overlap = std::any_of(accepted.begin(), accepted.end(), [&](const auto& a) {
            return std::hypot(a[0] - p[0], a[1] - p[1]) < 32 || std::hypot(a[2] - p[2], a[3] - p[3]) < 32;
        });
        std::array<double, 3> in{}, ref{};
        if (overlap || !aperture(input, p[0], p[1], options.inputSaturation, in) ||
            !aperture(reference, p[2], p[3], options.referenceSaturation, ref)) {
            ++result.rejectedStars; continue;
        }
        auto& split = accepted.size() % 2 == 0 ? training : validation;
        for (int c = 0; c < 3; ++c) split[c].push_back(std::log(ref[c] / ref[1]) - std::log(in[c] / in[1]));
        accepted.push_back(p);
    }
    result.trainingStars = training[0].size(); result.validationStars = validation[0].size();
    if (result.trainingStars < 12 || result.validationStars < 12)
        return fail("ColorCalibrationInsufficientStars", "Need at least 12 training and 12 independent validation stars");
    for (int c = 0; c < 3; ++c) {
        const double logGain = quantile(training[c], .5), gain = std::exp(logGain);
        if (!std::isfinite(gain) || gain < .05 || gain > 20)
            return fail("ColorCalibrationInvalidGain", "Reference-relative color gain is outside [0.05,20]");
        result.gains[c] = gain;
        std::vector<double> residuals;
        for (double v : validation[c]) residuals.push_back(logGain - v);
        result.validationMedianLogError[c] = quantile(residuals, .5);
        for (auto& v : residuals) v = std::abs(v);
        result.validationP90AbsoluteLogError[c] = quantile(residuals, .9);
    }
    if (std::max(result.validationP90AbsoluteLogError[0], result.validationP90AbsoluteLogError[2]) > .2F)
        return fail("ColorCalibrationInconsistent", "Held-out star colors do not support a single diagonal gain");
    result.ok = true;
    return result;
}
} // namespace photonstack
