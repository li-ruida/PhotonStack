#include "photonstack/AstroDevelop.hpp"
#include "DisplayGamut.hpp"
#include "MaskedImageSampling.hpp"
#include "photonstack/StarDetector.hpp"
#include "photonstack/StarColorCleanup.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
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

AstroDevelopResult AstroDevelop::apply(const ImageBuffer& im, const AstroDevelopOptions& opt,
                                     const ImageBuffer* statisticsReference) const {
    if (im.empty() || (im.channels != 3 && im.channels != 4) || im.pixels.size() != im.sampleCount())
        return error("ImageBufferInvalid", "Develop requires a non-empty RGB image");
    if (im.colorEncoding != ColorEncoding::Linear)
        return error("ImageColorEncodingMismatch", "Develop requires linear data, before display stretching");
    if (!std::isfinite(opt.background) || opt.background < 0 || opt.background >= .5F ||
        !std::isfinite(opt.skyPercentile) || opt.skyPercentile < .05F || opt.skyPercentile > .5F ||
        !std::isfinite(opt.brightness) || opt.brightness <= 0 || opt.brightness > 20 || !std::isfinite(opt.toneScale) ||
        opt.toneScale < 0 || !std::isfinite(opt.saturation) || opt.saturation < 0 || opt.saturation > 3 ||
        !std::isfinite(opt.starExposure) || opt.starExposure < .05F || opt.starExposure > 1 ||
        !std::isfinite(opt.starPeakThreshold) || opt.starPeakThreshold < 0 || opt.starPeakThreshold > 1 ||
        !std::isfinite(opt.shadowNeutralization) || opt.shadowNeutralization < 0 || opt.shadowNeutralization > 1 ||
        !std::isfinite(opt.starColorCleanup) || opt.starColorCleanup < 0 || opt.starColorCleanup > 1 ||
        !std::isfinite(opt.whitePoint) || opt.whitePoint < 0)
        return error("ArgumentInvalid", "Invalid astronomical development options");
    for (float g : opt.gains)
        if (!std::isfinite(g) || g <= 0 || g > 20)
            return error("ArgumentInvalid", "Color gains must be positive and at most 20");
    if (detail::imageHasInvalidCoveredColor(im))
        return error("ImageBufferInvalid", "Covered colors and coverage must be finite");
    if (statisticsReference && (statisticsReference->width != im.width || statisticsReference->height != im.height ||
        statisticsReference->channels != im.channels || statisticsReference->pixels.size() != im.sampleCount() ||
        statisticsReference->colorEncoding != ColorEncoding::Linear ||
        detail::imageHasInvalidCoveredColor(*statisticsReference)))
        return error("ArgumentInvalid", "Development statistics reference requires matching finite linear RGB geometry");
    const auto& statistics = statisticsReference ? *statisticsReference : im;
    AstroDevelopResult result;
    result.gains = opt.gains;
    std::array<std::vector<float>, 3> samples;
    constexpr std::size_t sampleLimit = 100000;
    for (auto& channel : samples) channel.reserve(std::min(sampleLimit, statistics.pixelCount()));
    std::array<float, 3> maxima;
    maxima.fill(std::numeric_limits<float>::lowest());
    std::mt19937_64 generator(13831);
    std::size_t coveredCount = 0;
    // A regular stride can sample just one column phase, giving a colored
    // sky pedestal. A fixed-seed reservoir samples covered pixels evenly and
    // bounds the memory used by the percentiles.
    for (std::size_t p = 0; p < statistics.pixelCount(); ++p) {
        if (!covered(statistics, p))
            continue;
        const auto* rgb = &statistics.pixels[p * statistics.channels];
        if (opt.stellarBalance) {
            // The bright-star exclusion depends on actual peaks. Missing a
            // stellar core in the sky sample must not disqualify other stars.
            for (int c = 0; c < 3; ++c) maxima[c] = std::max(maxima[c], rgb[c]);
        }
        ++coveredCount;
        if (coveredCount <= sampleLimit) {
            for (int c = 0; c < 3; ++c) samples[c].push_back(rgb[c]);
        } else {
            const auto slot = std::uniform_int_distribution<std::size_t>(0, coveredCount - 1)(generator);
            if (slot < sampleLimit)
                for (int c = 0; c < 3; ++c) samples[c][slot] = rgb[c];
        }
    }
    if (samples[0].empty())
        return error("ImageCoverageEmpty", "No valid covered pixels");
    for (int c = 0; c < 3; ++c)
        result.sky[c] = quantile(samples[c], opt.skyPercentile);

    if (opt.stellarBalance) {
        // Sky-reference styling must not change which stars calibrate color.
        // Keep the historical saturation rejection pedestal independent of
        // the user-selected display black point.
        std::array<float, 3> calibrationSky;
        for (int c = 0; c < 3; ++c)
            calibrationSky[c] = quantile(samples[c], .5F);
        StarDetectionOptions detection;
        // Scientific samples can be negative after pedestal subtraction.
        // Detection already applies a sky-relative threshold; an absolute zero
        // floor would discard the same stars solely because the offset changed.
        detection.minPeak = -std::numeric_limits<float>::max();
        detection.sigmaThreshold = 3;
        detection.maxStars = 1500;
        std::vector<float> redRatios, blueRatios;
        std::vector<Star> accepted;
        // A few very bright stars can raise the global detection threshold
        // above every unsaturated calibrator. Retry against the sky noise
        // only if needed; require strong local aperture flux in all bands.
        for (const bool backgroundThreshold : {false, true}) {
            if (accepted.size() >= 8) break;
            detection.robustStatistics = backgroundThreshold;
            const auto stars = StarDetector().detect(statistics, detection);
            if (!stars.ok)
                return error("StarDetectionFailed", "Cannot estimate stellar color balance");
            for (const auto& s : stars.stars) {
                const int x = std::lround(s.x), y = std::lround(s.y);
                if (x < 10 || y < 10 || x >= static_cast<int>(statistics.width) - 10 || y >= static_cast<int>(statistics.height) - 10)
                    continue;
                if (s.fwhm < 1 || s.eccentricity > .6F)
                    continue;
                if (std::any_of(accepted.begin(), accepted.end(),
                                [&](const Star& t) { return std::hypot(t.x - s.x, t.y - s.y) < 12; }))
                    continue;
                std::array<std::vector<float>, 3> annulus;
                std::array<double, 3> flux{};
                std::array<float, 3> peak;
                peak.fill(-std::numeric_limits<float>::max());
                bool valid = true;
                int area = 0;
                for (int dy = -9; dy <= 9; ++dy)
                    for (int dx = -9; dx <= 9; ++dx) {
                        const int rr = dx * dx + dy * dy;
                        const auto p = static_cast<std::size_t>(y + dy) * statistics.width + x + dx;
                        if (!covered(statistics, p)) {
                            valid = false;
                            continue;
                        }
                        if (rr <= 25) {
                            ++area;
                            for (int c = 0; c < 3; ++c) {
                                float v = statistics.pixels[p * statistics.channels + c];
                                flux[c] += v;
                                peak[c] = std::max(peak[c], v);
                            }
                        } else if (rr >= 49 && rr <= 81) {
                            for (int c = 0; c < 3; ++c)
                                annulus[c].push_back(statistics.pixels[p * statistics.channels + c]);
                        }
                    }
                if (!valid)
                    continue;
                for (int c = 0; c < 3; ++c) {
                    const float sky = quantile(annulus[c], .5F);
                    flux[c] -= area * sky;
                    if (backgroundThreshold) {
                        for (float& v : annulus[c]) v = std::abs(v - sky);
                        const double noise = 1.4826 * quantile(annulus[c], .5F);
                        if (flux[c] <= 8 * noise * std::sqrt(area)) valid = false;
                    }
                    if (flux[c] <= 0 || peak[c] - calibrationSky[c] >= (maxima[c] - calibrationSky[c]) * .75F)
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
    double skyVariation = 0;
    if (opt.shadowNeutralization > 0 || opt.toneScale == 0) {
        // Include correlated sky fluctuations: a differenced/high-pass noise
        // estimator misses the low-frequency chroma that this control targets.
        std::vector<float> deviations;
        const double center=quantile(luminance,.5F);
        for(float v:luminance)deviations.push_back(float(std::abs(v-center)));
        skyVariation=1.4826*quantile(deviations,.5F);
        if (!std::isfinite(skyVariation))
            return error("ImageValueInvalid", "Shadow noise exceeds finite statistics");
        if (opt.shadowNeutralization > 0) result.shadowNoise = skyVariation;
    }
    // Choose a physical half-response from extended/stellar signal, not its maximum.
    const float white = quantile(luminance, .995F);
    // A signal-only percentile can put the half-response below sky noise in
    // short/sparse exposures. That makes positive fluctuations bright while
    // negative ones collapse to black. Keep automatic stretching conservative
    // in physical data units; explicit tone scale and brightness remain user
    // controls. This is a scene-variation guard, not a detector-noise estimate.
    const double autoScale = std::max({1.e-6, double(white) *
        (opt.toneCurve == AstroToneCurve::Asinh ? .012 : .18), 12 * skyVariation});
    result.toneScale = (opt.toneScale > 0 ? opt.toneScale : autoScale) / opt.brightness;
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
    if (opt.starColorCleanup > 0) {
        auto cleaned = StarColorCleanup().apply(working, result.gains, opt.starColorCleanup);
        if (!cleaned.ok) return error(cleaned.errorCode.c_str(), cleaned.message.c_str());
        result.colorCleanedStars = cleaned.correctedStars;
        working = std::move(cleaned.image);
    }
    result.image = im;
    const bool useAsinh = opt.toneCurve == AstroToneCurve::Asinh;
    const double slope = (1 - opt.background) / (result.toneScale * (useAsinh ? asinhNorm : 1));
    const double skyLight = detail::decodeSRGB(opt.background);
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
        double mapped;
        if (lum >= 0) {
            mapped = mapPositiveLuminance(lum);
        } else {
            mapped = opt.background > 0 ? opt.background * std::exp(slope * lum / opt.background) : 0;
        }
        // The tone curve always sets neutral brightness in sRGB code values.
        // Older renders applied stellar color ratios directly in that space.
        const double neutral = opt.linearDisplayColor ? detail::decodeSRGB(mapped) : mapped;
        double colorSlope;
        double shadowChroma = 1;
        if (!opt.linearDisplayColor) {
            const double denominator = result.toneScale + std::max(0.0, lum);
            colorSlope = useAsinh ? (lum > 0 ? (mapped - opt.background) / lum : slope)
                                  : (1 - opt.background) / denominator;
            if (lum < 0) shadowChroma = opt.background > 0 ? mapped / opt.background : 0;
        } else if (lum > 0 && mapped > opt.background) {
            colorSlope = (neutral - skyLight) / lum;
        } else {
            const double decodeSlope = mapped <= .04045 ? 1 / 12.92 : 2.4 * neutral / (mapped + .055);
            const double shadow = lum < 0 && opt.background > 0 ? mapped / opt.background : 1;
            colorSlope = slope * decodeSlope * shadow;
        }
        // A gradual highlight chroma rolloff keeps bright cores from turning
        // into flat, saturated color patches while retaining middle-tone color.
        const double signal=std::max(0.0,lum), limit=3*result.shadowNoise;
        const double confidence=limit>0 ? signal*signal/(signal*signal+limit*limit) : 1;
        const double neutralization=1-opt.shadowNeutralization*(1-confidence);
        const double highlightChroma = 1 - .65 * mapped * mapped * mapped * mapped;
        for (int c = 0; c < 3; ++c)
            color[c] = neutral + (color[c] - lum) * colorSlope * opt.saturation * shadowChroma *
                                    highlightChroma * neutralization;
        // Compress chroma smoothly near the gamut boundary. Merely scaling an
        // out-of-gamut color to the boundary makes a channel exactly 0 or 1,
        // flattening colored highlights even when luminance is still graded.
        // A rational shoulder preserves the luminance and chroma direction,
        // leaves colors within 80% of the available gamut alone, and retains
        // more endpoint headroom than an exponentially saturating shoulder.
        double availableChroma = std::numeric_limits<double>::infinity();
        for (double c : color) {
            if (c > neutral)
                availableChroma = std::min(availableChroma, (1 - neutral) / (c - neutral));
            if (c < neutral)
                availableChroma = std::min(availableChroma, neutral / (neutral - c));
        }
        const double chroma = detail::softGamutScale(availableChroma);
        for (int c = 0; c < 3; ++c) {
            const double value = std::clamp(neutral + (color[c] - neutral) * chroma, 0.0, 1.0);
            result.image.pixels[o + c] = static_cast<float>(opt.linearDisplayColor ? detail::encodeSRGB(value) : value);
        }
        ++result.displayPixels;
        const auto* rgb = result.image.pixels.data() + o;
        result.nearBlackPixels += *std::max_element(rgb, rgb + 3) <= 1.0F / 255;
        result.nearWhitePixels += *std::min_element(rgb, rgb + 3) >= .995F;
    }
    result.image.colorEncoding = ColorEncoding::SRGB;
    result.ok = true;
    return result;
}
} // namespace photonstack
