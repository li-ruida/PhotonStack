#include "photonstack/SpectralCalibration.hpp"
#include "photonstack/StellarPhotometry.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace photonstack;
void require(bool ok, const char* reason) {
    if (!ok)
        throw std::runtime_error(reason);
}
template <class F> void rejects(F f, const char* reason) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, reason);
}
int main() {
    try {
        SampledSpectrum flat{{{400, 500, 700}, {2, 2, 2}}, {.1, .1, .1}};
        SpectralBand response{{{400, 450, 550, 700}, {1, 1, 1, 1}}};
        const auto flux = integratePhotonSpectrum(flat, response);
        require(std::abs(flux.value - 330000) < 1e-8 && std::abs(flux.errorBound - 16500) < 1e-8 && flux.hasErrors,
                "photon weighting or uncertainty integral");
        require(std::abs(flux.meanWavelength - 550) < 1e-10 &&
                    std::abs(flux.rmsBandwidth - 300 / std::sqrt(12.)) < 1e-9,
                "response moments");
        SampledSpectrum linear{{{1, 1.3, 1.8, 2}, {2, 2.6, 3.6, 4}}, {}};
        SpectralBand eight(8, SpectralCurve{{1, 1.2, 1.9, 2}, {.5, .6, .95, 1}});
        const double exact = 2. / 256 * (std::pow(2, 11) - 1) / 11;
        require(std::abs(integratePhotonSpectrum(linear, eight).value - exact) < 1e-12,
                "union-knot high-degree product integral");
        auto scaled = flat;
        for (auto& f : scaled.flux.value)
            f *= 1e-20;
        for (auto& f : scaled.error)
            f *= 1e-20;
        require(std::abs(integratePhotonSpectrum(scaled, response).value / flux.value - 1e-20) < 1e-34,
                "physical small-flux scale");
        auto negative = flat;
        negative.flux.value = {-2, -2, -2};
        require(integratePhotonSpectrum(negative, response).value == -flux.value, "negative spectral bins clipped");
        auto bad = response;
        bad[0].value[0] = -1;
        rejects([&] { integratePhotonSpectrum(flat, bad); }, "negative transmission accepted");
        bad = response;
        bad[0].wavelength[0] = 300;
        rejects([&] { integratePhotonSpectrum(flat, bad); }, "spectrum extrapolated");
        bad = response;
        bad[0].wavelength[1] = 400;
        rejects([&] { integratePhotonSpectrum(flat, bad); }, "duplicate wavelengths accepted");
        std::vector<SpectralColorSample> stars;
        for (unsigned j = 0; j < 10; ++j)
            for (unsigned i = 0; i < 10; ++i) {
                SpectralColorSample s;
                s.id = std::to_string(j * 10 + i);
                s.x = 20 + 30 * i;
                s.y = 20 + 30 * j;
                s.predicted = {2. + i * .1, 3. + j * .1, 1. + (i + j) * .1};
                s.observed = {s.predicted[0] / 1.7, s.predicted[1], s.predicted[2] / .65};
                stars.push_back(s);
            }
        const std::array<double, 3> white{2, 1, .5};
        auto fit = fitSpectralColors(stars, white, 320, 320);
        require(fit.valid && fit.training == 80 && fit.validation == 20 && fit.validationRegions >= 4,
                "held-out split or coverage");
        require(std::abs(fit.gains[0] - .85) < 1e-12 && std::abs(fit.gains[2] - 1.3) < 1e-12,
                "white-relative calibration");
        auto contaminated = stars;
        for (unsigned i : {0, 10, 20, 30, 40, 50, 60})
            contaminated[i].observed[0] *= 5;
        fit = fitSpectralColors(contaminated, white, 320, 320);
        require(fit.valid && fit.rejected == 7 && std::abs(fit.gains[0] - .85) < 1e-12, "fit contaminants bias gain");
        for (unsigned i = 4; i < 100; i += 5)
            contaminated[i].observed[2] *= 2;
        fit = fitSpectralColors(contaminated, white, 320, 320);
        require(!fit.valid && fit.validation == 20 && fit.validationP90[2] > .6,
                "held-out failures trimmed into passing result");
        auto duplicate = stars;
        duplicate[1].id = duplicate[0].id;
        rejects([&] { fitSpectralColors(duplicate, white, 320, 320); }, "duplicate source ID accepted");
        // Native aperture: three different stellar amplitudes on independent tilted
        // skies, negative background, subpixel center and contaminated annulus.
        ImageBuffer image;
        image.width = image.height = 80;
        image.channels = 4;
        image.pixels.resize(image.sampleCount());
        const double cx = 40.21, cy = 39.73, sigma = 1.5;
        const std::array<double, 3> amplitude{100, 200, 300};
        for (unsigned y = 0; y < 80; ++y)
            for (unsigned x = 0; x < 80; ++x) {
                const double dx = x - cx, dy = y - cy;
                const auto p = (y * 80 + x) * 4;
                for (unsigned c = 0; c < 3; ++c)
                    image.pixels[p + c] = float(-20.0 + 2.0 * c + .1 * dx - .2 * dy +
                                                amplitude[c] * std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma)));
                image.pixels[p + 3] = 1;
            }
        auto ap = measureStellarAperture(image, cx + .3, cy - .3);
        if (!ap.valid)
            std::cerr << "Aperture status: " << ap.status << " center " << ap.x << "," << ap.y << "\n";
        require(ap.valid, "synthetic stellar aperture failed");
        require(std::hypot(ap.x - cx, ap.y - cy) < 1e-4, "centroid refinement inaccurate");
        for (unsigned c = 0; c < 3; ++c) {
            require(std::abs(ap.flux[c] / (2 * std::acos(-1) * sigma * sigma * amplitude[c]) - 1) < 3e-6,
                    "aperture lost known Gaussian flux");
            require(std::abs(ap.skyPlane[c][1] - .1) < 1e-6 && std::abs(ap.skyPlane[c][2] + .2) < 1e-6,
                    "tilted sky not recovered");
        }
        auto polluted = image;
        auto clipped = image;
        for (std::size_t p = 0; p < clipped.pixelCount(); ++p)
            clipped.pixels[p * 4] = std::min(clipped.pixels[p * 4], 20.F);
        const auto clippedMeasurement = measureStellarAperture(clipped, cx, cy);
        require(!clippedMeasurement.valid && clippedMeasurement.status == "clipped-channel",
                "single-channel clipped core passed luminance validation");
        for (unsigned y = 38; y < 42; ++y)
            for (unsigned x = 52; x < 54; ++x)
                for (unsigned c = 0; c < 3; ++c)
                    polluted.pixels[(y * 80 + x) * 4 + c] += 500;
        auto robust = measureStellarAperture(polluted, cx, cy);
        require(robust.valid && std::abs(robust.flux[0] / ap.flux[0] - 1) < 1e-5,
                "annulus contamination changes stellar color");
        image.pixels[(40 * 80 + 40) * 4 + 3] = 0;
        require(!measureStellarAperture(image, cx, cy).valid, "coverage hole accepted");
        StellarApertureOptions opt;
        opt.saturation = 50;
        require(!measureStellarAperture(polluted, cx, cy, opt).valid, "saturated source accepted");
        std::cout
            << "Photon integration, white-normalized fit, held-out rejection and robust native photometry passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
