#include "photonstack/StarCentroid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {
constexpr int radius = 7, side = 15, count = side * side, parameters = 9;
using Params = std::array<double, parameters>;

double median(std::vector<double> values) {
    auto mid = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
}

double evaluate(const Params& p, double x, double y, Params* jacobian = nullptr) {
    const double sx = std::exp(p[3]), sy = std::exp(p[4]);
    const double z = std::tanh(p[5]), rho = .9 * z, den = 1 - rho * rho;
    const double u = (x - p[1]) / sx, v = (y - p[2]) / sy;
    const double numerator = u * u + v * v - 2 * rho * u * v;
    const double gaussian = std::exp(-.5 * numerator / den), signal = p[0] * gaussian;
    if (jacobian) {
        *jacobian = {gaussian, signal * (u - rho * v) / (sx * den),
                     signal * (v - rho * u) / (sy * den),
                     signal * (u * u - rho * u * v) / den,
                     signal * (v * v - rho * u * v) / den,
                     signal * (u * v * den - rho * numerator) / (den * den) * .9 * (1 - z * z),
                     1, x, y};
    }
    return signal + p[6] + p[7] * x + p[8] * y;
}

bool solve(double matrix[parameters][parameters + 1], Params& result) {
    for (int col = 0; col < parameters; ++col) {
        int pivot = col;
        for (int row = col + 1; row < parameters; ++row)
            if (std::abs(matrix[row][col]) > std::abs(matrix[pivot][col])) pivot = row;
        if (std::abs(matrix[pivot][col]) < 1e-12) return false;
        for (int k = col; k <= parameters; ++k) std::swap(matrix[col][k], matrix[pivot][k]);
        const double divisor = matrix[col][col];
        for (int k = col; k <= parameters; ++k) matrix[col][k] /= divisor;
        for (int row = 0; row < parameters; ++row) {
            if (row == col) continue;
            const double factor = matrix[row][col];
            for (int k = col; k <= parameters; ++k) matrix[row][k] -= factor * matrix[col][k];
        }
    }
    for (int j = 0; j < parameters; ++j) {
        result[j] = matrix[j][parameters];
        if (!std::isfinite(result[j])) return false;
    }
    return true;
}

double error(const std::array<double, count>& data, const Params& p) {
    double sum = 0;
    for (int j = 0; j < count; ++j) {
        const double delta = data[j] - evaluate(p, j % side - radius, j / side - radius);
        sum += delta * delta;
    }
    return sum;
}
} // namespace

StarCentroidFit fitStarCentroid(const ImageBuffer& image, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y) || image.empty() ||
        (image.channels != 1 && image.channels != 3 && image.channels != 4) ||
        image.pixels.size() != image.sampleCount() || x < radius || y < radius ||
        x >= static_cast<double>(image.width) - radius - 1 ||
        y >= static_cast<double>(image.height) - radius - 1) return {};
    const int cx = static_cast<int>(std::round(x)), cy = static_cast<int>(std::round(y));
    std::array<double, count> data{};
    std::vector<double> background;
    double peak = -std::numeric_limits<double>::infinity();
    for (int j = 0; j < count; ++j) {
        const int dx = j % side - radius, dy = j / side - radius;
        const auto pixel = static_cast<std::size_t>(cy + dy) * image.width + cx + dx;
        if (!detail::pixelHasValidColor(image, pixel) || detail::pixelCoverage(image, pixel) < .999999F)
            return {};
        const auto offset = pixel * image.channels;
        data[j] = image.channels == 1 ? image.pixels[offset] :
            .2126 * image.pixels[offset] + .7152 * image.pixels[offset + 1] + .0722 * image.pixels[offset + 2];
        if (dx * dx + dy * dy > 30) background.push_back(data[j]);
        if (std::abs(dx) <= 2 && std::abs(dy) <= 2) peak = std::max(peak, data[j]);
    }
    const double sky = median(background), amplitude = peak - sky;
    if (!(amplitude > 0) || !std::isfinite(amplitude)) return {};
    int plateau = 0;
    for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx)
        plateau += std::abs(data[(dy + radius) * side + dx + radius] - peak) <= amplitude * 1e-6;
    // Four equal samples can be a valid symmetric source at half-pixel phase.
    if (plateau >= 5) return {};
    for (auto& value : data) value = (value - sky) / amplitude;
    Params p{1, x - cx, y - cy, std::log(1.4), std::log(1.4), 0, 0, 0, 0};
    double damping = .01, cost = error(data, p);
    for (int iteration = 0; iteration < 40; ++iteration) {
        double matrix[parameters][parameters + 1]{};
        for (int j = 0; j < count; ++j) {
            Params gradient;
            const double residual = data[j] - evaluate(p, j % side - radius, j / side - radius, &gradient);
            for (int row = 0; row < parameters; ++row) {
                matrix[row][parameters] += gradient[row] * residual;
                for (int col = 0; col < parameters; ++col) matrix[row][col] += gradient[row] * gradient[col];
            }
        }
        for (int j = 0; j < parameters; ++j) matrix[j][j] += damping * std::max(matrix[j][j], .01);
        Params step, next = p;
        if (!solve(matrix, step)) return {};
        double stepNorm = 0;
        for (int j = 0; j < parameters; ++j) { next[j] += step[j]; stepNorm += step[j] * step[j]; }
        next[0] = std::clamp(next[0], .1, 3.);
        for (int j : {1, 2}) next[j] = std::clamp(next[j], -3., 3.);
        for (int j : {3, 4}) next[j] = std::clamp(next[j], std::log(.5), std::log(4.));
        next[5] = std::clamp(next[5], -2., 2.);
        const double nextCost = error(data, next);
        if (nextCost < cost) {
            p = next; cost = nextCost; damping = std::max(damping / 3, 1e-7);
            if (stepNorm < 1e-10) break;
        } else { damping = std::min(damping * 10, 1e7); }
    }
    const double sx = std::exp(p[3]), sy = std::exp(p[4]), rho = .9 * std::tanh(p[5]);
    const double middle = (sx * sx + sy * sy) / 2;
    const double spread = std::hypot((sx * sx - sy * sy) / 2, rho * sx * sy);
    const double residual = std::sqrt(cost / count) / p[0];
    if (!std::isfinite(residual) || residual > .06 || middle - spread < .55 * .55 ||
        middle + spread > 3.8 * 3.8 || (middle - spread) / (middle + spread) < .10 ||
        std::hypot(p[1], p[2]) > 2.5) return {};
    return {true, static_cast<float>(cx + p[1]), static_cast<float>(cy + p[2]), static_cast<float>(residual),
            static_cast<float>(sx * sx), static_cast<float>(sy * sy), static_cast<float>(rho * sx * sy)};
}
} // namespace photonstack
