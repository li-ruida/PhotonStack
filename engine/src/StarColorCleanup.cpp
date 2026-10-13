#include "photonstack/StarColorCleanup.hpp"
#include "MaskedImageSampling.hpp"
#include "photonstack/StarCentroid.hpp"
#include "photonstack/StarDetector.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace photonstack {
namespace {
double median(std::vector<double> values) {
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}
bool covered(const ImageBuffer& im, std::size_t p) {
    return detail::pixelCoverage(im, p) >= .99F && detail::pixelHasValidColor(im, p);
}
struct Sample {
    std::size_t pixel;
    double luminance = 0, weight = 0;
    std::array<double, 3> signal{};
};
} // namespace

StarColorCleanupResult StarColorCleanup::apply(const ImageBuffer& im,
                                              const std::array<float, 3>& gains,
                                              float strength) const {
    StarColorCleanupResult result;
    if (im.empty() || (im.channels != 3 && im.channels != 4) ||
        im.pixels.size() != im.sampleCount() || im.colorEncoding != ColorEncoding::Linear ||
        detail::imageHasInvalidCoveredColor(im)) {
        result.errorCode = "ImageBufferInvalid";
        result.message = "Stellar color cleanup requires finite covered linear RGB";
        return result;
    }
    if (!std::isfinite(strength) || strength < 0 || strength > 1 ||
        std::any_of(gains.begin(), gains.end(), [](float g) { return !std::isfinite(g) || g <= 0 || g > 20; })) {
        result.errorCode = "ArgumentInvalid";
        result.message = "Invalid stellar color cleanup strength or gains";
        return result;
    }
    result.image = im;
    result.ok = true;
    if (strength == 0 || im.width < 48 || im.height < 48) return result;

    const std::array<double, 3> weights = {.2126 * gains[0], .7152 * gains[1], .0722 * gains[2]};
    ImageBuffer mono;
    mono.width = im.width; mono.height = im.height;
    mono.channels = 1; mono.format = PixelFormat::Float32Gray;
    mono.pixels.resize(im.pixelCount());
    for (std::size_t p = 0; p < im.pixelCount(); ++p) {
        double luminance = 0;
        if (covered(im, p))
            for (int c = 0; c < 3; ++c) luminance += weights[c] * im.pixels[p * im.channels + c];
        if (!std::isfinite(static_cast<float>(luminance))) {
            result.ok = false; result.errorCode = "ImageValueInvalid";
            result.message = "Stellar luminance exceeds Float32 range";
            return result;
        }
        mono.pixels[p] = static_cast<float>(luminance);
    }
    StarDetectionOptions options;
    options.minPeak = 0; options.sigmaThreshold = 4; options.maxStars = 25000;
    options.border = 22; options.robustStatistics = true; options.countAllDetections = true;
    const auto detected = StarDetector().detectLuminance(im.width, im.height, mono.pixels, options);
    if (!detected.ok) return result;
    // Fitting near missing coverage must fail, rather than treating masked zero
    // as a dark sky measurement. Detection above still receives finite samples.
    for (std::size_t p = 0; p < im.pixelCount(); ++p)
        if (!covered(im, p)) mono.pixels[p] = std::numeric_limits<float>::quiet_NaN();

    const auto& stars = detected.stars;
    constexpr int cell = 32;
    const int columns = (im.width + cell - 1) / cell, rows = (im.height + cell - 1) / cell;
    std::vector<std::vector<std::size_t>> grid(static_cast<std::size_t>(columns) * rows);
    for (std::size_t i = 0; i < stars.size(); ++i)
        grid[int(stars[i].y) / cell * columns + int(stars[i].x) / cell].push_back(i);
    std::vector<unsigned char> occupied(im.pixelCount(), 0);
    for (std::size_t i = 0; i < stars.size(); ++i) {
        const auto fit = fitStarCentroid(mono, stars[i].x, stars[i].y);
        if (!fit.ok) continue;
        // Detector moments use a 3x3 patch and underestimate stellar wings.
        // A local fit supplies the size, but never replaces measured pixels.
        const double fwhm = 2.35482 * std::sqrt((fit.covarianceXX + fit.covarianceYY) * .5);
        if (fwhm < 1 || fwhm > 6) continue;
        double nearest = 100;
        const int gx = int(fit.x) / cell, gy = int(fit.y) / cell;
        for (int yy = std::max(0, gy - 1); yy <= std::min(rows - 1, gy + 1); ++yy)
            for (int xx = std::max(0, gx - 1); xx <= std::min(columns - 1, gx + 1); ++xx)
                for (auto j : grid[yy * columns + xx])
                    if (j != i && stars[j].flux > .15 * stars[i].flux)
                        nearest = std::min(nearest, std::hypot(double(fit.x - stars[j].x), double(fit.y - stars[j].y)));
        // Preserve blended stars with potentially different intrinsic colors.
        if (nearest < 2.3 * fwhm) continue;
        const double radius = std::min(std::clamp(4 * fwhm, 6., 14.), .8 * nearest);
        const int x = std::lround(fit.x), y = std::lround(fit.y), outer = std::ceil(radius) + 7;
        if (x < outer || y < outer || x + outer >= int(im.width) || y + outer >= int(im.height)) continue;
        std::array<std::vector<double>, 3> skySamples;
        std::vector<double> skyLuminance;
        bool valid = true;
        for (int dy = -outer; dy <= outer; ++dy)
            for (int dx = -outer; dx <= outer; ++dx) {
                const double distance = std::hypot(x + dx - fit.x, y + dy - fit.y);
                if (distance > outer) continue;
                const auto p = std::size_t(y + dy) * im.width + x + dx;
                if (!covered(im, p)) { valid = false; continue; }
                // Measure beyond the halo; a sky annulus inside it leaves a
                // blue pedestal that survives even full-strength correction.
                if (distance > radius + 4) {
                    for (int c = 0; c < 3; ++c) skySamples[c].push_back(im.pixels[p * im.channels + c]);
                    skyLuminance.push_back(mono.pixels[p]);
                }
            }
        if (!valid || skyLuminance.size() < 24) continue;
        std::array<double, 3> sky;
        for (int c = 0; c < 3; ++c) sky[c] = median(skySamples[c]);
        const double center = median(skyLuminance);
        for (auto& v : skyLuminance) v = std::abs(v - center);
        const double noise = 1.4826 * median(skyLuminance);
        std::vector<Sample> samples;
        std::array<double, 3> flux{};
        double luminanceFlux = 0, compensationWeight = 0;
        bool overlap = false;
        const int extent = std::ceil(radius);
        for (int dy = -extent; dy <= extent; ++dy)
            for (int dx = -extent; dx <= extent; ++dx) {
                const double distance = std::hypot(x + dx - fit.x, y + dy - fit.y);
                if (distance >= radius) continue;
                const auto p = std::size_t(y + dy) * im.width + x + dx;
                if (occupied[p]) overlap = true;
                Sample sample{p};
                for (int c = 0; c < 3; ++c) {
                    sample.signal[c] = im.pixels[p * im.channels + c] - sky[c];
                    sample.luminance += weights[c] * sample.signal[c];
                    flux[c] += sample.signal[c];
                }
                luminanceFlux += sample.luminance;
                const double taper = std::clamp((radius - distance) / 1.5, 0., 1.);
                const double positive = std::max(sample.luminance, 0.);
                const double denominator = positive * positive + 4 * noise * noise;
                sample.weight = denominator > 0 ? taper * taper * (3 - 2 * taper) * positive * positive / denominator : 0;
                compensationWeight += sample.weight * positive;
                samples.push_back(sample);
            }
        if (overlap || luminanceFlux <= 20 * noise * std::sqrt(samples.size()) || compensationWeight <= 0 ||
            std::any_of(flux.begin(), flux.end(), [](double v) { return !std::isfinite(v) || v <= 0; })) continue;

        // Match each pixel to this star's own integrated color. Subtract the
        // total change with a luminosity-shaped weight so neither blue nor red
        // stellar flux is lost. Green enforces unchanged calibrated luminance.
        std::array<double, 3> totalChange{};
        for (const auto& sample : samples)
            for (int c : {0, 2})
                totalChange[c] += sample.weight * (sample.luminance * flux[c] / luminanceFlux - sample.signal[c]);
        for (const auto& sample : samples) {
            std::array<double, 3> delta{};
            for (int c : {0, 2})
                delta[c] = strength * (sample.weight * (sample.luminance * flux[c] / luminanceFlux - sample.signal[c]) -
                           totalChange[c] * sample.weight * std::max(sample.luminance, 0.) / compensationWeight);
            delta[1] = -(weights[0] * delta[0] + weights[2] * delta[2]) / weights[1];
            for (int c = 0; c < 3; ++c) {
                const auto offset = sample.pixel * im.channels + c;
                result.image.pixels[offset] = static_cast<float>(im.pixels[offset] + delta[c]);
            }
            occupied[sample.pixel] = 1;
        }
        ++result.correctedStars;
    }
    return result;
}
} // namespace photonstack
