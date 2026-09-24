// C++ adaptation of Colour - Demosaicing's Menon (2007) DDFAPD implementation.
// Copyright 2015 Colour Developers. BSD-3-Clause; see
// third_party/colour-demosaicing-LICENSE.txt for the complete notice.
// Source: colour-science/colour-demosaicing, bayer/demosaicing/menon2007.py
// Paper: Menon, Andriani & Calvagno, IEEE TIP 16(1), 132-141 (2007).
#include "DemosaicMenon.hpp"
#include <cmath>
#include <cstddef>
#include <limits>

namespace photonstack::detail {
std::vector<float> demosaicMenon(const std::vector<float>& mosaic, int width, int height,
                                 const std::array<int, 4>& colors, const std::array<float, 3>& gains) {
    const auto count = mosaic.size();
    const auto color = [&](int x, int y) { return colors[(y % 2) * 2 + x % 2]; };
    const auto mirror = [](int x, int n) {
        while (x < 0 || x >= n)
            x = x < 0 ? -x : 2 * n - 2 - x;
        return x;
    };
    const auto index = [&](int x, int y) {
        return static_cast<std::size_t>(mirror(y, height)) * width + mirror(x, width);
    };
    const auto at = [&](const std::vector<float>& a, int x, int y) { return a[index(x, y)]; };
    std::vector<float> raw(count), gh(count), gv(count), ch(count), cv(count);
    std::array<std::vector<float>, 3> rgb;
    for (auto& c : rgb)
        c.resize(count);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(y) * width + x;
            raw[p] = mosaic[p] * gains[color(x, y)];
            rgb[color(x, y)][p] = raw[p];
        }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(y) * width + x;
            if (color(x, y) == 1) {
                gh[p] = gv[p] = raw[p];
                continue;
            }
            gh[p] =
                .5F * (at(raw, x - 1, y) + at(raw, x + 1, y) + raw[p]) - .25F * (at(raw, x - 2, y) + at(raw, x + 2, y));
            gv[p] =
                .5F * (at(raw, x, y - 1) + at(raw, x, y + 1) + raw[p]) - .25F * (at(raw, x, y - 2) + at(raw, x, y + 2));
            ch[p] = raw[p] - gh[p];
            cv[p] = raw[p] - gv[p];
        }
    std::vector<float> dh(count), dv(count);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(y) * width + x;
            dh[p] = std::abs(ch[p] - at(ch, x + 2, y));
            dv[p] = std::abs(cv[p] - at(cv, x, y + 2));
        }
    const int kernel[5][5] = {{0, 0, 1, 0, 1}, {0, 0, 0, 1, 0}, {0, 0, 3, 0, 3}, {0, 0, 0, 1, 0}, {0, 0, 1, 0, 1}};
    std::vector<std::uint8_t> horizontal(count);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(y) * width + x;
            double h = 0, v = 0;
            for (int ky = 0; ky < 5; ++ky)
                for (int kx = 0; kx < 5; ++kx) {
                    const int sx = x + 2 - kx, sy = y + 2 - ky;
                    if (sx < 0 || sy < 0 || sx >= width || sy >= height)
                        continue;
                    const auto q = static_cast<std::size_t>(sy) * width + sx;
                    // Do not multiply NaN by zero: an unused neighbor is not a sample.
                    if (kernel[ky][kx])
                        h += kernel[ky][kx] * dh[q];
                    if (kernel[kx][ky])
                        v += kernel[kx][ky] * dv[q];
                }
            horizontal[p] = v >= h;
            rgb[1][p] = horizontal[p] ? gh[p] : gv[p];
        }
    // Direction buffers are no longer needed; release them before refinement
    // so a full-resolution stack need not retain all intermediate planes.
    for (auto* plane : {&raw, &gh, &gv, &ch, &cv, &dh, &dv})
        std::vector<float>().swap(*plane);
    // Red/blue at green sites use the axis containing measured samples.
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            if (color(x, y) != 1)
                continue;
            const auto p = static_cast<std::size_t>(y) * width + x;
            for (int c : {0, 2}) {
                const int dx = color(x ^ 1, y) == c ? 1 : 0, dy = 1 - dx;
                rgb[c][p] = rgb[1][p] + .5F * (at(rgb[c], x - dx, y - dy) + at(rgb[c], x + dx, y + dy) -
                                               at(rgb[1], x - dx, y - dy) - at(rgb[1], x + dx, y + dy));
            }
        }
    // Opposite red/blue sites use the posterior direction decision.
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int measured = color(x, y);
            if (measured == 1)
                continue;
            const int c = 2 - measured;
            const auto p = static_cast<std::size_t>(y) * width + x;
            const int dx = horizontal[p] ? 1 : 0, dy = 1 - dx;
            rgb[c][p] =
                rgb[measured][p] + .5F * (at(rgb[c], x - dx, y - dy) + at(rgb[c], x + dx, y + dy) -
                                          at(rgb[measured], x - dx, y - dy) - at(rgb[measured], x + dx, y + dy));
        }
    // Refinement snapshots each stage so iteration order cannot alter a result.
    std::array<std::vector<float>, 2> differences;
    const auto updateDifferences = [&] {
        for (auto& d : differences)
            d.resize(count);
        for (std::size_t p = 0; p < count; ++p) {
            differences[0][p] = rgb[0][p] - rgb[1][p];
            differences[1][p] = rgb[2][p] - rgb[1][p];
        }
    };
    updateDifferences();
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int c = color(x, y);
            if (c == 1)
                continue;
            const auto p = static_cast<std::size_t>(y) * width + x;
            const int dx = horizontal[p] ? 1 : 0, dy = 1 - dx;
            const auto& d = differences[c / 2];
            rgb[1][p] = rgb[c][p] - (at(d, x - dx, y - dy) + d[p] + at(d, x + dx, y + dy)) / 3;
        }
    updateDifferences();
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            if (color(x, y) != 1)
                continue;
            const auto p = static_cast<std::size_t>(y) * width + x;
            for (int c : {0, 2}) {
                const int dx = color(x ^ 1, y) == c ? 1 : 0, dy = 1 - dx;
                const auto& d = differences[c / 2];
                rgb[c][p] = rgb[1][p] + .5F * (at(d, x - dx, y - dy) + at(d, x + dx, y + dy));
            }
        }
    auto& difference = differences[0];
    for (std::size_t p = 0; p < count; ++p)
        difference[p] = rgb[0][p] - rgb[2][p];
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int c = color(x, y);
            if (c == 1)
                continue;
            const auto p = static_cast<std::size_t>(y) * width + x;
            const int dx = horizontal[p] ? 1 : 0, dy = 1 - dx;
            const float d = (at(difference, x - dx, y - dy) + difference[p] + at(difference, x + dx, y + dy)) / 3;
            rgb[2 - c][p] = rgb[c][p] + (c == 2 ? d : -d);
        }
    std::vector<float> out(count * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto p = static_cast<std::size_t>(y) * width + x;
            for (int c = 0; c < 3; ++c)
                out[p * 3 + c] = c == color(x, y) ? mosaic[p] : rgb[c][p] / gains[c];
        }
    return out;
}
} // namespace photonstack::detail
