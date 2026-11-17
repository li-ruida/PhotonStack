#include "photonstack/SkyProjection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace photonstack {
namespace {
using Matrix = std::array<double, 9>;
constexpr double rad = std::numbers::pi / 180;
constexpr auto nan = std::numeric_limits<double>::quiet_NaN();

// Rows are unit vectors in the east, north, and tangent-point directions.
Matrix basis(const TanGrid& g) {
    const double a = g.ra * rad, d = g.dec * rad;
    return {-sin(a), cos(a), 0, -sin(d) * cos(a), -sin(d) * sin(a), cos(d), cos(d) * cos(a), cos(d) * sin(a), sin(d)};
}

Matrix transform(const TanGrid& from, const TanGrid& to) {
    const auto f = basis(from), t = basis(to);
    Matrix m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                m[3 * i + j] += t[3 * i + k] * f[3 * j + k];
    // Combine CD, the tangent-plane rotation, and inverse destination CD once.
    const auto& c = from.cd;
    for (int i : {0, 3, 6}) {
        const double u = m[i], v = m[i + 1];
        m[i] = (u * c[0] + v * c[2]) * rad;
        m[i + 1] = (u * c[1] + v * c[3]) * rad;
    }
    const auto& o = to.cd;
    const double det = (o[0] * o[3] - o[1] * o[2]) * rad;
    for (int i = 0; i < 3; ++i) {
        const double u = m[i], v = m[i + 3];
        m[i] = (o[3] * u - o[1] * v) / det;
        m[i + 3] = (-o[2] * u + o[0] * v) / det;
    }
    return m;
}

SkyPoint project(const Matrix& m, double x, double y) {
    const double z = m[6] * x + m[7] * y + m[8];
    if (!(z > 1e-8))
        return {nan, nan}; // outside the visible hemisphere
    return {(m[0] * x + m[1] * y + m[2]) / z, (m[3] * x + m[4] * y + m[5]) / z};
}
} // namespace

SkyProjection::SkyProjection(const SolvedPanel& panel, const TanGrid& output)
    : panel_(panel.grid), output_(output), forward_(transform(panel_, output)), inverse_(transform(output, panel_)),
      terms_(panel.distortion) {
    for (const auto& term : terms_) {
        if (term.i > 5 || term.j > 5 || term.i + term.j > 5 || !std::isfinite(term.a) || !std::isfinite(term.b))
            throw std::invalid_argument("Invalid SIP term for sky projection");
    }
}

SkyPoint SkyProjection::distortion(double x, double y) const {
    // Most camera solutions are quadratic. Avoid pow() inside the resampling loop.
    std::array<double, 6> px{1, x}, py{1, y};
    unsigned nx = 1, ny = 1;
    SkyPoint d{};
    for (auto [i, j, a, b] : terms_) {
        while (nx < i) {
            px[nx + 1] = px[nx] * x;
            ++nx;
        }
        while (ny < j) {
            py[ny + 1] = py[ny] * y;
            ++ny;
        }
        const double p = px[i] * py[j];
        d.x += a * p;
        d.y += b * p;
    }
    return d;
}

SkyPoint SkyProjection::toCanvas(double x, double y) const {
    x += 1 - panel_.x;
    y += 1 - panel_.y;
    const auto d = distortion(x, y);
    const auto p = project(forward_, x + d.x, y + d.y);
    return {p.x + output_.x - 1, p.y + output_.y - 1};
}

SkyPoint SkyProjection::toPanel(double x, double y) const {
    const auto target = project(inverse_, x + 1 - output_.x, y + 1 - output_.y);
    double u = target.x, v = target.y;
    if (!terms_.empty()) {
        bool converged = false;
        for (int iter = 0; iter < 24; ++iter) {
            const auto d = distortion(u, v);
            const double du = target.x - u - d.x, dv = target.y - v - d.y;
            if (std::max(std::abs(du), std::abs(dv)) < 1e-6) {
                converged = true;
                break;
            }
            u += du;
            v += dv;
        }
        if (!converged)
            return {nan, nan};
    }
    return {u + panel_.x - 1, v + panel_.y - 1};
}
} // namespace photonstack
