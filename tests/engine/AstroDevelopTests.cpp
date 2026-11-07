#include "photonstack/AstroDevelop.hpp"
#include "DisplayPhotometry.hpp"
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
void checkSpatialSampling() {
    ImageBuffer im;
    im.width = im.height = 1000;
    im.channels = 3;
    // A resolved star field on both sides of the former ten-pixel sampling
    // pitch. Five pixels of translation must not hide all calibration stars.
    for (const int shift : {0, 5}) {
        im.pixels.assign(im.sampleCount(), 100.F);
        for (int i = 0; i < 100; ++i) {
            const int x = 40 + (i % 10) * 90 + shift, y = 40 + (i / 10) * 90;
            const float amplitude = 200.F + (i % 13) * 125.F;
            for (int dy = -8; dy <= 8; ++dy) for (int dx = -8; dx <= 8; ++dx) {
                auto* rgb = &im.pixels[((y + dy) * im.width + x + dx) * 3];
                const double signal = amplitude * std::exp(-(dx * dx + dy * dy) / 4.5);
                rgb[0] += 1.5 * signal;
                rgb[1] += signal;
                rgb[2] += .8 * signal;
            }
        }
        const auto result = AstroDevelop().apply(im);
        require(result.ok && result.calibrationStars >= 50, "calibration survives an offset relative to the sampling grid");
        require(std::abs(result.gains[0] - 1 / 1.5) < .001 && std::abs(result.gains[2] - 1 / .8) < .001,
                "same known stellar colors recover the same gains at either offset");
    }

    // Most sky pixels are neutral, but sampling only every tenth blue column
    // would turn a narrow detector pattern into a full-image blue subtraction.
    im.pixels.assign(im.sampleCount(), 100.F);
    for (std::size_t p = 0; p < im.pixelCount(); p += 10) im.pixels[3 * p + 2] += 40;
    AstroDevelopOptions sky;
    sky.stellarBalance = false;
    const auto result = AstroDevelop().apply(im, sky);
    require(result.ok && result.sky[0] == 100 && result.sky[1] == 100 && result.sky[2] == 100,
            "sky samples cover the color pattern instead of selecting a single phase");
}

void checkCalibrationUnderBrightStars() {
    ImageBuffer im;
    im.width = 344;
    im.height = 320;
    im.channels = 3;
    im.pixels.assign(im.sampleCount(), 100.F);
    const auto blank = AstroDevelop().apply(im);
    require(!blank.ok && blank.errorCode == "CalibrationStarsInsufficient", "empty sky is not a color reference");
    // Bright cores raise global variance, leaving only the brightest sources
    // above a mean-plus-sigma threshold. Their peaks then fail the high-end
    // cut. Fainter, well-resolved stars against the same sky are usable.
    for (int row = 0; row < 9; ++row) for (int col = 0; col < 9; ++col) {
        constexpr float levels[]{70, 420, 4200};
        constexpr float colors[][3]{{1.8F, 1, .55F}, {1, 1, 1}, {.55F, 1, 1.8F}};
        const float amplitude = levels[(col + 2 * row) % 3];
        const auto& rgb = colors[(col + row) % 3];
        const int x = 20 + 37 * col, y = 20 + 34 * row;
        for (int dy = -8; dy <= 8; ++dy) for (int dx = -8; dx <= 8; ++dx) {
            const float signal = amplitude * std::exp(-(dx * dx + dy * dy) / (2.F * 1.55F * 1.55F));
            const auto offset = ((y + dy) * im.width + x + dx) * 3;
            for (int c = 0; c < 3; ++c) im.pixels[offset + c] += signal * rgb[c];
        }
    }
    const auto calibrated = AstroDevelop().apply(im);
    require(calibrated.ok && calibrated.calibrationStars >= 27, "recover unsaturated stars beneath bright sources");
    require(std::abs(calibrated.gains[0] - 1) < .01 && std::abs(calibrated.gains[2] - 1) < .01,
            "balanced red, neutral and blue star populations retain neutral gains");
}

void checkMidtoneStellarColor() {
    auto image = field();
    for (const auto curve : {AstroToneCurve::Asinh, AstroToneCurve::Rational}) {
        AstroDevelopOptions opt;
        opt.stellarBalance = false;
        opt.background = 0;
        opt.toneCurve = curve;
        opt.toneScale = 100;
        opt.whitePoint = 4000;
        for (const auto ratios : {std::array{.8, 1., 1.4}, std::array{1.4, 1., .8}}) {
            const std::array<double, 3> sky{12, -4, 7};
            for (int c = 0; c < 3; ++c) image.pixels[c] = sky[c] + 40 * ratios[c];
            const auto result = AstroDevelop().apply(image, opt);
            require(result.ok, "known midtone star develops");
            const auto& codes = result.image.pixels;
            for (int c : {0, 2})
                require(std::abs(displayLight(codes[c]) / displayLight(codes[1]) - ratios[c]) < .01,
                        "midtone display light preserves measured stellar ratios for warm and cool stars");
        }
    }
}

void checkLegacyCodeReplay() {
    ImageBuffer image;
    image.width = image.height = 32;
    image.channels = 3;
    image.pixels.assign(image.sampleCount(), 100.F);
    const std::array<float, 9> samples{94, 96, 99, 122, 138, 159, 900, 300, 140};
    std::copy(samples.begin(), samples.end(), image.pixels.begin());
    // RGB16 values from the previous production CLI: colored shadow,
    // midtone, gamut-compressed highlight and neutral sky, in that order.
    const std::array<int, 12> rational{9023,9822,11019, 21121,26714,34054, 65335,49569,45365, 11796,11796,11796};
    const std::array<int, 12> asinh{11102,11313,11630, 14612,16337,18602, 62217,28107,19011, 11796,11796,11796};
    AstroDevelopOptions options;
    options.stellarBalance = false;
    options.linearDisplayColor = false;
    options.background = .18F;
    options.saturation = .9F;
    options.toneScale = 100;
    options.whitePoint = 4000;
    for (const auto curve : {AstroToneCurve::Rational, AstroToneCurve::Asinh}) {
        options.toneCurve = curve;
        const auto result = AstroDevelop().apply(image, options);
        require(result.ok && result.image.colorEncoding == ColorEncoding::SRGB, "legacy display transfer succeeds");
        const auto& expected = curve == AstroToneCurve::Rational ? rational : asinh;
        for (std::size_t i = 0; i < expected.size(); ++i)
            require(std::lround(result.image.pixels[i] * 65535.) == expected[i], "replay historical RGB16 codes");
    }
}

int main() {
    checkLegacyCodeReplay();
    checkMidtoneStellarColor();
    checkSpatialSampling();
    checkCalibrationUnderBrightStars();
    AstroDevelop develop;
    // A sparse, noisy field must not turn into bright salt-and-pepper pixels
    // merely because its 99.5th-percentile signal is weak (Seestar regression).
    ImageBuffer noisy;
    noisy.width = noisy.height = 128;
    noisy.channels = 3;
    noisy.pixels.resize(noisy.sampleCount());
    for (std::size_t p = 0; p < noisy.pixelCount(); ++p) {
        const float noise = float((p * 7919) % 1001) / 100.F - 5.F;
        for (int c = 0; c < 3; ++c)
            noisy.pixels[p * 3 + c] = 100 + noise + (p % 997 == 0 ? 300 : 0);
    }
    AstroDevelopOptions automatic;
    automatic.stellarBalance = false;
    automatic.toneCurve = AstroToneCurve::Asinh;
    automatic.background = .025F;
    const auto safeAuto = develop.apply(noisy, automatic);
    require(safeAuto.ok, "noisy sparse field develops automatically");
    // Denoising the sky must not re-stretch an untouched subject when a
    // shared reference is explicitly selected. No local star operations here.
    {
    auto reduced = noisy;
    for (std::size_t p = 0; p < noisy.pixelCount(); ++p) {
        const auto x=p%128,y=p/128;
        if (x>=48 && x<80 && y>=48 && y<80) continue;
        for(unsigned c=0;c<3;++c) reduced.pixels[p*3+c]=100+(noisy.pixels[p*3+c]-100)*.3F;
    }
    const auto unchangedInput=reduced.pixels,unchangedReference=noisy.pixels;
    const auto independentTransfer=develop.apply(reduced,automatic);
    const auto sharedTransfer=develop.apply(reduced,automatic,&noisy);
    const auto selfReference=develop.apply(noisy,automatic,&noisy);
    require(independentTransfer.ok && sharedTransfer.ok && selfReference.ok,"reference development succeeds");
    require(selfReference.image.pixels==safeAuto.image.pixels,"self reference preserves legacy rendering exactly");
    require(sharedTransfer.sky==safeAuto.sky && sharedTransfer.gains==safeAuto.gains &&
            sharedTransfer.toneScale==safeAuto.toneScale && sharedTransfer.whitePoint==safeAuto.whitePoint &&
            sharedTransfer.shadowNoise==safeAuto.shadowNoise,"reference holds complete global transfer");
    double automaticDrift=0;
    for(unsigned y=48;y<80;++y)for(unsigned x=48;x<80;++x)for(unsigned c=0;c<3;++c) {
        const auto i=(y*128+x)*3+c;
        require(sharedTransfer.image.pixels[i]==safeAuto.image.pixels[i],"untouched target retains exact display values");
        automaticDrift=std::max(automaticDrift,double(std::abs(independentTransfer.image.pixels[i]-safeAuto.image.pixels[i])));
    }
    require(automaticDrift>.02,"fixture detects denoise and automatic stretch feedback");
    require(sharedTransfer.image.pixels!=safeAuto.image.pixels,"reference pixels are not substituted for denoised input");
    require(reduced.pixels==unchangedInput && noisy.pixels==unchangedReference,"both inputs immutable");
    auto invalidReference=noisy;invalidReference.width-=1;
    require(!develop.apply(reduced,automatic,&invalidReference).ok,"reject mismatched reference geometry");
    invalidReference=noisy;invalidReference.colorEncoding=ColorEncoding::SRGB;
    require(!develop.apply(reduced,automatic,&invalidReference).ok,"reject display reference");
    invalidReference=noisy;invalidReference.pixels[0]=std::numeric_limits<float>::quiet_NaN();
    require(!develop.apply(reduced,automatic,&invalidReference).ok,"reject nonfinite reference");
    auto normalizedInput=reduced, normalizedReference=noisy;
    for(auto& v:normalizedInput.pixels)v*=.01F;
    for(auto& v:normalizedReference.pixels)v*=.01F;
    const auto normalizedTransfer=develop.apply(normalizedInput,automatic,&normalizedReference);
    require(normalizedTransfer.ok,"normalized reference develops");
    for(std::size_t i=0;i<reduced.sampleCount();++i)
        require(std::abs(normalizedTransfer.image.pixels[i]-sharedTransfer.image.pixels[i])<.00002,
                "shared transfer respects common scientific units");
    }
    std::size_t crushed = 0, amplified = 0;
    for (std::size_t p = 0; p < noisy.pixelCount(); ++p) {
        if (safeAuto.image.pixels[p * 3] < .002F) ++crushed;
        if (p % 997 != 0 && safeAuto.image.pixels[p * 3] > .2F) ++amplified;
    }
    require(crushed < noisy.pixelCount() / 100, "automatic stretch must not crush the noisy sky to black");
    require(amplified < noisy.pixelCount() / 100, "automatic stretch must not amplify sky into bright speckles");
    auto noisyScaled = noisy;
    for (auto& v : noisyScaled.pixels) v *= .01F;
    const auto safeScaled = develop.apply(noisyScaled, automatic);
    require(safeScaled.ok, "automatic noisy field works in normalized units");
    for (std::size_t i = 0; i < noisy.sampleCount(); ++i)
        require(std::abs(safeAuto.image.pixels[i] - safeScaled.image.pixels[i]) < .00002,
                "noise-aware automatic stretch is independent of FITS units");
    auto im = field();
    AstroDevelopOptions opt;
    opt.toneScale = 100;
    auto result = develop.apply(im, opt);
    require(result.ok, "stellar field develops");
    require(result.displayPixels == im.pixelCount(), "display counts cover valid pixels");
    auto counted = im;
    counted.pixels[3] = 0;
    AstroDevelopOptions extremes;
    extremes.stellarBalance = false; extremes.background = .001F;
    extremes.toneCurve = AstroToneCurve::Asinh; extremes.toneScale = 1; extremes.whitePoint = 10;
    const auto inspected = develop.apply(counted, extremes);
    require(inspected.ok && inspected.displayPixels == im.pixelCount()-1, "masked pixels excluded from review");
    std::size_t blackCount=0,whiteCount=0;
    for(std::size_t p=1;p<im.pixelCount();++p) {
        const auto* rgb=inspected.image.pixels.data()+p*4;
        blackCount+=std::max({rgb[0],rgb[1],rgb[2]})<=1.F/255;
        whiteCount+=std::min({rgb[0],rgb[1],rgb[2]})>=.995F;
    }
    require(blackCount>0 && whiteCount>0, "fixture has both display extremes");
    require(inspected.nearBlackPixels==blackCount && inspected.nearWhitePixels==whiteCount,
            "display counters match the produced RGB, not linear source values");
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
    // Scientific background subtraction can put even stellar peaks below zero
    // in one channel or the entire luminance image. A constant pedestal must
    // not remove calibration stars or change their sky-subtracted color ratios.
    for (const auto pedestal : {std::array<float,3>{0,0,-1024},
                               std::array<float,3>{-1024,-2048,-4096}}) {
        auto negativeField = im;
        for (std::size_t p=0;p<im.pixelCount();++p)
            for (int c=0;c<3;++c) negativeField.pixels[p*4+c] += pedestal[c];
        const auto originalNegative = negativeField.pixels;
        const auto negativeResult = develop.apply(negativeField,opt);
        require(negativeResult.ok, "negative stellar peaks remain valid color-calibration samples");
        require(negativeResult.calibrationStars==result.calibrationStars,
                "constant negative pedestals retain the same calibration stars");
        for(int c=0;c<3;++c) require(std::abs(negativeResult.gains[c]-result.gains[c])<.00002,
                                   "signed calibration retains channel response");
        for(std::size_t i=0;i<im.sampleCount();++i)
            require(std::abs(negativeResult.image.pixels[i]-result.image.pixels[i])<.00005,
                    "negative stellar pedestal does not change the developed image");
        require(negativeField.pixels==originalNegative,"signed scientific input remains untouched");
    }
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
        const double red = displayLight(r), green = displayLight(g), blue = displayLight(b);
        require(std::abs((red - green) / (green - blue) - 2) < .0001,
                "gamut compression preserves the chroma direction");
        require(std::abs(red * .2126 + green * .7152 + blue * .0722 -
                         displayLight(neutralHighlights.image.pixels[offset])) < .000003,
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
    // A nebula occupying most of the frame is not sky. Median subtraction
    // makes its constant-color component disappear even at high brightness.
    // An explicit lower reference retains that signal without changing the
    // display sky pedestal or mutating the scientific input.
    ImageBuffer diffuse;
    diffuse.width = diffuse.height = 128;
    diffuse.channels = 4;
    diffuse.pixels.resize(diffuse.sampleCount());
    for (int y=0;y<128;++y) for (int x=0;x<128;++x) {
        const auto o=(y*128+x)*4;
        const float signal = x>=40 ? 1.F : 0.F;
        diffuse.pixels[o] = 100 + 60*signal;
        diffuse.pixels[o+1] = 200 + 10*signal;
        diffuse.pixels[o+2] = 300 + 4*signal;
        diffuse.pixels[o+3] = 1;
    }
    const auto originalDiffuse = diffuse.pixels;
    auto coloredField = field();
    for(std::size_t p=0;p<coloredField.pixelCount();++p)
        for(int c=0;c<3;++c) coloredField.pixels[p*4+c] += float((p*7919)%1001)/200;
    AstroDevelopOptions balanceReference;
    const auto originalBalance = develop.apply(coloredField, balanceReference);
    balanceReference.skyPercentile = .2F;
    const auto lowerBalance = develop.apply(coloredField, balanceReference);
    require(originalBalance.ok && lowerBalance.ok && originalBalance.gains == lowerBalance.gains &&
            originalBalance.calibrationStars == lowerBalance.calibrationStars,
            "display reference must not silently change statistical white balance");
    for (auto curve : {AstroToneCurve::Rational, AstroToneCurve::Asinh}) {
        AstroDevelopOptions lower;
        lower.stellarBalance = false;
        lower.toneCurve = curve;
        lower.toneScale = 20;
        lower.whitePoint = 300;
        const auto medianReference = develop.apply(diffuse, lower);
        lower.skyPercentile = .2F;
        const auto retained = develop.apply(diffuse, lower);
        require(medianReference.ok && retained.ok, "extended nebula develops with both references");
        const auto o=(64*128+64)*4;
        require(std::abs(medianReference.image.pixels[o]-lower.background)<1e-6,
                "fixture reproduces diffuse signal lost to whole-frame median");
        require(retained.image.pixels[o] > retained.image.pixels[o+1]+.08F,
                "lower sky reference retains extended red emission");
        require(std::abs(retained.image.pixels[0]-lower.background)<1e-6,
                "true sky remains at requested display background");
        require(retained.sky == std::array<float,3>{100,200,300}, "reference remains auditable in physical units");
        auto transformed = diffuse;
        for(std::size_t p=0;p<diffuse.pixelCount();++p)
            for(int c=0;c<3;++c) transformed.pixels[p*4+c]=diffuse.pixels[p*4+c]*.01F + (c-1)*17;
        auto normalized = lower;
        normalized.toneScale *= .01F;
        normalized.whitePoint *= .01F;
        const auto transformedResult = develop.apply(transformed, normalized);
        require(transformedResult.ok, "lower reference accepts physical rescaling and channel pedestals");
        for(std::size_t i=0;i<diffuse.sampleCount();++i)
            require(std::abs(retained.image.pixels[i]-transformedResult.image.pixels[i])<.00005F,
                    "reference control is invariant to physical scale and channel offsets");
        require(diffuse.pixels == originalDiffuse, "display reference leaves original linear data untouched");
        auto maskedDiffuse = diffuse;
        maskedDiffuse.pixels[0] = -1e9F;
        maskedDiffuse.pixels[3] = 0;
        const auto maskedReference = develop.apply(maskedDiffuse, lower);
        require(maskedReference.ok && maskedReference.sky == retained.sky && maskedReference.image.pixels[3]==0,
                "invalid coverage cannot contaminate sky-reference quantile");
        for(float invalidReference : {0.F,.049F,.501F,std::numeric_limits<float>::quiet_NaN()}) {
            lower.skyPercentile = invalidReference;
            require(!develop.apply(diffuse, lower).ok, "invalid sky reference rejected");
        }
    }
    std::cout << "Astronomical development regressions passed\n";
}
