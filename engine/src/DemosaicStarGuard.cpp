#include "DemosaicStarGuard.hpp"
#include "photonstack/StarCentroid.hpp"
#include "photonstack/StarDetector.hpp"
#include "photonstack/ParallelRanges.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>
#include <vector>

namespace photonstack::detail {
namespace {
double median(std::vector<double> values) {
    const auto mid = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), mid, values.end());
    return *mid;
}
struct PixelChange { std::size_t offset; double lift, capacity; };
}

void guardDemosaicStars(ImageBuffer& image, std::span<const float> mosaic,
                       const std::array<int, 4>& colors,
                       const std::array<float, 3>& gains) {
    const int w = image.width, h = image.height;
    if (w < 48 || h < 48) return;
    const auto color = [&](int x, int y) { return colors[(y % 2) * 2 + x % 2]; };
    ImageBuffer mono;
    mono.width = w; mono.height = h; mono.channels = 1;
    mono.format = PixelFormat::Float32Gray; mono.pixels.resize(image.pixelCount());
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        const double value = .2126 * gains[0] * image.pixels[p * 4] +
            .7152 * gains[1] * image.pixels[p * 4 + 1] + .0722 * gains[2] * image.pixels[p * 4 + 2];
        mono.pixels[p] = std::isfinite(value) && std::isfinite(mosaic[p])
            ? static_cast<float>(value) : 0;
    }
    StarDetectionOptions options;
    options.minPeak = std::numeric_limits<float>::lowest(); options.sigmaThreshold = 8; options.border = 22;
    options.maxStars = 5000; options.robustStatistics = true; options.countAllDetections = true;
    const auto detected = StarDetector().detectLuminance(w, h, mono.pixels, options);
    if (!detected.ok) return;
    for (std::size_t p = 0; p < image.pixelCount(); ++p)
        if (!std::isfinite(mosaic[p])) mono.pixels[p] = std::numeric_limits<float>::quiet_NaN();
    const auto& stars = detected.stars;
    constexpr int cell = 32;
    const int columns = (w + cell - 1) / cell, rows = (h + cell - 1) / cell;
    std::vector<std::vector<std::size_t>> grid(static_cast<std::size_t>(columns) * rows);
    for (std::size_t i = 0; i < stars.size(); ++i)
        grid[int(stars[i].y) / cell * columns + int(stars[i].x) / cell].push_back(i);
    // Fits only read mono; aperture edits below still run in detection order.
    // Joining before editing preserves identical floating-point results and
    // the overlap policy while avoiding thousands of serial nonlinear fits.
    std::vector<StarCentroidFit> fits(stars.size());
    std::atomic<bool> allocationFailed{false};
    parallelRanges(stars.size(), 64, [&](std::size_t first, std::size_t last) noexcept {
        try {
            for (std::size_t i = first; i < last; ++i)
                fits[i] = fitStarCentroid(mono, stars[i].x, stars[i].y);
        } catch (const std::bad_alloc&) { allocationFailed.store(true, std::memory_order_relaxed); }
    });
    if (allocationFailed.load(std::memory_order_relaxed)) throw std::bad_alloc();
    std::vector<unsigned char> occupied(image.pixelCount());
    for (std::size_t i = 0; i < stars.size(); ++i) {
        const auto& fit = fits[i];
        if (!fit.ok) continue;
        const double fwhm = 2.35482 * std::sqrt((fit.covarianceXX + fit.covarianceYY) * .5);
        if (fwhm < 1 || fwhm > 6) continue;
        double nearest = 100;
        const int gx = int(fit.x) / cell, gy = int(fit.y) / cell;
        for (int yy = std::max(0, gy - 1); yy <= std::min(rows - 1, gy + 1); ++yy)
            for (int xx = std::max(0, gx - 1); xx <= std::min(columns - 1, gx + 1); ++xx)
                for (auto j : grid[yy * columns + xx])
                    if (j != i && stars[j].flux > .1 * stars[i].flux)
                        nearest = std::min(nearest, std::hypot(double(fit.x - stars[j].x), double(fit.y - stars[j].y)));
        if (nearest < 2.3 * fwhm) continue;
        const double radius = std::min(std::clamp(4 * fwhm, 6., 14.), .8 * nearest);
        const int x = std::lround(fit.x), y = std::lround(fit.y), outer = std::ceil(radius) + 7;
        if (x < outer || y < outer || x + outer >= w || y + outer >= h) continue;
        std::array<std::vector<double>, 3> skySamples;
        bool valid = true;
        for (int dy = -outer; dy <= outer; ++dy)
            for (int dx = -outer; dx <= outer; ++dx) {
                const double distance = std::hypot(x + dx - fit.x, y + dy - fit.y);
                if (distance > outer) continue;
                const auto p = std::size_t(y + dy) * w + x + dx;
                if (!std::isfinite(mosaic[p]) || occupied[p]) { valid = false; continue; }
                if (distance > radius + 4) skySamples[color(x + dx, y + dy)].push_back(mosaic[p]);
            }
        if (!valid || std::any_of(skySamples.begin(), skySamples.end(), [](const auto& s) { return s.size() < 12; })) continue;
        std::array<double, 3> sky, noise;
        for (int c = 0; c < 3; ++c) {
            sky[c] = median(skySamples[c]);
            for (auto& value : skySamples[c]) value = std::abs(value - sky[c]);
            noise[c] = 1.4826 * median(skySamples[c]);
        }
        const double skyL = .2126 * gains[0] * sky[0] + .7152 * gains[1] * sky[1] + .0722 * gains[2] * sky[2];
        const double noiseL = std::sqrt(std::pow(.2126 * gains[0] * noise[0], 2) +
            std::pow(.7152 * gains[1] * noise[1], 2) + std::pow(.0722 * gains[2] * noise[2], 2));
        if (stars[i].peak - skyL < 50 * noiseL) continue;
        const int extent = std::ceil(radius);
        std::array<std::vector<PixelChange>, 3> changes;
        std::array<double, 3> lifted{}, capacity{};
        for (int dy = -extent; dy <= extent; ++dy)
            for (int dx = -extent; dx <= extent; ++dx) {
                if (std::hypot(x + dx - fit.x, y + dy - fit.y) >= radius) continue;
                const auto p = std::size_t(y + dy) * w + x + dx;
                for (int c = 0; c < 3; ++c) {
                    if (color(x + dx, y + dy) == c) continue;
                    // A negative scientific sky is legitimate. The bound is
                    // relative to measured local sky and its noise, never zero.
                    double lower = sky[c];
                    for (int yy = -1; yy <= 1; ++yy)
                        for (int xx = -1; xx <= 1; ++xx)
                            if (color(x + dx + xx, y + dy + yy) == c)
                                lower = std::min(lower, double(mosaic[std::size_t(y + dy + yy) * w + x + dx + xx]));
                    lower -= 3 * noise[c];
                    const double original = image.pixels[p * 4 + c];
                    const double lift = std::max(0., lower - original);
                    const double positive = std::max(0., original - sky[c] - 3 * noise[c]);
                    changes[c].push_back({p * 4 + c, lift, positive});
                    lifted[c] += lift; capacity[c] += positive;
                }
            }
        bool changed = false;
        for (int c = 0; c < 3; ++c) {
            if (lifted[c] <= 0 || capacity[c] <= 0 || lifted[c] > .75 * capacity[c]) continue;
            // Redistribute only among interpolated samples of this aperture.
            // Clamping alone would systematically brighten weak color channels.
            const double fraction = lifted[c] / capacity[c];
            for (const auto& change : changes[c])
                image.pixels[change.offset] = static_cast<float>(image.pixels[change.offset] +
                    change.lift - fraction * change.capacity);
            changed = true;
        }
        if (changed)
            for (int dy = -extent; dy <= extent; ++dy)
                for (int dx = -extent; dx <= extent; ++dx)
                    if (std::hypot(x + dx - fit.x, y + dy - fit.y) < radius)
                        occupied[std::size_t(y + dy) * w + x + dx] = 1;
    }
}
}
