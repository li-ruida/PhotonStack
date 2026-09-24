#include "photonstack/AstroDevelop.hpp"
#include "photonstack/ChannelAlignment.hpp"
#include "photonstack/StarCentroid.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
using namespace photonstack;
void require(bool b, const char* s) {
    if (!b)
        throw std::runtime_error(s);
}
ImageBuffer field(double dx, double dy, bool variable = false, bool sparse = false) {
    ImageBuffer a;
    a.width = 256;
    a.height = 256;
    a.channels = 4;
    a.pixels.resize(a.sampleCount());
    for (unsigned y = 0; y < a.height; ++y)
        for (unsigned x = 0; x < a.width; ++x) {
            const auto p = (std::size_t(y) * a.width + x) * 4;
            a.pixels[p + 3] = 1;
            for (unsigned c = 0; c < 3; ++c) {
                double v = -3.0 + 2.0 * c + .001 * x - .002 * y;
                for (int sy = 28; sy < 240; sy += 40)
                    for (int sx = 28; sx < 240; sx += 40) {
                        if (sparse && (sy > 68 || sx > 68))
                            continue;
                        const double shiftX = c == 0 ? dx + (variable && sx > 128 ? .4 : 0) : c == 2 ? -dx * .3 : 0;
                        const double shiftY = c == 0 ? dy : c == 2 ? -dy * .3 : 0;
                        const double xx = double(x) - sx - .23 - shiftX, yy = double(y) - sy - .31 - shiftY;
                        v += (c == 0   ? 1.3
                              : c == 2 ? .8
                                       : 1) *
                             (100 + (sx + sy) % 80) * std::exp(-(xx * xx + yy * yy) / 4.5);
                    }
                a.pixels[p + c] = v;
            }
        }
    return a;
}
int main() {
    auto input = field(-.38, .16);
    auto r = ChannelAlignment().apply(input);
    require(r.ok && r.applied[0] && r.applied[2], "Reliable coherent shifts corrected");
    require(std::hypot(r.dx[0] + .38, r.dy[0] - .16) < .005, "Known red displacement recovered");
    const auto replay=ChannelAlignment().applyMeasured(input,r);
    require(replay.ok && replay.image.pixels==r.image.pixels, "Replay uses exactly the measured sampling kernel");
    auto noise=input, added=input;
    std::mt19937 replayRng(448);
    std::normal_distribution<float> replayNoise(0,2);
    for(std::size_t p=0;p<input.pixelCount();++p) for(unsigned c=0;c<3;++c) {
        noise.pixels[p*4+c]=replayNoise(replayRng);
        added.pixels[p*4+c]+=noise.pixels[p*4+c];
    }
    const auto propagated=ChannelAlignment().applyMeasured(noise,r);
    const auto combined=ChannelAlignment().applyMeasured(added,r);
    require(propagated.ok && combined.ok && propagated.applied==r.applied, "Noise replay does not redetect stars");
    for(std::size_t p=0;p<input.pixelCount();++p) {
        require(propagated.image.pixels[p*4+1]==noise.pixels[p*4+1] && propagated.image.pixels[p*4+3]==1,
                "Replay leaves green and alpha unchanged");
        for(unsigned c=0;c<3;++c)
            require(std::abs(combined.image.pixels[p*4+c]-r.image.pixels[p*4+c]-propagated.image.pixels[p*4+c])<.0001,
                    "Signal and noise share the same linear transform within Float32 rounding");
    }
    auto badTransform=r;badTransform.sourceWidth--;
    require(!ChannelAlignment().applyMeasured(input,badTransform).ok,"Mismatched geometry rejected");
    badTransform=r;badTransform.dx[0]=std::numeric_limits<double>::quiet_NaN();
    require(!ChannelAlignment().applyMeasured(input,badTransform).ok,"Nonfinite transform rejected");
    badTransform=r;badTransform.applied[1]=true;
    require(!ChannelAlignment().applyMeasured(input,badTransform).ok,"Green displacement rejected");
    auto badNoise=noise;badNoise.pixels[3]=0;
    require(!ChannelAlignment().applyMeasured(badNoise,r).ok,"Invalid diagnostic coverage rejected");
    ImageBuffer red, green;
    red.width = green.width = 256;
    red.height = green.height = 256;
    red.channels = green.channels = 1;
    red.pixels.resize(input.pixelCount());
    green.pixels.resize(input.pixelCount());
    for (std::size_t p = 0; p < input.pixelCount(); ++p) {
        require(r.image.pixels[p * 4 + 1] == input.pixels[p * 4 + 1] && r.image.pixels[p * 4 + 3] == 1,
                "Green and alpha untouched");
        red.pixels[p] = r.image.pixels[p * 4];
        green.pixels[p] = r.image.pixels[p * 4 + 1];
    }
    auto rf = fitStarCentroid(red, 108.23F, 108.31F), gf = fitStarCentroid(green, 108.23F, 108.31F);
    require(rf.ok && gf.ok && std::hypot(rf.x - gf.x, rf.y - gf.y) < .025,
            "Correct shift direction and subpixel sampling");
    for (unsigned c : {0U, 2U}) {
        double before = 0, after = 0;
        for (int y = 100; y <= 116; ++y)
            for (int x = 100; x <= 116; ++x) {
                const double bg = -3. + 2 * c + .001 * x - .002 * y;
                before += input.pixels[(y * 256 + x) * 4 + c] - bg;
                after += r.image.pixels[(y * 256 + x) * 4 + c] - bg;
            }
        require(std::abs(after / before - 1) < .001, "Stellar aperture flux retained");
    }
    auto zero = field(0, 0);
    auto same = ChannelAlignment().apply(zero);
    require(same.ok && !same.applied[0] && !same.applied[2] && same.image.pixels == zero.pixels,
            "Already aligned exact bypass");
    auto sparse = ChannelAlignment().apply(field(-.38, .16, false, true));
    require(sparse.ok && !sparse.applied[0] && !sparse.applied[2], "Sparse field skipped");
    auto variable = ChannelAlignment().apply(field(-.38, .16, true));
    require(variable.ok && !variable.applied[0], "Spatially varying shifts reject global translation");
    auto large = ChannelAlignment().apply(field(-1.8, .16));
    require(large.ok && !large.applied[0], "Large displacement skipped");
    auto hole = input;
    hole.pixels[3] = 0;
    auto masked = ChannelAlignment().apply(hole);
    require(masked.ok && masked.image.pixels == hole.pixels, "Incomplete coverage exact bypass");
    hole = input;
    hole.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    require(!ChannelAlignment().apply(hole).ok, "Nonfinite rejected");

    // Display shadow treatment must not change luminance or wash out strong color.
    ImageBuffer noisy = field(0, 0);
    std::mt19937 rng(76);
    std::normal_distribution<float> n(0, 1);
    for (std::size_t p = 0; p < noisy.pixelCount(); ++p)
        for (unsigned c = 0; c < 3; ++c)
            noisy.pixels[p * 4 + c] += n(rng);
    AstroDevelopOptions opt;
    opt.stellarBalance = false;
    opt.toneScale = 12;
    opt.whitePoint = 300;
    opt.toneCurve = AstroToneCurve::Asinh;
    auto before = AstroDevelop().apply(noisy, opt);
    opt.shadowNeutralization = 1;
    auto after = AstroDevelop().apply(noisy, opt);
    require(before.ok && after.ok && after.shadowNoise > .5, "Measured shadow noise");
    double oldColor = 0, newColor = 0;
    for (unsigned y = 0; y < 256; ++y)
        for (unsigned x = 0; x < 256; ++x) {
            const auto p = (y * 256 + x) * 4;
            const auto lum = [&](const ImageBuffer& im) {
                return .2126 * im.pixels[p] + .7152 * im.pixels[p + 1] + .0722 * im.pixels[p + 2];
            };
            require(std::abs(lum(before.image) - lum(after.image)) < 2e-7, "Display luminance unchanged");
            if (y < 12) {
                oldColor += std::abs(before.image.pixels[p] - before.image.pixels[p + 1]);
                newColor += std::abs(after.image.pixels[p] - after.image.pixels[p + 1]);
            }
        }
    require(newColor < oldColor * .4, "Low-SNR background chroma reduced");
    const auto p = (28 * 256 + 28) * 4;
    require(std::abs((after.image.pixels[p] - after.image.pixels[p + 1]) /
                         (before.image.pixels[p] - before.image.pixels[p + 1]) -
                     1) < .01,
            "Strong stellar color retained");
    auto scaled = noisy;
    for (std::size_t p = 0; p < scaled.pixelCount(); ++p)
        for (unsigned c = 0; c < 3; ++c)
            scaled.pixels[p * 4 + c] *= .01F;
    auto tinyOpt = opt;
    tinyOpt.toneScale *= .01F;
    tinyOpt.whitePoint *= .01F;
    auto tiny = AstroDevelop().apply(scaled, tinyOpt);
    require(tiny.ok, "Scaled physical units accepted");
    for (std::size_t p = 0; p < tiny.image.sampleCount(); ++p)
        require(std::abs(tiny.image.pixels[p] - after.image.pixels[p]) < 2e-5, "Unit invariant shadow treatment");
    opt.shadowNeutralization = std::numeric_limits<float>::quiet_NaN();
    require(!AstroDevelop().apply(noisy, opt).ok, "Invalid shadow strength rejected");
    std::cout << "Channel alignment and noise-aware display tests passed\n";
}
