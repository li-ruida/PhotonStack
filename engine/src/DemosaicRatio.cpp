#include "DemosaicRatio.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace photonstack::detail {
namespace {
int mirror(int x, int size) {
    while (x < 0 || x >= size)
        x = x < 0 ? -x : 2 * size - 2 - x;
    return x;
}
} // namespace

// Ratio interpolation principle: https://github.com/LuisSR/RCD-Demosaicing
// (algorithm overview). This is a separate implementation with fixed direction
// weights, a guide moment correction and a measured-site flux constraint; it
// is not RCD. Fixed weights avoid steering faint signals with noisy gradients.
std::vector<float> demosaicRatio(const std::vector<float>& mosaic, int width, int height,
                                const std::array<int, 4>& colors,
                                const std::array<float, 3>& gains,
                                const std::vector<float>& baseline) {
    const auto count = mosaic.size();
    const auto color = [&](int x, int y) { return colors[(y % 2) * 2 + x % 2]; };
    const auto index = [&](int x, int y) {
        return static_cast<std::size_t>(mirror(y, height)) * width + mirror(x, width);
    };
    const bool unitGains = gains == std::array<float, 3>{1, 1, 1};
    std::vector<double> normalized;
    if (!unitGains) {
        normalized.resize(count);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto p = static_cast<std::size_t>(y) * width + x;
                normalized[p] = static_cast<double>(mosaic[p]) * gains[color(x, y)];
            }
    }
    const auto sample = [&](int x, int y) {
        const auto p = index(x, y);
        return unitGains ? static_cast<double>(mosaic[p]) : normalized[p];
    };
    std::vector<double> guide(count), green(count);
    std::vector<float> rgb(count * 3, std::numeric_limits<float>::quiet_NaN());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(y) * width + x;
            double sum = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    sum += (dx ? 1 : 2) * (dy ? 1 : 2) * sample(x + dx, y + dy);
            guide[p] = sum / 16;
        }
    // [1,2,1]/4 has variance 1/2 along each axis. Undo its second spatial
    // moment before using ratios, without fitting a stellar PSF or a strength.
    auto low = guide;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(y) * width + x;
            guide[p] = low[p] - .25 * (low[index(x - 1, y)] + low[index(x + 1, y)] +
                                      low[index(x, y - 1)] + low[index(x, y + 1)] - 4 * low[p]);
        }
    std::vector<double>().swap(low);
    const auto estimate = [&](int x, int y, int dx, int dy) {
        const double near = sample(x + dx, y + dy);
        const double center = guide[index(x, y)], far = guide[index(x + 2 * dx, y + 2 * dy)];
        if (center > 0 && far > 0)
            return near * (2 * center / (center + far));
        // Signed scientific samples cannot be normalized by a positive ratio.
        return near + (center - far) * .5;
    };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(y) * width + x;
            green[p] = color(x, y) == 1 ? sample(x, y) :
                ((estimate(x, y, -1, 0) + estimate(x, y, 1, 0)) +
                 (estimate(x, y, 0, -1) + estimate(x, y, 0, 1))) / 4;
            rgb[p * 3 + 1] = color(x, y) == 1 ? mosaic[p] : static_cast<float>(green[p] / gains[1]);
        }
    std::vector<double>().swap(guide);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(y) * width + x;
            for (int c : {0, 2}) {
                if (color(x, y) == c) {
                    rgb[p * 3 + c] = mosaic[p];
                    continue;
                }
                double residual = 0;
                if (color(x, y) == 1) {
                    const int dx = color(x ^ 1, y) == c ? 1 : 0, dy = 1 - dx;
                    residual = (sample(x - dx, y - dy) - green[index(x - dx, y - dy)] +
                                sample(x + dx, y + dy) - green[index(x + dx, y + dy)]) / 2;
                } else {
                    for (int dy : {-1, 1})
                        for (int dx : {-1, 1})
                            residual += sample(x + dx, y + dy) - green[index(x + dx, y + dy)];
                    residual /= 4;
                }
                rgb[p * 3 + c] = static_cast<float>((green[p] + residual) / gains[c]);
            }
        }
    std::vector<double>().swap(green);
    std::vector<double>().swap(normalized);

    // Remove low-frequency drift relative to the conservative linear estimate.
    // This binomial kernel annihilates CFA parity frequencies. Missing samples
    // occupy 1/2 of G and 3/4 of R/B; correcting only those sites preserves the
    // interior integrated flux of the baseline while retaining measured values.
    std::vector<double> difference(count), horizontal(count), smooth(count);
    constexpr int kernel[5]{1, 4, 6, 4, 1};
    for (int c = 0; c < 3; ++c) {
        for (std::size_t p = 0; p < count; ++p) {
            if (!std::isfinite(rgb[p * 3 + c]))
                rgb[p * 3 + c] = baseline[p * 4 + c];
            difference[p] = std::isfinite(rgb[p * 3 + c]) && std::isfinite(baseline[p * 4 + c])
                ? static_cast<double>(rgb[p * 3 + c]) - baseline[p * 4 + c] : 0;
        }
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                double sum = 0;
                for (int j = 0; j < 5; ++j)
                    sum += kernel[j] * difference[index(x + j - 2, y)];
                horizontal[static_cast<std::size_t>(y) * width + x] = sum / 16;
            }
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                double sum = 0;
                for (int j = 0; j < 5; ++j)
                    sum += kernel[j] * horizontal[index(x, y + j - 2)];
                smooth[static_cast<std::size_t>(y) * width + x] = sum / 16;
            }
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto p = static_cast<std::size_t>(y) * width + x;
                if (color(x, y) == c) {
                    rgb[p * 3 + c] = mosaic[p];
                    continue;
                }
                const double value = rgb[p * 3 + c] - (c == 1 ? 2. : 4. / 3.) * smooth[p];
                rgb[p * 3 + c] = std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max()
                    ? static_cast<float>(value) : baseline[p * 4 + c];
            }
    }
    return rgb;
}
} // namespace photonstack::detail
