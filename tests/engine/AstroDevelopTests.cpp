#include "photonstack/AstroDevelop.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace photonstack;
void require(bool b, const char* message) {
    if (!b) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
ImageBuffer field() {
    ImageBuffer im;
    im.width = 256;
    im.height = 256;
    im.channels = 4;
    im.pixels.resize(im.sampleCount());
    for (std::size_t p = 0; p < im.pixelCount(); ++p) {
        im.pixels[p * 4] = 12;
        im.pixels[p * 4 + 1] = -4;
        im.pixels[p * 4 + 2] = 7;
        im.pixels[p * 4 + 3] = 1;
    }
    for (int y = 24; y < 240; y += 32)
        for (int x = 24; x < 240; x += 32) {
            const double amplitude = 200 + (x + y) % 140;
            for (int dy = -9; dy <= 9; ++dy)
                for (int dx = -9; dx <= 9; ++dx) {
                    const auto o = ((y + dy) * im.width + x + dx) * 4;
                    const double signal = amplitude * std::exp(-(dx * dx + dy * dy) / 4.5);
                    im.pixels[o] += 1.5 * signal;
                    im.pixels[o + 1] += signal;
                    im.pixels[o + 2] += .8 * signal;
                }
        }
    return im;
}
int main() {
    AstroDevelop develop;
    auto im = field();
    AstroDevelopOptions opt;
    opt.toneScale = 100;
    auto result = develop.apply(im, opt);
    require(result.ok, "stellar field develops");
    require(result.calibrationStars >= 8, "enough independent calibration stars");
    require(std::abs(result.gains[0] - 2.F / 3) < .003 && std::abs(result.gains[2] - 1.25F) < .003,
            "recovers known channel response");
    require(result.image.colorEncoding == ColorEncoding::SRGB, "display result is tagged");
    for (float p : result.image.pixels)
        require(std::isfinite(p) && p >= 0 && p <= 1, "finite bounded gamut");
    auto shifted = im;
    for (std::size_t p = 0; p < im.pixelCount(); ++p) {
        shifted.pixels[p * 4] += 100;
        shifted.pixels[p * 4 + 1] += 50;
        shifted.pixels[p * 4 + 2] -= 80;
    }
    auto offset = develop.apply(shifted, opt);
    require(offset.ok, "offset field develops");
    for (std::size_t i = 0; i < im.sampleCount(); ++i)
        require(std::abs(offset.image.pixels[i] - result.image.pixels[i]) < .00002, "per-channel pedestal invariance");
    auto scaled = im;
    for (std::size_t p = 0; p < im.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c)
            scaled.pixels[p * 4 + c] *= .01F;
    auto optScaled = opt;
    optScaled.toneScale *= .01F;
    auto sr = develop.apply(scaled, optScaled);
    require(sr.ok, "normalized units work");
    for (std::size_t i = 0; i < im.sampleCount(); ++i)
        require(std::abs(sr.image.pixels[i] - result.image.pixels[i]) < .00002, "physical unit invariance");
    opt.starExposure = .3F;
    auto reduced = develop.apply(im, opt);
    require(reduced.ok && reduced.adjustedStars >= 8, "compact star exposure is adjusted");
    require(reduced.image.pixels[(24 * im.width + 24) * 4] < result.image.pixels[(24 * im.width + 24) * 4],
            "stellar signal reduced");
    require(std::abs(reduced.image.pixels[0] - result.image.pixels[0]) < .00001, "sky remains unchanged");
    // An extended nucleus has high luminance but a broad radial profile.
    auto nucleus = im;
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            auto o = (y * 256 + x) * 4;
            float signal = 1000 * std::exp(-((x - 128) * (x - 128) + (y - 128) * (y - 128)) / 200.F);
            for (int c = 0; c < 3; ++c)
                nucleus.pixels[o + c] += signal;
        }
    opt.stellarBalance = false;
    auto protectedNucleus = develop.apply(nucleus, opt);
    opt.starExposure = 1;
    auto baselineNucleus = develop.apply(nucleus, opt);
    require(protectedNucleus.ok && baselineNucleus.ok, "nucleus renders");
    for (int c = 0; c < 3; ++c)
        require(protectedNucleus.image.pixels[(128 * 256 + 128) * 4 + c] ==
                    baselineNucleus.image.pixels[(128 * 256 + 128) * 4 + c],
                "broad nucleus is protected");
    auto invalid = im;
    invalid.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    require(!develop.apply(invalid, opt).ok, "covered NaN rejected");
    invalid.pixels[3] = 0;
    auto masked = develop.apply(invalid, opt);
    require(masked.ok && masked.image.pixels[0] == 0 && masked.image.pixels[3] == 0, "masked NaN remains transparent");
    invalid = im;
    invalid.colorEncoding = ColorEncoding::SRGB;
    require(!develop.apply(invalid, opt).ok, "already stretched input rejected");
    opt.background = -1;
    require(!develop.apply(im, opt).ok, "invalid background rejected");
    opt.background = .03F;
    opt.starExposure = 0;
    require(!develop.apply(im, opt).ok, "zero star exposure rejected");
    // Shadow rolloff preserves a gradation below the median sky rather than clipping.
    opt.starExposure = 1;
    auto shadows = im;
    for (int c = 0; c < 3; ++c) {
        shadows.pixels[c] -= 10;
        shadows.pixels[4 + c] -= 5;
    }
    auto sh = develop.apply(shadows, opt);
    require(sh.ok && sh.image.pixels[0] > 0 && sh.image.pixels[0] < sh.image.pixels[4], "smooth shadow gradation");
    // Asinh is monotonic, keeps highlight gradation, and is invariant under
    // a change of scientific units when its physical scales change together.
    AstroDevelopOptions asinh;
    asinh.toneCurve = AstroToneCurve::Asinh;
    asinh.toneScale = 20;
    asinh.whitePoint = 300;
    auto ar = develop.apply(im, asinh);
    require(ar.ok, "asinh develops");
    auto ascaled = asinh;
    ascaled.toneScale *= .01F;
    ascaled.whitePoint *= .01F;
    auto scaledAsinh = develop.apply(scaled, ascaled);
    require(scaledAsinh.ok, "asinh scaled units develop");
    for (std::size_t i = 0; i < im.sampleCount(); ++i) {
        require(std::isfinite(ar.image.pixels[i]) && ar.image.pixels[i] >= 0 && ar.image.pixels[i] <= 1,
                "asinh finite gamut");
        require(std::abs(ar.image.pixels[i] - scaledAsinh.image.pixels[i]) < .00002, "asinh physical unit invariance");
    }
    auto ramp = im;
    for (int i=0;i<10;++i) for (int c=0;c<3;++c) ramp.pixels[i*4+c] = 1000.0F + i*500;
    asinh.stellarBalance = false;
    auto highlights = develop.apply(ramp, asinh);
    require(highlights.ok, "highlight ramp renders");
    for (int i=1;i<10;++i)
        require(highlights.image.pixels[i*4] > highlights.image.pixels[(i-1)*4] && highlights.image.pixels[i*4] < 1,
                "highlights remain graded without clipping");
    asinh.whitePoint = std::numeric_limits<float>::quiet_NaN();
    require(!develop.apply(im, asinh).ok, "invalid asinh white point rejected");
    // Colored highlights used to land exactly on a gamut boundary even while
    // neutral highlights retained their gradation. Test observable hue,
    // luminance and 16-bit headroom rather than the compression formula.
    auto coloredRamp = field();
    auto neutralRamp = field();
    AstroDevelopOptions coloredOptions;
    coloredOptions.stellarBalance = false;
    coloredOptions.toneCurve = AstroToneCurve::Asinh;
    coloredOptions.toneScale = 20;
    coloredOptions.whitePoint = 3000;
    coloredOptions.saturation = 2;
    for (int i = 0; i < 24; ++i) {
        const float signal = 100 + i * 150;
        const float components[] = {2.5F * signal, signal, .25F * signal};
        const float luminance = components[0] * .2126F + components[1] * .7152F + components[2] * .0722F;
        const float sky[] = {12, -4, 7};
        for (int c = 0; c < 3; ++c) {
            coloredRamp.pixels[i * 4 + c] = sky[c] + components[c];
            neutralRamp.pixels[i * 4 + c] = sky[c] + luminance;
        }
    }
    const auto coloredHighlights = develop.apply(coloredRamp, coloredOptions);
    const auto neutralHighlights = develop.apply(neutralRamp, coloredOptions);
    require(coloredHighlights.ok && neutralHighlights.ok, "colored and neutral highlight ramps render");
    for (int i = 0; i < 24; ++i) {
        const auto offset = i * 4;
        const auto& values = coloredHighlights.image.pixels;
        const float r = values[offset], g = values[offset + 1], b = values[offset + 2];
        require(r > g && g > b && b > .5F / 65535 && r < 1 - .5F / 65535,
                "colored highlights retain hue and do not quantize onto black or white");
        require(std::abs((r - g) / (g - b) - 2) < .0001,
                "gamut compression preserves the chroma direction");
        require(std::abs(r * .2126 + g * .7152 + b * .0722 - neutralHighlights.image.pixels[offset]) < .000003,
                "gamut compression preserves mapped luminance");
        if (i > 0)
            require(r > values[offset - 4], "colored highlights retain increasing channel gradation");
    }
    // Bright-star styling must leave faint sources and the sky unchanged. The
    // threshold is in display units, so changing FITS units cannot change it.
    auto mixed = field();
    for (int y = 112; y <= 144; ++y)
        for (int x = 112; x <= 144; ++x) {
            const double signal = 15000 * std::exp(-((x-128.)*(x-128.)+(y-128.)*(y-128.))/4.5);
            for (int c = 0; c < 3; ++c) mixed.pixels[(y*256+x)*4+c] += signal;
        }
    // A wider bright star used to fail the compact-source test or retain its
    // outer halo because a fixed 8-pixel aperture ended inside the source.
    for (int y = 40; y <= 88; ++y)
        for (int x = 104; x <= 152; ++x) {
            const double signal = 18000 * std::exp(-((x-128.)*(x-128.)+(y-64.)*(y-64.))/18.);
            for (int c = 0; c < 3; ++c) mixed.pixels[(y*256+x)*4+c] += signal;
        }
    AstroDevelopOptions bright;
    bright.stellarBalance = false;
    bright.toneCurve = AstroToneCurve::Asinh;
    bright.toneScale = 20;
    bright.whitePoint = 3000;
    const auto mixedBaseline = develop.apply(mixed, bright);
    bright.starExposure = .35F;
    bright.starPeakThreshold = .7F;
    const auto selected = develop.apply(mixed, bright);
    require(mixedBaseline.ok && selected.ok && selected.adjustedStars > 0, "bright stars are selected");
    require(selected.image.pixels[(128*256+128)*4] < mixedBaseline.image.pixels[(128*256+128)*4] - .02,
            "bright stellar peak is reduced");
    std::cout << "Wide star display peak " << mixedBaseline.image.pixels[(64*256+128)*4] << " -> " << selected.image.pixels[(64*256+128)*4] << ", wing " << mixedBaseline.image.pixels[(64*256+137)*4] << " -> " << selected.image.pixels[(64*256+137)*4] << "\n";
    require(selected.image.pixels[(64*256+128)*4] < mixedBaseline.image.pixels[(64*256+128)*4] - .01,
            "wide bright star is selected");
    require(selected.image.pixels[(64*256+137)*4] < mixedBaseline.image.pixels[(64*256+137)*4] - .01,
            "wide stellar wing is also attenuated");
    auto brightNucleus = bright;
    const auto selectedNucleus = develop.apply(nucleus, brightNucleus);
    brightNucleus.starExposure = 1;
    const auto untouchedNucleus = develop.apply(nucleus, brightNucleus);
    require(selectedNucleus.ok && untouchedNucleus.ok, "selective nucleus renders");
    std::cout << "Nucleus peak " << untouchedNucleus.image.pixels[(128*256+128)*4] << " -> " << selectedNucleus.image.pixels[(128*256+128)*4] << "\n";
    for (int c = 0; c < 3; ++c)
        require(selectedNucleus.image.pixels[(128*256+128)*4+c] == untouchedNucleus.image.pixels[(128*256+128)*4+c],
                "selective mode protects extended nucleus");
    for (int y = 12; y <= 36; ++y)
        for (int x = 12; x <= 36; ++x)
            for (int c = 0; c < 4; ++c)
                require(selected.image.pixels[(y*256+x)*4+c] == mixedBaseline.image.pixels[(y*256+x)*4+c],
                        "faint stellar source and surrounding sky remain unchanged");
    auto normalizedMixed = mixed;
    for (std::size_t p = 0; p < mixed.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c) normalizedMixed.pixels[p*4+c] *= .01F;
    auto normalizedBright = bright;
    normalizedBright.toneScale *= .01F;
    normalizedBright.whitePoint *= .01F;
    const auto normalizedSelected = develop.apply(normalizedMixed, normalizedBright);
    require(normalizedSelected.ok && normalizedSelected.adjustedStars == selected.adjustedStars,
            "bright selection is independent of FITS units");
    for (std::size_t j = 0; j < mixed.sampleCount(); ++j)
        require(std::abs(normalizedSelected.image.pixels[j] - selected.image.pixels[j]) < .00002,
                "selective bright reduction is independent of FITS units");
    bright.starPeakThreshold = 1;
    auto none = develop.apply(mixed, bright);
    require(none.ok && none.adjustedStars == 0 && none.image.pixels == mixedBaseline.image.pixels,
            "threshold one retains all sources");
    for (float value : {-1.F,1.01F,std::numeric_limits<float>::quiet_NaN()}) {
        bright.starPeakThreshold = value;
        require(!develop.apply(mixed, bright).ok, "invalid bright-star threshold rejected");
    }
    // The wider taper must not make a secondary bright ring or dark trough in
    // the radial profile of an isolated star, including a broader blue PSF.
    auto isolated = mixed;
    bright.starPeakThreshold = .75F;
    for (double width : {2.,3.,4.}) {
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                const double rr = (x-128.)*(x-128.)+(y-128.)*(y-128.);
                for (int c = 0; c < 3; ++c) {
                    const double sigma = width * (c==2 ? 1.15 : 1);
                    isolated.pixels[(y*256+x)*4+c] = 100 + 15000 * std::exp(-rr/(2*sigma*sigma));
                }
                isolated.pixels[(y*256+x)*4+3] = 1;
            }
        const auto tapered = develop.apply(isolated, bright);
        require(tapered.ok && tapered.adjustedStars==1, "isolated broad source is detected once");
        double previousLuma = 1;
        for (int radius = 0; radius <= 24; ++radius) {
            const auto o = (128*256+128+radius)*4;
            const double lum = .2126*tapered.image.pixels[o]+.7152*tapered.image.pixels[o+1]+.0722*tapered.image.pixels[o+2];
            require(lum <= previousLuma+1e-6 && lum >= bright.background-1e-6,
                    "stellar taper must not introduce radial ring or dark trough");
            previousLuma = lum;
        }
    }
    std::cout << "Astronomical development regressions passed\n";
}
