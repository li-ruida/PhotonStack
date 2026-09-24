#include "photonstack/StarColorCalibrator.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>

using namespace photonstack;
namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture { ImageBuffer input, reference; std::vector<StarColorPair> pairs; };
Fixture field() {
    Fixture f;
    for (auto* im : {&f.input, &f.reference}) {
        im->width = im->height = 300; im->channels = 4; im->pixels.resize(im->sampleCount());
    }
    std::mt19937 random(31);
    std::normal_distribution<double> noise(0, .03);
    for (int y = 0; y < 300; ++y) for (int x = 0; x < 300; ++x) {
        const auto p = (y * 300 + x) * 4;
        for (int c = 0; c < 3; ++c) {
            f.input.pixels[p + c] = -4 + 8 * c + .07 * x - .09 * y + noise(random);
            f.reference.pixels[p + c] = (2 - c + .03 * x + .04 * y + noise(random)) / 10000;
        }
        f.input.pixels[p + 3] = f.reference.pixels[p + 3] = 1;
    }
    const std::array<double, 3> gains{.65, 1, .76};
    for (int y = 30; y < 280; y += 42) for (int x = 30; x < 280; x += 42) {
        const int id = f.pairs.size();
        const double sx = x + .17 + .1 * (id % 3), sy = y + .23;
        const double rx = sx + 2.35, ry = sy - 1.18;
        f.pairs.push_back({static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(rx), static_cast<float>(ry)});
        // Intrinsically different stellar colors: no assumption of a white median star.
        const std::array<double, 3> color{.6 + .1 * (id % 10), 1, .8 + .12 * (id % 7)};
        for (int dy = -13; dy <= 13; ++dy) for (int dx = -13; dx <= 13; ++dx) {
            const int px = x + dx, py = y + dy;
            const auto o = (py * 300 + px) * 4;
            const double a = (120 + id * 3) * std::exp(-((px - sx) * (px - sx) + (py - sy) * (py - sy)) / 4.5);
            const double b = (120 + id * 3) * std::exp(-((px - rx) * (px - rx) + (py - ry) * (py - ry)) / 4.5);
            for (int c = 0; c < 3; ++c) {
                f.input.pixels[o + c] += a * color[c];
                f.reference.pixels[o + c] += b * color[c] * gains[c] / 10000;
            }
        }
    }
    return f;
}
}
int main() {
    const auto f = field();
    const StarColorCalibrator calibrator;
    auto result = calibrator.estimate(f.input, f.reference, f.pairs);
    require(result.ok && result.trainingStars == 18 && result.validationStars == 18, "disjoint calibration succeeds");
    require(std::abs(result.gains[0] - .65) < .002 && std::abs(result.gains[2] - .76) < .002,
            "recovers reference ratios with independent units, color populations, sky gradients and pixel phases");
    require(result.validationP90AbsoluteLogError[0] < .004 && result.validationP90AbsoluteLogError[2] < .004,
            "held-out colors recover independently");
    auto duplicates = f.pairs;
    duplicates.insert(duplicates.end(), f.pairs.begin(), f.pairs.end());
    const auto repeated = calibrator.estimate(f.input, f.reference, duplicates);
    require(repeated.ok && repeated.trainingStars == 18 && repeated.rejectedStars == 36,
            "duplicates do not leak between training and validation");
    auto scaled = f.input;
    for (std::size_t p = 0; p < scaled.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c) scaled.pixels[p * 4 + c] *= .0001F;
    const auto unit = calibrator.estimate(scaled, f.reference, f.pairs);
    require(unit.ok && std::abs(unit.gains[0] - result.gains[0]) < 1e-5, "input unit invariance");
    auto bad = f.reference;
    // Corrupt only the held-out stars. The fitted gains alone cannot expose this.
    for (std::size_t i = 1; i < f.pairs.size(); i += 2) {
        const int x = std::lround(f.pairs[i][2]), y = std::lround(f.pairs[i][3]);
        for (int dy = -8; dy <= 8; ++dy) for (int dx = -8; dx <= 8; ++dx)
            bad.pixels[((y + dy) * 300 + x + dx) * 4 + 2] *= 1.7;
    }
    require(calibrator.estimate(f.input, bad, f.pairs).errorCode == "ColorCalibrationInconsistent",
            "independent validation rejects inconsistent reference colors");
    bad = f.reference;
    const int x = std::lround(f.pairs[0][2]), y = std::lround(f.pairs[0][3]);
    const auto p = (y * 300 + x) * 4;
    bad.pixels[p + 3] = 0;
    const auto masked = calibrator.estimate(f.input, bad, f.pairs);
    require(masked.ok && masked.rejectedStars == 1, "incomplete apertures are excluded");
    bad = f.reference; bad.pixels[p] = 1;
    StarColorCalibrationOptions opt; opt.referenceSaturation = .5;
    const auto saturated = calibrator.estimate(f.input, bad, f.pairs, opt);
    require(saturated.ok && saturated.rejectedStars == 1, "saturated channels are excluded");
    bad = f.reference; bad.pixels[p] = std::numeric_limits<float>::quiet_NaN();
    require(calibrator.estimate(f.input, bad, f.pairs).rejectedStars == 1, "nonfinite apertures are excluded");
    std::vector<StarColorPair> sparse(f.pairs.begin(), f.pairs.begin() + 23);
    require(!calibrator.estimate(f.input, f.reference, sparse).ok, "insufficient validation fails");
    bad = f.reference; bad.colorEncoding = ColorEncoding::SRGB;
    require(!calibrator.estimate(f.input, bad, f.pairs).ok, "stretched reference rejected");
    opt.referenceSaturation = std::numeric_limits<float>::quiet_NaN();
    require(!calibrator.estimate(f.input, f.reference, f.pairs, opt).ok, "invalid options rejected");
    auto invalidPairs = f.pairs; invalidPairs[0][0] = std::numeric_limits<float>::infinity();
    require(!calibrator.estimate(f.input, f.reference, invalidPairs).ok, "nonfinite coordinates rejected");
    const auto before = f.input.pixels;
    calibrator.estimate(f.input, f.reference, f.pairs);
    require(before == f.input.pixels, "native pixels remain unchanged");
    std::cout << "Reference-relative star color tests passed\n";
}
