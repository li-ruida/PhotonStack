#include "photonstack/AstroDevelop.hpp"
#include "DisplayGamut.hpp"
#include "MaskedImageSampling.hpp"
#include "photonstack/StarDetector.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace photonstack {
namespace {
float quantile(std::vector<float> values, float q) {
    if (values.empty())
        return 0;
    auto it = values.begin() + static_cast<std::size_t>((values.size() - 1) * q);
    std::nth_element(values.begin(), it, values.end());
    return *it;
}
bool covered(const ImageBuffer& im, std::size_t p) {
    if (im.channels == 4 && (!std::isfinite(im.pixels[p * 4 + 3]) || im.pixels[p * 4 + 3] < .99F))
        return false;
    for (int c = 0; c < 3; ++c)
        if (!std::isfinite(im.pixels[p * im.channels + c]))
            return false;
    return true;
}
AstroDevelopResult error(const char* code, const char* message) {
    AstroDevelopResult r;
    r.errorCode = code;
    r.message = message;
    return r;
}
} // namespace

AstroDevelopResult AstroDevelop::apply(const ImageBuffer& im, const AstroDevelopOptions& opt) const {
    if (im.empty() || (im.channels != 3 && im.channels != 4) || im.pixels.size() != im.sampleCount())
        return error("ImageBufferInvalid", "Develop requires a non-empty RGB image");
    if (im.colorEncoding != ColorEncoding::Linear)
        return error("ImageColorEncodingMismatch", "Develop requires linear data, before display stretching");
    if (!std::isfinite(opt.background) || opt.background < 0 || opt.background >= .5F ||
        !std::isfinite(opt.brightness) || opt.brightness <= 0 || opt.brightness > 20 || !std::isfinite(opt.toneScale) ||
        opt.toneScale < 0 || !std::isfinite(opt.saturation) || opt.saturation < 0 || opt.saturation > 3 ||
        !std::isfinite(opt.starExposure) || opt.starExposure < .05F || opt.starExposure > 1 ||
        !std::isfinite(opt.starPeakThreshold) || opt.starPeakThreshold < 0 || opt.starPeakThreshold > 1 ||
        !std::isfinite(opt.shadowNeutralization) || opt.shadowNeutralization < 0 || opt.shadowNeutralization > 1 ||
        !std::isfinite(opt.whitePoint) || opt.whitePoint < 0)
        return error("ArgumentInvalid", "Invalid astronomical development options");
    for (float g : opt.gains)
        if (!std::isfinite(g) || g <= 0 || g > 20)
            return error("ArgumentInvalid", "Color gains must be positive and at most 20");
    if (detail::imageHasInvalidCoveredColor(im))
        return error("ImageBufferInvalid", "Covered colors and coverage must be finite");
    AstroDevelopResult result;
    result.gains = opt.gains;
    std::array<std::vector<float>, 3> samples;
    const std::size_t stride = std::max<std::size_t>(1, im.pixelCount() / 100000);
    for (std::size_t p = 0; p < im.pixelCount(); p += stride) {
        if (!covered(im, p))
            continue;
        for (int c = 0; c < 3; ++c)
            samples[c].push_back(im.pixels[p * im.channels + c]);
    }
    if (samples[0].empty())
        return error("ImageCoverageEmpty", "No valid covered pixels");
    for (int c = 0; c < 3; ++c)
        result.sky[c] = quantile(samples[c], .5F);

    if (opt.stellarBalance) {
        StarDetectionOptions detection;
        detection.minPeak = 0;
        detection.sigmaThreshold = 3;
        detection.maxStars = 1500;
        const auto stars = StarDetector().detect(im, detection);
        if (!stars.ok)
            return error("StarDetectionFailed", "Cannot estimate stellar color balance");
        std::vector<float> redRatios, blueRatios;
        std::vector<Star> accepted;
        std::array<float, 3> maxima;
        for (int c = 0; c < 3; ++c)
            maxima[c] = *std::max_element(samples[c].begin(), samples[c].end());
        for (const auto& s : stars.stars) {
            const int x = std::lround(s.x), y = std::lround(s.y);
            if (x < 10 || y < 10 || x >= static_cast<int>(im.width) - 10 || y >= static_cast<int>(im.height) - 10)
                continue;
            if (s.fwhm < 1 || s.eccentricity > .6F)
                continue;
            if (std::any_of(accepted.begin(), accepted.end(),
                            [&](const Star& t) { return std::hypot(t.x - s.x, t.y - s.y) < 12; }))
                continue;
            std::array<std::vector<float>, 3> annulus;
            std::array<double, 3> flux{};
            std::array<float, 3> peak{};
            bool valid = true;
            int area = 0;
            for (int dy = -9; dy <= 9; ++dy)
                for (int dx = -9; dx <= 9; ++dx) {
                    const int rr = dx * dx + dy * dy;
                    const auto p = static_cast<std::size_t>(y + dy) * im.width + x + dx;
                    if (!covered(im, p)) {
                        valid = false;
                        continue;
                    }
                    if (rr <= 25) {
                        ++area;
                        for (int c = 0; c < 3; ++c) {
                            float v = im.pixels[p * im.channels + c];
                            flux[c] += v;
                            peak[c] = std::max(peak[c], v);
                        }
                    } else if (rr >= 49 && rr <= 81) {
                        for (int c = 0; c < 3; ++c)
                            annulus[c].push_back(im.pixels[p * im.channels + c]);
                    }
                }
            if (!valid)
                continue;
            for (int c = 0; c < 3; ++c) {
                flux[c] -= area * quantile(annulus[c], .5F);
                if (flux[c] <= 0 || peak[c] - result.sky[c] >= (maxima[c] - result.sky[c]) * .75F)
                    valid = false;
            }
            if (!valid)
                continue;
            const double rg = flux[0] / flux[1], bg = flux[2] / flux[1];
            if (rg < .1 || rg > 10 || bg < .1 || bg > 10)
                continue;
            redRatios.push_back(static_cast<float>(rg));
            blueRatios.push_back(static_cast<float>(bg));
            accepted.push_back(s);
        }
        result.calibrationStars = accepted.size();
        if (accepted.size() < 8)
            return error("CalibrationStarsInsufficient",
                         "Need at least eight usable stars; disable stellar balance or use manual gains");
        result.gains[0] /= quantile(redRatios, .5F);
        result.gains[2] /= quantile(blueRatios, .5F);
    }
    std::vector<float> luminance;
    for (std::size_t i = 0; i < samples[0].size(); ++i) {
        double value = 0;
        const float weights[] = {.2126F, .7152F, .0722F};
        for (int c = 0; c < 3; ++c)
            value += (static_cast<double>(samples[c][i]) - result.sky[c]) * result.gains[c] * weights[c];
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
            return error("ImageValueInvalid", "Scene dynamic range exceeds Float32 statistics");
        luminance.push_back(static_cast<float>(value));
    }
    if (opt.shadowNeutralization > 0) {
        // Include correlated sky fluctuations: a differenced/high-pass noise
        // estimator misses the low-frequency chroma that this control targets.
        std::vector<float> deviations;
        const double center=quantile(luminance,.5F);
        for(float v:luminance)deviations.push_back(float(std::abs(v-center)));
        result.shadowNoise=1.4826*quantile(deviations,.5F);
        if (!std::isfinite(result.shadowNoise))
            return error("ImageValueInvalid", "Shadow noise exceeds finite statistics");
    }
    // Choose a physical half-response from extended/stellar signal, not its maximum.
    const float white = quantile(luminance, .995F);
    result.toneScale = (opt.toneScale > 0 ? opt.toneScale : std::max(1.e-6F, white * (opt.toneCurve == AstroToneCurve::Asinh ? .012F : .18F))) / opt.brightness;
    if (!std::isfinite(result.toneScale) || result.toneScale <= 0)
        return error("ImageValueInvalid", "Tone scale is outside the supported range");
    result.whitePoint = opt.whitePoint > 0 ? opt.whitePoint : std::max(result.toneScale, quantile(luminance, .9999F));
    const double asinhNorm = std::asinh(static_cast<double>(result.whitePoint) / result.toneScale);
    if (!std::isfinite(asinhNorm) || asinhNorm <= 0)
        return error("ImageValueInvalid", "Asinh normalization is outside the supported range");
    const auto mapPositiveLuminance = [&](double lum) {
        double mapped = opt.background + (1 - opt.background) *
            (opt.toneCurve == AstroToneCurve::Asinh ? std::asinh(lum / result.toneScale) / asinhNorm
                                                  : lum / (result.toneScale + lum));
        if (opt.toneCurve == AstroToneCurve::Asinh && mapped > .85)
            mapped = .85 + .15 * -std::expm1(-(mapped - .85) / .15);
        return mapped;
    };
    // Estimate local stellar background only around compact maxima. Subtracting
    // a local pedestal before scaling preserves the galaxy beneath each star.
    // Broad nuclei fail the radial compactness check; overlapping masks use max,
    // so crowded stars are not repeatedly attenuated.
    ImageBuffer working = im;
    if (opt.starExposure < 1) {
        const bool selectiveBrightStars = opt.starPeakThreshold > 0;
        const int backgroundRadius = selectiveBrightStars ? 18 : 8;
        std::vector<float> detectionLuminance(im.pixelCount(), 0);
        for (std::size_t p = 0; p < im.pixelCount(); ++p)
            if (covered(im, p)) {
                double l = 0;
                const double weights[] = {.2126, .7152, .0722};
                for (int c = 0; c < 3; ++c)
                    l += (im.pixels[p * im.channels + c] - result.sky[c]) * result.gains[c] * weights[c];
                detectionLuminance[p] = static_cast<float>(std::max(0.0, l) / (result.toneScale + std::max(0.0, l)));
            }
        StarDetectionOptions d;
        d.minPeak = .05F;
        d.sigmaThreshold = .7F;
        d.maxStars = 40000;
        d.border = 10;
        const auto detected = StarDetector().detectLuminance(im.width, im.height, detectionLuminance, d);
        std::vector<float> strongest(im.pixelCount(), 0);
        if (detected.ok)
            for (const auto& star : detected.stars) {
                const int x = std::lround(star.x), y = std::lround(star.y);
                const int border = backgroundRadius + 2;
                if (x < border || y < border || x >= static_cast<int>(im.width) - border ||
                    y >= static_cast<int>(im.height) - border)
                    continue;
                std::array<std::vector<float>, 3> annulus;
                std::array<std::vector<float>, 3> nearAnnulus;
                std::array<std::vector<float>, 13> rings;
                std::vector<float> shoulder;
                bool valid = true;
                for (int dy = -backgroundRadius; dy <= backgroundRadius; ++dy)
                    for (int dx = -backgroundRadius; dx <= backgroundRadius; ++dx) {
                        const int rr = dx * dx + dy * dy;
                        const auto p = static_cast<std::size_t>(y + dy) * im.width + x + dx;
                        if (!covered(im, p)) {
                            valid = false;
                            continue;
                        }
                        const int skyInner = selectiveBrightStars ? 14 : 6;
                        if (rr >= skyInner * skyInner && rr <= backgroundRadius * backgroundRadius)
                            for (int c = 0; c < 3; ++c)
                                annulus[c].push_back(im.pixels[p * im.channels + c]);
                        if (selectiveBrightStars && rr >= 36 && rr <= 64)
                            for (int c = 0; c < 3; ++c)
                                nearAnnulus[c].push_back(im.pixels[p * im.channels + c]);
                        if (rr >= 12 && rr <= 20)
                            shoulder.push_back(im.pixels[p * im.channels] * .2126F +
                                               im.pixels[p * im.channels + 1] * .7152F +
                                               im.pixels[p * im.channels + 2] * .0722F);
                        if (selectiveBrightStars && rr <= 144)
                            rings[static_cast<int>(std::lround(std::sqrt(rr)))].push_back(
                                im.pixels[p * im.channels] * .2126F +
                                im.pixels[p * im.channels + 1] * .7152F +
                                im.pixels[p * im.channels + 2] * .0722F);
                    }
                if (!valid)
                    continue;
                std::array<float, 3> bg;
                for (int c = 0; c < 3; ++c)
                    bg[c] = quantile(annulus[c], .5F);
                // Use nearby sky for source selection/extent, so the curved
                // light of a galaxy behind a faint star cannot masquerade as
                // that star's extended halo. The outer sky remains useful for
                // subtracting the selected bright source's actual wings.
                auto localBg = bg;
                if (selectiveBrightStars)
                    for (int c = 0; c < 3; ++c) localBg[c] = quantile(nearAnnulus[c], .5F);
                const auto center = (static_cast<std::size_t>(y) * im.width + x) * im.channels;
                const float peak =
                    im.pixels[center] * .2126F + im.pixels[center + 1] * .7152F + im.pixels[center + 2] * .0722F;
                const float pedestal = localBg[0] * .2126F + localBg[1] * .7152F + localBg[2] * .0722F;
                if (peak <= pedestal || (peak - quantile(shoulder, .5F)) / (peak - pedestal) <
                                           (selectiveBrightStars ? .45F : .65F))
                    continue;
                double attenuation = 1 - opt.starExposure;
                if (opt.starPeakThreshold > 0) {
                    const double weights[] = {.2126, .7152, .0722};
                    double calibratedPeak = 0;
                    for (int c = 0; c < 3; ++c)
                        calibratedPeak += (im.pixels[center + c] - localBg[c]) * result.gains[c] * weights[c];
                    const double mappedPeak = mapPositiveLuminance(std::max(0.0, calibratedPeak));
                    const double t = std::clamp((mappedPeak - opt.starPeakThreshold) /
                                               std::max(1e-6, 1.0 - opt.starPeakThreshold), 0.0, 1.0);
                    attenuation *= t * t * (3 - 2 * t);
                    if (attenuation <= 0) continue;
                }
                int innerRadius = 4;
                if (selectiveBrightStars)
                    for (int radius = 4; radius <= 12; ++radius)
                        if (!rings[radius].empty() && quantile(rings[radius], .5F) > pedestal + .1F * (peak - pedestal))
                            innerRadius = radius + 2;
                const int outerRadius = innerRadius + 4;
                ++result.adjustedStars;
                for (int dy = -outerRadius; dy <= outerRadius; ++dy)
                    for (int dx = -outerRadius; dx <= outerRadius; ++dx) {
                        const float radius = std::hypot(x + dx - star.x, y + dy - star.y);
                        const float t = std::clamp((outerRadius - radius) / 4, 0.0F, 1.0F);
                        const float weight = t * t * (3 - 2 * t) * attenuation;
                        const auto p = static_cast<std::size_t>(y + dy) * im.width + x + dx;
                        if (weight <= strongest[p])
                            continue;
                        strongest[p] = weight;
                        for (int c = 0; c < 3; ++c) {
                            const auto o = p * im.channels + c;
                            working.pixels[o] =
                                im.pixels[o] - std::max(0.0F, im.pixels[o] - bg[c]) * weight;
                        }
                    }
            }
    }
    result.image = im;
    for (std::size_t p = 0; p < im.pixelCount(); ++p) {
        const auto o = p * im.channels;
        bool valid = im.channels != 4 || (std::isfinite(im.pixels[o + 3]) && im.pixels[o + 3] > 0);
        for (int c = 0; c < 3; ++c)
            valid = valid && std::isfinite(im.pixels[o + c]);
        if (!valid) {
            for (int c = 0; c < im.channels; ++c)
                result.image.pixels[o + c] = 0;
            continue;
        }
        std::array<double, 3> color;
        for (int c = 0; c < 3; ++c)
            color[c] = (static_cast<double>(working.pixels[o + c]) - result.sky[c]) * result.gains[c];
        const double lum = color[0] * .2126 + color[1] * .7152 + color[2] * .0722;
        const double denominator = result.toneScale + std::max(0.0, lum);
        const bool useAsinh = opt.toneCurve == AstroToneCurve::Asinh;
        const double slope = (1 - opt.background) / (result.toneScale * (useAsinh ? asinhNorm : 1));
        double mapped;
        if (lum >= 0) {
            mapped = mapPositiveLuminance(lum);
        } else {
            mapped = opt.background > 0 ? opt.background * std::exp(slope * lum / opt.background) : 0;
        }
        const double colorSlope = useAsinh ? (lum > 0 ? (mapped - opt.background) / lum : slope)
                                          : (1 - opt.background) / denominator;
        const double shadowChroma = lum < 0 ? (opt.background > 0 ? mapped / opt.background : 0) : 1;
        // A gradual highlight chroma rolloff keeps bright cores from turning
        // into flat, saturated color patches while retaining middle-tone color.
        const double signal=std::max(0.0,lum), limit=3*result.shadowNoise;
        const double confidence=limit>0 ? signal*signal/(signal*signal+limit*limit) : 1;
        const double neutralization=1-opt.shadowNeutralization*(1-confidence);
        const double highlightChroma = 1 - .65 * mapped * mapped * mapped * mapped;
        for (int c = 0; c < 3; ++c)
            color[c] = mapped + (color[c] - lum) * colorSlope * opt.saturation * shadowChroma *
                                    highlightChroma * neutralization;
        // Compress chroma smoothly near the gamut boundary. Merely scaling an
        // out-of-gamut color to the boundary makes a channel exactly 0 or 1,
        // flattening colored highlights even when luminance is still graded.
        // A rational shoulder preserves the luminance and chroma direction,
        // leaves colors within 80% of the available gamut alone, and retains
        // more endpoint headroom than an exponentially saturating shoulder.
        double availableChroma = std::numeric_limits<double>::infinity();
        for (double c : color) {
            if (c > mapped)
                availableChroma = std::min(availableChroma, (1 - mapped) / (c - mapped));
            if (c < mapped)
                availableChroma = std::min(availableChroma, mapped / (mapped - c));
        }
        const double chroma = detail::softGamutScale(availableChroma);
        for (int c = 0; c < 3; ++c)
            result.image.pixels[o + c] =
                static_cast<float>(std::clamp(mapped + (color[c] - mapped) * chroma, 0.0, 1.0));
    }
    result.image.colorEncoding = ColorEncoding::SRGB;
    result.ok = true;
    return result;
}
} // namespace photonstack
