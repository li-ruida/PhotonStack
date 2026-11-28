#include "BackgroundSurface.hpp"
#include <array>
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif
#include <numeric>
#include <stdexcept>

namespace photonstack {
namespace {
double kernel(double dx, double dy) noexcept {
    const double r2 = dx * dx + dy * dy;
    return r2 > 0 ? .5 * r2 * std::log(r2) : 0.;
}
struct Point {
    double x, y;
    std::array<double, 3> color;
    std::size_t measurement;
};

// Partial-pivot LU. Inverse diagonal is obtained with triangular solves, and
// is used for leave-one-out residuals, not inversion of the image model.
class Factor {
    std::size_t n_;
    std::vector<double> lu_;
    std::vector<std::size_t> pivots_;

  public:
    Factor(std::size_t n, std::vector<double> matrix) : n_(n), lu_(std::move(matrix)), pivots_(n) {
        double scale = 0;
        for (double v : lu_)
            scale = std::max(scale, std::abs(v));
        for (std::size_t k = 0; k < n_; ++k) {
            auto pivot = k;
            for (auto i = k + 1; i < n_; ++i)
                if (std::abs(lu_[i * n_ + k]) > std::abs(lu_[pivot * n_ + k]))
                    pivot = i;
            if (std::abs(lu_[pivot * n_ + k]) <= 1e-12 * std::max(scale, 1.))
                throw std::runtime_error("Sky samples are duplicate, collinear or numerically ill-conditioned");
            pivots_[k] = pivot;
            if (k != pivot)
                for (std::size_t j = 0; j < n_; ++j)
                    std::swap(lu_[k * n_ + j], lu_[pivot * n_ + j]);
            for (auto i = k + 1; i < n_; ++i) {
                auto& multiplier = lu_[i * n_ + k];
                multiplier /= lu_[k * n_ + k];
                for (auto j = k + 1; j < n_; ++j)
                    lu_[i * n_ + j] -= multiplier * lu_[k * n_ + j];
            }
        }
    }
    std::vector<double> solve(std::vector<double> rhs) const {
        for (std::size_t k = 0; k < n_; ++k) {
            std::swap(rhs[k], rhs[pivots_[k]]);
        }
        for (std::size_t i = 0; i < n_; ++i)
            for (std::size_t j = 0; j < i; ++j)
                rhs[i] -= lu_[i * n_ + j] * rhs[j];
        for (std::size_t i = n_; i-- > 0;) {
            for (auto j = i + 1; j < n_; ++j)
                rhs[i] -= lu_[i * n_ + j] * rhs[j];
            rhs[i] /= lu_[i * n_ + i];
        }
        return rhs;
    }
    std::vector<double> inverseDiagonal(std::size_t count) const {
        std::vector<double> out(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::vector<double> unit(n_);
            unit[i] = 1;
            out[i] = solve(std::move(unit))[i];
            if (!std::isfinite(out[i]) || out[i] <= 1e-12)
                throw std::runtime_error("Sky samples do not constrain leave-one-out validation");
        }
        return out;
    }
};
struct Fit {
    std::vector<double> weights, loo;
    std::array<double, 3> plane{};
};
Fit fit(const std::vector<Point>& points, const std::vector<bool>& kept, int channel, double smoothing) {
    std::vector<std::size_t> selected;
    for (std::size_t i = 0; i < points.size(); ++i)
        if (kept[i])
            selected.push_back(i);
    const auto n = selected.size(), size = n + 3;
    if (n < 12)
        throw std::runtime_error("RBF background needs at least twelve retained sky samples");
    std::vector<double> matrix(size * size), rhs(size);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& p = points[selected[i]];
        rhs[i] = p.color[channel];
        for (std::size_t j = 0; j < n; ++j) {
            const auto& q = points[selected[j]];
            matrix[i * size + j] = kernel(p.x - q.x, p.y - q.y) + (i == j ? smoothing : 0);
        }
        const std::array<double, 3> terms{1, p.x, p.y};
        for (std::size_t j = 0; j < 3; ++j)
            matrix[i * size + n + j] = matrix[(n + j) * size + i] = terms[j];
    }
    Factor factor(size, std::move(matrix));
    const auto coefficients = factor.solve(std::move(rhs));
    const auto diagonal = factor.inverseDiagonal(n);
    Fit out;
    out.weights.resize(points.size());
    out.loo.resize(points.size());
    for (std::size_t i = 0; i < n; ++i) {
        out.weights[selected[i]] = coefficients[i];
        out.loo[selected[i]] = coefficients[i] / diagonal[i];
    }
    for (std::size_t c = 0; c < 3; ++c)
        out.plane[c] = coefficients[n + c];
    return out;
}
} // namespace
BackgroundExtractionResult BackgroundExtractor::extractRBF(const ImageBuffer& im,
                                                           const BackgroundRBFOptions& options) const {
    const auto fail = detail::backgroundSurfaceError;
    if (!std::isfinite(options.smoothing) || options.smoothing < 0 || options.smoothing > 1)
        return fail("ArgumentInvalid", "RBF smoothing must be finite and between 0 and 1");
    auto sampling = options.sampling;
    sampling.degree = 1; // Degree is specific to the polynomial model; TPS has an affine tail.
    auto result = detail::measureBackgroundSamples(im, sampling);
    if (!result.ok)
        return result;
    const double scale = std::max(im.width, im.height);
    std::vector<Point> points;
    for (std::size_t i = 0; i < result.measurements.size(); ++i) {
        const auto& m = result.measurements[i];
        if (m.status == "sampled")
            points.push_back({std::lround(m.point.x) / scale, std::lround(m.point.y) / scale, m.measured, i});
    }
    if (points.size() < 12 || points.size() > 512)
        return fail("BackgroundSamplesInsufficient",
                    "RBF requires 12...512 valid sky samples; reduce the grid or disable extra points");
    const int channels = std::min<int>(3, im.channels);
    std::array<Fit, 3> fits;
    std::array<std::vector<bool>, 3> kept;
    try {
        for (int c = 0; c < channels; ++c) {
            kept[c].assign(points.size(), true);
            for (int iteration = 0; iteration < 4; ++iteration) {
                fits[c] = fit(points, kept[c], c, options.smoothing);
                if (!options.rejectOutliers || iteration == 3)
                    break;
                std::vector<double> errors, values;
                for (std::size_t n = 0; n < points.size(); ++n)
                    if (kept[c][n]) {
                        errors.push_back(fits[c].loo[n]);
                        values.push_back(std::abs(points[n].color[c]));
                    }
                const double center = detail::backgroundMedian(errors);
                for (double& v : errors)
                    v = std::abs(v - center);
                const double threshold = std::max(5e-7 * std::max(1., detail::backgroundMedian(values)),
                                                  6.6717 * detail::backgroundMedian(errors));
                bool changed = false;
                for (std::size_t n = 0; n < points.size(); ++n)
                    if (kept[c][n] && std::abs(fits[c].loo[n] - center) > threshold) {
                        kept[c][n] = false;
                        changed = true;
                    }
                if (!changed)
                    break;
            }
        }
    } catch (const std::exception& error) {
        result.ok = false;
        result.errorCode = "BackgroundRBFUnconstrained";
        result.message = error.what();
        return result;
    }
    // Each distance/log is shared by RGB. All pixels evaluate the actual TPS;
    // no undersampled model grid or hidden approximation is used.
    const auto evaluate = [&](double x, double y) noexcept {
        x /= scale;
        y /= scale;
        std::array<double, 3> color{};
        for (int c = 0; c < channels; ++c)
            color[c] = fits[c].plane[0] + x * fits[c].plane[1] + y * fits[c].plane[2];
        for (std::size_t n = 0; n < points.size(); ++n) {
            const auto k = kernel(x - points[n].x, y - points[n].y);
            for (int c = 0; c < channels; ++c)
                color[c] += k * fits[c].weights[n];
        }
        return color;
    };
    for (std::size_t n = 0; n < points.size(); ++n) {
        auto& m = result.measurements[points[n].measurement];
        const auto prediction = evaluate(std::lround(m.point.x), std::lround(m.point.y));
        for (int c = 0; c < channels; ++c) {
            m.used[c] = kept[c][n];
            m.validationResidual[c] = m.used[c] ? fits[c].loo[n] : m.measured[c] - prediction[c];
        }
    }
#ifdef __APPLE__
    const auto block = [&](std::size_t start, std::size_t row, int count, auto& colors) noexcept {
        std::array<double, 64> x, distance, logs;
        const double y = row / scale;
        for (int i = 0; i < count; ++i) {
            x[i] = (start + i) / scale;
            for (int c = 0; c < channels; ++c)
                colors[i][c] = fits[c].plane[0] + x[i] * fits[c].plane[1] + y * fits[c].plane[2];
        }
        for (std::size_t n = 0; n < points.size(); ++n) {
            const double dy = y - points[n].y;
            for (int i = 0; i < count; ++i) {
                const double dx = x[i] - points[n].x;
                distance[i] = dx * dx + dy * dy;
                logs[i] = distance[i] > 0 ? distance[i] : 1.;
            }
            vvlog(logs.data(), logs.data(), &count);
            for (int i = 0; i < count; ++i) {
                const double k = .5 * distance[i] * logs[i];
                for (int c = 0; c < channels; ++c) colors[i][c] += k * fits[c].weights[n];
            }
        }
    };
    return detail::applyBackgroundSurface(im, sampling, std::move(result), evaluate, block);
#else
    return detail::applyBackgroundSurface(im, sampling, std::move(result), evaluate);
#endif
}
} // namespace photonstack
