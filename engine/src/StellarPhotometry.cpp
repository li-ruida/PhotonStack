#include "photonstack/StellarPhotometry.hpp"
#include "MaskedImageSampling.hpp"
#include "photonstack/StarCentroid.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
namespace photonstack {
namespace {
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return .5 * (v[(v.size() - 1) / 2] + v[v.size() / 2]);
}
bool solve(double a[3][4], std::array<double, 3>& beta) {
    for (int c = 0; c < 3; ++c) {
        int p = c;
        for (int i = c + 1; i < 3; ++i)
            if (std::abs(a[i][c]) > std::abs(a[p][c]))
                p = i;
        if (std::abs(a[p][c]) < 1e-12)
            return false;
        for (int j = c; j < 4; ++j)
            std::swap(a[p][j], a[c][j]);
        double d = a[c][c];
        for (int j = c; j < 4; ++j)
            a[c][j] /= d;
        for (int i = 0; i < 3; ++i)
            if (i != c) {
                double f = a[i][c];
                for (int j = c; j < 4; ++j)
                    a[i][j] -= f * a[c][j];
            }
    }
    for (int c = 0; c < 3; ++c)
        beta[c] = a[c][3];
    return true;
}
struct Sample {
    double x, y, weight;
    std::array<double, 3> rgb;
    bool sky;
};
} // namespace
StellarApertureResult measureStellarAperture(const ImageBuffer& im, double px, double py,
                                             const StellarApertureOptions& o) {
    if (im.empty() || (im.channels != 3 && im.channels != 4) || im.pixels.size() != im.sampleCount() ||
        im.colorEncoding != ColorEncoding::Linear)
        throw std::invalid_argument("Stellar photometry needs linear RGB");
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(o.radius) || !std::isfinite(o.skyInner) ||
        !std::isfinite(o.skyOuter) || !std::isfinite(o.saturation) || !std::isfinite(o.minSNR) ||
        !std::isfinite(o.maxCenterOffset) || o.radius < 3 || o.radius > 20 || o.skyInner <= o.radius + 1 ||
        o.skyOuter < o.skyInner + 2 || o.skyOuter > 40 || o.saturation < 0 || o.minSNR < 1 || o.maxCenterOffset <= 0 ||
        o.maxCenterOffset > 5)
        throw std::invalid_argument("Invalid stellar aperture options");
    StellarApertureResult r;
    r.x = px;
    r.y = py;
    auto fail = [&](std::string reason) {
        r.status = std::move(reason);
        return r;
    };
    if (px < o.skyOuter + 2 || py < o.skyOuter + 2 || px >= im.width - o.skyOuter - 2 ||
        py >= im.height - o.skyOuter - 2)
        return fail("edge");
    const auto center = fitStarCentroid(im, float(px), float(py));
    if (!center.ok)
        return fail("centroid-fit");
    r.x = center.x;
    r.y = center.y;
    r.centerOffset = std::hypot(r.x - px, r.y - py);
    if (r.centerOffset > o.maxCenterOffset)
        return fail("position-mismatch");
    if (r.x < o.skyOuter + 1 || r.y < o.skyOuter + 1 || r.x >= im.width - o.skyOuter - 1 ||
        r.y >= im.height - o.skyOuter - 1)
        return fail("edge");
    const int radius = int(std::ceil(o.skyOuter + 1)), cx = int(std::round(r.x)), cy = int(std::round(r.y));
    std::vector<Sample> samples;
    double squareWeight = 0;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const double x = cx + dx - r.x, y = cy + dy - r.y, d = std::hypot(x, y);
            const bool sky = d >= o.skyInner && d <= o.skyOuter;
            double weight = 0;
            if (d < o.radius - .71)
                weight = 1;
            else if (d < o.radius + .71) {
                for (unsigned j = 0; j < 8; ++j)
                    for (unsigned i = 0; i < 8; ++i)
                        if (std::hypot(x + (i + .5) / 8 - .5, y + (j + .5) / 8 - .5) <= o.radius)
                            weight += 1. / 64;
            }
            if (!sky && weight == 0)
                continue;
            const auto p = std::size_t(cy + dy) * im.width + cx + dx;
            if (!detail::pixelHasValidColor(im, p) || detail::pixelCoverage(im, p) < .999999F)
                return fail("coverage");
            Sample s{x, y, weight, {}, sky};
            for (unsigned c = 0; c < 3; ++c) {
                s.rgb[c] = im.pixels[p * im.channels + c];
                if (o.saturation > 0 && s.rgb[c] >= o.saturation)
                    return fail("saturation");
            }
            samples.push_back(s);
            r.area += weight;
            squareWeight += weight * weight;
        }
    for (unsigned c = 0; c < 3; ++c) {
        std::vector<double> weights(samples.size(), 1);
        std::array<double, 3> beta{};
        double sigma = 0;
        for (unsigned iteration = 0; iteration < 8; ++iteration) {
            double matrix[3][4]{};
            for (std::size_t i = 0; i < samples.size(); ++i)
                if (samples[i].sky) {
                    const auto& s = samples[i];
                    const std::array<double, 3> b{1, s.x, s.y};
                    for (unsigned j = 0; j < 3; ++j) {
                        matrix[j][3] += weights[i] * b[j] * s.rgb[c];
                        for (unsigned k = 0; k < 3; ++k)
                            matrix[j][k] += weights[i] * b[j] * b[k];
                    }
                }
            if (!solve(matrix, beta))
                return fail("sky-fit");
            std::vector<double> residuals;
            for (const auto& s : samples)
                if (s.sky)
                    residuals.push_back(s.rgb[c] - beta[0] - beta[1] * s.x - beta[2] * s.y);
            if (residuals.size() < 30)
                return fail("sky-samples");
            const double location = median(residuals);
            for (auto& v : residuals)
                v = std::abs(v - location);
            sigma = 1.4826 * median(residuals);
            // Absolute scale floor follows the local numeric precision, not a
            // brightness-dependent clipping threshold that suppresses dim stars.
            const double floor = std::max(1e-12, std::abs(beta[0]) * 1e-7), scale = 4.685 * std::max(sigma, floor);
            for (std::size_t i = 0; i < samples.size(); ++i)
                if (samples[i].sky) {
                    const auto& s = samples[i];
                    const double u = (s.rgb[c] - beta[0] - beta[1] * s.x - beta[2] * s.y - location) / scale;
                    weights[i] = std::abs(u) < 1 ? std::pow(1 - u * u, 2) : 0;
                }
        }
        // Refit with the final weights so the saved plane and accepted samples
        // refer to the same state, including strongly contaminated annuli.
        double matrix[3][4]{};
        std::array<double, 3> apertureBasis{};
        for (const auto& s : samples) {
            apertureBasis[0] += s.weight;
            apertureBasis[1] += s.weight * s.x;
            apertureBasis[2] += s.weight * s.y;
        }
        for (std::size_t i = 0; i < samples.size(); ++i)
            if (samples[i].sky) {
                const auto& s = samples[i];
                std::array<double, 3> b{1, s.x, s.y};
                r.skyUsed[c] += weights[i] > .01;
                for (unsigned j = 0; j < 3; ++j) {
                    matrix[j][3] += weights[i] * b[j] * s.rgb[c];
                    for (unsigned k = 0; k < 3; ++k)
                        matrix[j][k] += weights[i] * b[j] * b[k];
                }
            }
        double covarianceSystem[3][4]{};
        for (unsigned j = 0; j < 3; ++j) {
            for (unsigned k = 0; k < 3; ++k)
                covarianceSystem[j][k] = matrix[j][k];
            covarianceSystem[j][3] = apertureBasis[j];
        }
        std::array<double, 3> influence{};
        if (r.skyUsed[c] < 30 || !solve(matrix, beta) || !solve(covarianceSystem, influence))
            return fail("sky-samples");
        // A single RGB channel can clip while luminance still has a smooth
        // peak. Detect a flat core separately without inventing an ADC ceiling
        // for stacked values. Four equal samples remain valid at half phase.
        double corePeak = -std::numeric_limits<double>::infinity();
        for (const auto& s : samples)
            if (std::abs(s.x) <= 2.5 && std::abs(s.y) <= 2.5)
                corePeak = std::max(corePeak, s.rgb[c]);
        const double coreContrast = corePeak - beta[0];
        unsigned plateau = 0;
        if (coreContrast > 0)
            for (const auto& s : samples)
                if (std::abs(s.x) <= 2.5 && std::abs(s.y) <= 2.5 &&
                    std::abs(s.rgb[c] - corePeak) <= coreContrast * 1e-6)
                    ++plateau;
        if (plateau >= 5)
            return fail("clipped-channel");
        std::vector<double> residuals;
        double skyVariance = 0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto& s = samples[i];
            const double value = s.rgb[c] - beta[0] - beta[1] * s.x - beta[2] * s.y;
            r.flux[c] += s.weight * value;
            if (s.sky) {
                residuals.push_back(value);
                skyVariance += std::pow(weights[i] * (influence[0] + influence[1] * s.x + influence[2] * s.y), 2);
            }
        }
        const double location = median(residuals);
        for (auto& v : residuals)
            v = std::abs(v - location);
        sigma = 1.4826 * median(residuals);
        r.skyPlane[c] = beta;
        r.skySigma[c] = sigma;
        r.backgroundNoise[c] = sigma * std::sqrt(squareWeight + skyVariance);
        r.snr[c] = r.flux[c] / std::max(1e-30, r.backgroundNoise[c]);
        if (!std::isfinite(r.flux[c]) || !std::isfinite(r.snr[c]) || r.flux[c] <= 0 || r.snr[c] < o.minSNR)
            return fail("signal-to-noise");
    }
    r.valid = true;
    r.status = "measured";
    return r;
}
} // namespace photonstack
