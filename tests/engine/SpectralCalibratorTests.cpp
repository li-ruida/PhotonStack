#include "photonstack/SpectralCalibrator.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
using namespace photonstack;
namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f, const char* message) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}
// Independent inverse gnomonic formula; inputs are a simple diagonal TAN grid.
std::array<double, 2> world(double x, double y, const TanGrid& grid) {
    const double rad = std::numbers::pi / 180;
    const double a = (x + 1 - grid.x) * grid.cd[0] * rad;
    const double b = (y + 1 - grid.y) * grid.cd[3] * rad;
    const double d = grid.dec * rad;
    const double denominator = std::cos(d) - b * std::sin(d);
    return {grid.ra + std::atan2(a, denominator) / rad,
            std::atan2(std::sin(d) + b * std::cos(d), std::hypot(denominator, a)) / rad};
}
} // namespace
int main() {
    try {
        ImageBuffer image;
        image.width = image.height = 360;
        image.channels = 4;
        image.pixels.resize(image.sampleCount());
        for (unsigned y = 0; y < image.height; ++y)
            for (unsigned x = 0; x < image.width; ++x) {
                const auto p = (y * image.width + x) * 4;
                for (unsigned c = 0; c < 3; ++c)
                    image.pixels[p + c] = float(-10. + .001 * x - .002 * y + c);
                image.pixels[p + 3] = 1;
            }
        const SolvedPanel sky{{180, 180, 30, 20, {-.001, 0, 0, .001}}, {}};
        const std::array<SpectralBand, 3> bands{
            {{{{600, 700}, {1, 1}}}, {{{500, 600}, {1, 1}}}, {{{400, 500}, {1, 1}}}}};
        const SampledSpectrum white{{{350, 750}, {2, 2}}, {}};
        const std::array<double, 3> instrument{1.4, 1, .7};
        std::vector<SpectralReferenceStar> stars;
        for (unsigned j = 0; j < 8; ++j)
            for (unsigned i = 0; i < 8; ++i) {
                const double x = 40.2 + 40 * i, y = 40.3 + 40 * j;
                const auto coord = world(x, y, sky.grid);
                SpectralReferenceStar s;
                s.id = std::to_string(j * 8 + i);
                s.ra = coord[0];
                s.dec = coord[1];
                s.spectrum = {{{350, 750}, {1. + .05 * i, 1. + .05 * j}}, {.001, .001}};
                std::array<double, 3> amplitude{};
                for (unsigned c = 0; c < 3; ++c)
                    amplitude[c] = integratePhotonSpectrum(s.spectrum, bands[c]).value / (300 * instrument[c]);
                for (int dy = -12; dy <= 12; ++dy)
                    for (int dx = -12; dx <= 12; ++dx) {
                        const auto px = unsigned(x) + dx, py = unsigned(y) + dy;
                        const double g = std::exp(-((px - x) * (px - x) + (py - y) * (py - y)) / (2 * 1.5 * 1.5));
                        for (unsigned c = 0; c < 3; ++c)
                            image.pixels[(py * image.width + px) * 4 + c] += float(amplitude[c] * g);
                    }
                stars.push_back(s);
            }
        const auto original = image.pixels;
        auto reviewed = reviewSpectralCalibration(image, sky, stars, bands, white);
        require(reviewed.fit.valid && reviewed.detected == 64, "known star field not calibrated");
        require(reviewed.fit.training == 52 && reviewed.fit.validation == 12, "unexpected synthetic split");
        for (unsigned c = 0; c < 3; ++c)
            require(std::abs(reviewed.fit.instrumentGains[c] / instrument[c] - 1) < 2e-7,
                    "incorrect known instrument gain");
        require(image.pixels == original, "review modified original pixels");
        const std::vector<SpectralReferenceStar> tooFew(stars.begin(), stars.begin() + 10);
        const auto sparse = reviewSpectralCalibration(image, sky, tooFew, bands, white);
        require(!sparse.fit.valid, "insufficient independent references passed");
        rejects([&] { applySpectralCalibration(image, sparse.fit); }, "sparse review applied");
        auto noisy = image;
        std::mt19937 random(34567);
        std::normal_distribution<double> noise(0, .2);
        for (std::size_t p = 0; p < noisy.pixelCount(); ++p)
            for (unsigned c = 0; c < 3; ++c)
                noisy.pixels[p * 4 + c] += float(noise(random));
        const auto noisyReview = reviewSpectralCalibration(noisy, sky, stars, bands, white);
        require(noisyReview.fit.valid, "noisy known field failed validation");
        for (unsigned c = 0; c < 3; ++c)
            require(std::abs(noisyReview.fit.instrumentGains[c] / instrument[c] - 1) < .002,
                    "noisy field biases instrument gain");
        auto weak = image;
        for (unsigned y = 20; y < 62; ++y)
            for (unsigned x = 20; x < 62; ++x) {
                const auto p = (y * image.width + x) * 4;
                // Remove only R signal at star 0. Luminance still locates it;
                // per-channel signal-to-noise must reject its colors.
                weak.pixels[p] = float(-10. + .001 * x - .002 * y + 10 * noise(random));
            }
        const auto weakReview = reviewSpectralCalibration(weak, sky, stars, bands, white);
        require(weakReview.stars[0].status == "signal-to-noise", "low channel SNR accepted");
        const auto calibrated = applySpectralCalibration(image, reviewed.fit);
        for (std::size_t p = 0; p < image.pixelCount(); ++p) {
            require(calibrated.pixels[p * 4 + 3] == image.pixels[p * 4 + 3], "coverage changed");
            for (unsigned c = 0; c < 3; ++c)
                require(calibrated.pixels[p * 4 + c] == float(double(image.pixels[p * 4 + c]) * reviewed.fit.gains[c]),
                        "gain application is not linear");
        }
        require(calibrated.pixels[0] < 0, "negative background clipped");
        auto partiallyMasked = image;
        partiallyMasked.pixels[3] = 0;
        partiallyMasked.pixels[7] = .3;
        const auto maskedOutput = applySpectralCalibration(partiallyMasked, reviewed.fit);
        require(maskedOutput.pixels[0] == partiallyMasked.pixels[0] && maskedOutput.pixels[3] == 0 &&
                    maskedOutput.pixels[7] == .3F &&
                    maskedOutput.pixels[4] == float(double(partiallyMasked.pixels[4]) * reviewed.fit.gains[0]),
                "masked or partial coverage mishandled");
        auto invalid = reviewed.fit;
        invalid.valid = false;
        rejects([&] { applySpectralCalibration(image, invalid); }, "failed fit applied");
        invalid = reviewed.fit;
        invalid.gains[0] = std::numeric_limits<double>::infinity();
        rejects([&] { applySpectralCalibration(image, invalid); }, "nonfinite gain applied");
        auto excessive = image;
        excessive.pixels[0] = std::numeric_limits<float>::max();
        invalid = reviewed.fit;
        invalid.gains[0] = 2;
        rejects([&] { applySpectralCalibration(excessive, invalid); }, "Float32 overflow accepted");
        auto polluted = stars;
        polluted[0].spectrum.error = {10, 10};
        polluted[1].spectrum.error.clear();
        // A second catalog ID at the same star must reject both, not improve N.
        polluted.push_back(polluted[2]);
        polluted.back().id = "duplicate-position";
        SpectralCalibrationOptions options;
        options.excluded.insert("4");
        reviewed = reviewSpectralCalibration(image, sky, polluted, bands, white, options);
        require(reviewed.fit.valid && reviewed.stars[0].status == "spectral-error" &&
                    reviewed.stars[1].status == "spectral-error-missing" &&
                    reviewed.stars[2].status == "catalog-neighbor" &&
                    reviewed.stars.back().status == "catalog-neighbor" && reviewed.stars[4].status == "excluded",
                "reference rejection audit failed");
        auto offFrame = polluted[0];
        offFrame.id = "outside";
        offFrame.ra = 210;
        offFrame.dec = -20;
        polluted.push_back(offFrame);
        reviewed = reviewSpectralCalibration(image, sky, polluted, bands, white, options);
        require(reviewed.stars.back().status == "outside" && !reviewed.stars.back().hasPosition,
                "opposite hemisphere not rejected");
        auto crowded = image;
        // Uncatalogued secondary peak inside the expanded aperture margin.
        for (int y = 27; y <= 54; ++y)
            for (int x = 267; x <= 304; ++x)
                for (unsigned c = 0; c < 3; ++c)
                    crowded.pixels[(y * image.width + x) * 4 + c] +=
                        float(80 * std::exp(-((x - 289.2) * (x - 289.2) + (y - 40.3) * (y - 40.3)) / 4.5));
        reviewed = reviewSpectralCalibration(crowded, sky, stars, bands, white);
        require(reviewed.stars[6].status == "detected-neighbor", "uncatalogued neighbor accepted");
        auto halo = image;
        for (int y = 18; y <= 62; ++y)
            for (int x = 298; x <= 342; ++x)
                halo.pixels[(y * image.width + x) * 4] +=
                    float(12 * std::exp(-((x - 320.2) * (x - 320.2) + (y - 40.3) * (y - 40.3)) / 50));
        reviewed = reviewSpectralCalibration(halo, sky, stars, bands, white);
        require(reviewed.stars[7].status == "aperture-color-unstable", "color-dependent stellar wings accepted");
        options.excluded.insert("unknown");
        rejects([&] { reviewSpectralCalibration(image, sky, stars, bands, white, options); },
                "unknown exclusion accepted");
        auto brokenSky = sky;
        brokenSky.grid.cd = {0, 0, 0, 0};
        rejects([&] { reviewSpectralCalibration(image, brokenSky, stars, bands, white); }, "singular WCS accepted");
        auto nonlinear = image;
        nonlinear.colorEncoding = ColorEncoding::SRGB;
        rejects([&] { reviewSpectralCalibration(nonlinear, sky, stars, bands, white); }, "display image calibrated");
        std::cout << "Spectral review, isolation, uncertainty, exclusion, signed gains and coverage passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
