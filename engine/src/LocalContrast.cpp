#include "photonstack/LocalContrast.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "MaskedImageSampling.hpp"
#include "DisplayGamut.hpp"
#include "photonstack/StarDetector.hpp"

namespace photonstack {
namespace {

LocalContrastResult localContrastError(std::string code, std::string message) {
    LocalContrastResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

std::vector<float> gaussianKernel(std::uint32_t radius) {
    const int r = static_cast<int>(radius);
    const float sigma = std::max(1.0F, static_cast<float>(radius) * 0.5F);
    std::vector<float> kernel(static_cast<std::size_t>(r * 2 + 1), 0.0F);
    float sum = 0.0F;
    for (int i = -r; i <= r; ++i) {
        const float value = std::exp(-(static_cast<float>(i * i)) / (2.0F * sigma * sigma));
        kernel[static_cast<std::size_t>(i + r)] = value;
        sum += value;
    }
    for (auto& value : kernel) {
        value /= sum;
    }
    return kernel;
}

ImageBuffer gaussianBlur(const ImageBuffer& image, std::uint32_t radius, bool fillMasked = false) {
    const auto kernel = gaussianKernel(radius);
    ImageBuffer temp = image;
    ImageBuffer output = image;
    const int r = static_cast<int>(radius);
    const auto colorChannels = detail::colorChannelCount(image);
    std::vector<float> horizontalCoverage(image.pixelCount(), 0.0F);

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            const auto offset = pixel * image.channels;
            double sums[3] = {0.0, 0.0, 0.0};
            double weightSum = 0.0;
            for (int k = -r; k <= r; ++k) {
                const auto samplePixel = detail::clampedPixelIndex(image, static_cast<int>(x) + k, static_cast<int>(y));
                const float coverage = detail::pixelCoverage(image, samplePixel);
                if (!detail::pixelHasValidColor(image, samplePixel) || coverage <= 1.0e-6F) {
                    continue;
                }
                const double weight = kernel[static_cast<std::size_t>(k + r)] * coverage;
                const auto sampleOffset = samplePixel * image.channels;
                for (std::uint16_t c = 0; c < colorChannels; ++c) {
                    sums[c] += static_cast<double>(image.pixels[sampleOffset + c]) * weight;
                }
                weightSum += weight;
            }
            if (weightSum > 0.0F) {
                horizontalCoverage[pixel] = static_cast<float>(weightSum);
                for (std::uint16_t c = 0; c < colorChannels; ++c) {
                    temp.pixels[offset + c] = static_cast<float>(sums[c] / weightSum);
                }
            }
        }
    }

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            const auto offset = pixel * image.channels;
            if (!fillMasked && !detail::pixelHasValidColor(image, pixel)) {
                detail::clearMaskedPixel(output, pixel);
                continue;
            }

            double sums[3] = {0.0, 0.0, 0.0};
            double weightSum = 0.0;
            for (int k = -r; k <= r; ++k) {
                const auto samplePixel = detail::clampedPixelIndex(temp, static_cast<int>(x), static_cast<int>(y) + k);
                if (horizontalCoverage[samplePixel] <= 1.0e-6F) {
                    continue;
                }
                const double weight = kernel[static_cast<std::size_t>(k + r)] * horizontalCoverage[samplePixel];
                const auto sampleOffset = samplePixel * temp.channels;
                for (std::uint16_t c = 0; c < colorChannels; ++c) {
                    sums[c] += static_cast<double>(temp.pixels[sampleOffset + c]) * weight;
                }
                weightSum += weight;
            }
            for (std::uint16_t c = 0; c < colorChannels; ++c) {
                output.pixels[offset + c] =
                    weightSum > 0.0 ? static_cast<float>(sums[c] / weightSum) : image.pixels[offset + c];
            }
        }
    }
    return output;
}

LocalContrastResult coolFromContinuum(const ImageBuffer& image, const ImageBuffer& coarse,
                                       const std::vector<float>& coverage, double amount) {
    const auto count = image.pixelCount();
    ImageBuffer output = image;
    for (std::size_t p = 0; p < count; ++p) {
        if (coverage[p] <= 1e-6F) {
            detail::clearMaskedPixel(output, p);
            continue;
        }
        const auto offset = p * image.channels;
        const double base = coarse.pixels[p * 4];
        double low = std::clamp((base - .12) / .13, 0., 1.);
        low = low * low * (3 - 2 * low);
        double high = std::clamp((1 - base) / .2, 0., 1.);
        high = high * high * (3 - 2 * high);
        const double requested = amount * base * low * high;
        const double floor = .8 * std::min(image.pixels[offset], image.pixels[offset + 1]);
        const double pedestal = std::min(requested, floor);
        output.pixels[offset] = static_cast<float>(image.pixels[offset] - pedestal);
        output.pixels[offset + 1] = static_cast<float>(image.pixels[offset + 1] - pedestal);
    }
    LocalContrastResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

LocalContrastResult protectedStructure(const ImageBuffer& image, const LocalContrastOptions& options,
                                      std::optional<ContinuumCoolingOptions> coolingOptions = std::nullopt) {
    if (image.channels < 3 || !options.clampOutput || options.fineRadius == 0 || options.fineRadius >= options.radius) {
        return localContrastError("ArgumentInvalid",
                                  "Protected structure requires display RGB and fine-radius < radius");
    }
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        if (!detail::pixelHasValidColor(image, p))
            continue;
        for (int c = 0; c < 3; ++c) {
            if (image.pixels[p * image.channels + c] < 0 || image.pixels[p * image.channels + c] > 1)
                return localContrastError("ImageValueInvalid", "Protected structure requires samples in [0,1]");
        }
    }
    const auto count = image.pixelCount();
    std::vector<float> luma(count), coverage(count), residual(count), protection(count, 0);
    for (std::size_t p = 0; p < count; ++p) {
        coverage[p] = detail::pixelCoverage(image, p);
        if (coverage[p] <= 1e-6F)
            continue;
        const auto o = p * image.channels;
        luma[p] = image.pixels[o] * .2126F + image.pixels[o + 1] * .7152F + image.pixels[o + 2] * .0722F;
    }
    ImageBuffer broad = image;
    broad.channels = 4;
    broad.format = PixelFormat::Float32RGBA;
    broad.pixels.assign(broad.sampleCount(), 0);
    if (coolingOptions && coolingOptions->estimator == CoolingContinuumEstimator::Opening) {
        // A fixed grayscale opening removes positive compact peaks without
        // introducing a source/no-source decision into the continuum map.
        // This estimates only the display pedestal; output samples are not
        // replaced by the opened image.
        constexpr int openingRadius=6;
        auto extremum = [&](const std::vector<float>& input, bool vertical, bool minimum) {
            std::vector<float> result(count);
            for(int y=0;y<int(image.height);++y) for(int x=0;x<int(image.width);++x) {
                float value=minimum?std::numeric_limits<float>::infinity():-std::numeric_limits<float>::infinity();
                for(int k=-openingRadius;k<=openingRadius;++k) {
                    const int xx=std::clamp(x+(vertical?0:k),0,int(image.width)-1);
                    const int yy=std::clamp(y+(vertical?k:0),0,int(image.height)-1);
                    const auto q=static_cast<std::size_t>(yy)*image.width+xx;
                    if(coverage[q]>1e-6F) value=minimum?std::min(value,input[q]):std::max(value,input[q]);
                }
                result[static_cast<std::size_t>(y)*image.width+x]=std::isfinite(value)?value:0;
            }
            return result;
        };
        auto opened=extremum(luma,false,true);
        opened=extremum(opened,true,true);
        opened=extremum(opened,false,false);
        opened=extremum(opened,true,false);
        for(std::size_t p=0;p<count;++p) {
            for(int c=0;c<3;++c) broad.pixels[p*4+c]=opened[p];
            broad.pixels[p*4+3]=coverage[p];
        }
        const auto coarse=gaussianBlur(broad,options.radius);
        return coolFromContinuum(image, coarse, coverage, coolingOptions->amount);
    }
    // A small median suppresses compact peaks in the structure estimate only.
    // Original image samples (including their fine detail) are retained in output.
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto p = static_cast<std::size_t>(y) * image.width + x;
            if (coverage[p] <= 1e-6F) {
                detail::clearMaskedPixel(broad, p);
                continue;
            }
            float values[25];
            std::size_t n = 0;
            for (int j = -2; j <= 2; ++j)
                for (int i = -2; i <= 2; ++i) {
                    const auto q = detail::clampedPixelIndex(image, static_cast<int>(x) + i, static_cast<int>(y) + j);
                    if (coverage[q] > 1e-6F)
                        values[n++] = luma[q];
                }
            std::nth_element(values, values + n / 2, values + n);
            const float middle = values[n / 2];
            for (int c = 0; c < 3; ++c)
                broad.pixels[p * 4 + c] = middle;
            broad.pixels[p * 4 + 3] = coverage[p];
            residual[p] = luma[p] - middle;
        }
    }
    StarDetectionOptions detection;
    detection.minPeak = .05F;
    detection.sigmaThreshold = 4;
    detection.maxStars = 20000;
    const auto stars =
        StarDetector{}.detectLuminanceWithCoverage(image.width, image.height, residual, coverage, detection);
    if (!stars.ok && image.width >= 3 && image.height >= 3)
        return localContrastError(stars.errorCode, stars.message);
    auto candidates = stars.stars;
    // Saturated/broad bright cores can have almost no median residual. Include
    // maxima in the original luminance as well, then reject extended peaks by
    // their local radial contrast instead of a global image threshold alone.
    detection.minPeak = .2F;
    detection.sigmaThreshold = .7F;
    detection.border = 12;
    const auto brightStars =
        StarDetector{}.detectLuminanceWithCoverage(image.width, image.height, luma, coverage, detection);
    if (brightStars.ok)
        candidates.insert(candidates.end(), brightStars.stars.begin(), brightStars.stars.end());
    for (const auto& star : candidates) {
        const auto cx = static_cast<int>(std::lround(star.x)), cy = static_cast<int>(std::lround(star.y));
        const auto center = detail::clampedPixelIndex(image, cx, cy);
        std::vector<float> annulus;
        std::array<std::vector<float>, 13> rings;
        for (int j = -12; j <= 12; ++j)
            for (int i = -12; i <= 12; ++i) {
                const int rr = i * i + j * j;
                if (rr > 144)
                    continue;
                const auto q = detail::clampedPixelIndex(image, cx + i, cy + j);
                if (coverage[q] > 1e-6F) {
                    rings[static_cast<int>(std::lround(std::sqrt(rr)))].push_back(luma[q]);
                    if (rr >= 64)
                        annulus.push_back(luma[q]);
                }
            }
        if (annulus.empty())
            continue;
        std::nth_element(annulus.begin(), annulus.begin() + annulus.size() / 2, annulus.end());
        const float pedestal = annulus[annulus.size() / 2];
        const float amplitude = luma[center] - pedestal;
        if (amplitude < .04F)
            continue;
        auto& shoulder = rings[3];
        if (!shoulder.empty()) {
            std::nth_element(shoulder.begin(), shoulder.begin() + shoulder.size() / 2, shoulder.end());
            const auto o = center * image.channels;
            const bool saturated = std::max({image.pixels[o], image.pixels[o + 1], image.pixels[o + 2]}) >= .98F;
            if (!saturated && (luma[center] - shoulder[shoulder.size() / 2]) / amplitude < .45F)
                continue;
        }
        float inner = 4;
        for (int r = 3; r <= 10; ++r) {
            auto& ring = rings[r];
            if (ring.empty())
                continue;
            std::nth_element(ring.begin(), ring.begin() + ring.size() / 2, ring.end());
            if (ring[ring.size() / 2] > pedestal + .1F * amplitude)
                inner = r + 3;
        }
        const int outer = static_cast<int>(inner) + 6;
        for (int j = -outer; j <= outer; ++j)
            for (int i = -outer; i <= outer; ++i) {
                const int x = cx + i, y = cy + j;
                if (x < 0 || y < 0 || x >= static_cast<int>(image.width) || y >= static_cast<int>(image.height))
                    continue;
                const float d = std::hypot(static_cast<float>(x) - star.x, static_cast<float>(y) - star.y);
                float mask = std::clamp((outer - d) / (outer - inner), 0.F, 1.F);
                mask = mask * mask * (3 - 2 * mask);
                const auto p = static_cast<std::size_t>(y) * image.width + x;
                protection[p] = std::max(protection[p], mask);
            }
    }
    // Exclude compact sources from both structure estimates; protection alone
    // can leave a ring where a source's blurred contribution meets the mask.
    for (std::size_t p = 0; p < count; ++p)
        broad.pixels[p * 4 + 3] *= 1 - protection[p];
    // Fill only the structure-estimation layer with a common smooth continuum.
    // Independently normalizing two scales around dense holes can create false
    // spots in a smooth galaxy. Neither this estimate nor an inpainted star is
    // copied into the output image: the original samples remain the base.
    const auto continuum = gaussianBlur(broad, options.radius, true);
    std::vector<float> filled(count), next(count);
    std::vector<std::size_t> holes;
    for (std::size_t p = 0; p < count; ++p) {
        filled[p] = broad.pixels[p * 4] + (continuum.pixels[p * 4] - broad.pixels[p * 4]) * protection[p];
        if (protection[p] > 0 && coverage[p] > 1e-6F)
            holes.push_back(p);
    }
    next = filled;
    // Harmonic continuation follows the local boundary of each stellar hole;
    // a large Gaussian alone would bias a curved galaxy under those holes.
    for (int iteration = 0; iteration < 256; ++iteration) {
        float largest = 0;
        for (auto p : holes) {
            const auto x = p % image.width, y = p / image.width;
            double sum = 0, weight = 0;
            const std::array<std::size_t, 4> neighbors{x > 0 ? p - 1 : p, x + 1 < image.width ? p + 1 : p,
                                                       y > 0 ? p - image.width : p,
                                                       y + 1 < image.height ? p + image.width : p};
            for (auto q : neighbors)
                if (coverage[q] > 1e-6F) {
                    sum += filled[q] * coverage[q];
                    weight += coverage[q];
                }
            const float value = weight > 0 ? static_cast<float>(sum / weight) : filled[p];
            next[p] = broad.pixels[p * 4] * (1 - protection[p]) + value * protection[p];
            largest = std::max(largest, std::abs(next[p] - filled[p]));
        }
        filled.swap(next);
        if (largest < 1e-5F)
            break;
    }
    for (std::size_t p = 0; p < count; ++p) {
        for (int c = 0; c < 3; ++c)
            broad.pixels[p * 4 + c] = filled[p];
        broad.pixels[p * 4 + 3] = coverage[p];
    }
    const auto fine = gaussianBlur(broad, options.fineRadius);
    const auto coarse = gaussianBlur(broad, options.radius);
    if (coolingOptions)
        return coolFromContinuum(image, coarse, coverage, coolingOptions->amount);
    ImageBuffer mappedContinuum;
    if (std::any_of(options.continuumCurvePoints.begin(), options.continuumCurvePoints.end(),
                    [](const auto& point) { return point.input != point.output; })) {
        CurvesOptions curveOptions;
        curveOptions.points = options.continuumCurvePoints;
        const auto curve = Curves{}.apply(coarse, curveOptions);
        if (!curve.ok)
            return localContrastError(curve.errorCode, curve.message);
        mappedContinuum = curve.image;
    }
    ImageBuffer output = image;
    for (std::size_t p = 0; p < count; ++p) {
        if (coverage[p] <= 1e-6F) {
            detail::clearMaskedPixel(output, p);
            continue;
        }
        const auto o = p * image.channels;
        const double detailValue = fine.pixels[p * 4] - coarse.pixels[p * 4];
        const double base = coarse.pixels[p * 4];
        double skyGate = std::clamp((base - .12) / .13, 0., 1.);
        skyGate = skyGate * skyGate * (3 - 2 * skyGate);
        const double highlightGate = std::clamp((.95 - luma[p]) / .2, 0., 1.);
        double delta =
            std::abs(detailValue) < options.threshold ? 0 : options.amount * detailValue * skyGate * highlightGate;
        const double minimum = std::min({image.pixels[o], image.pixels[o + 1], image.pixels[o + 2]});
        const double maximum = std::max({image.pixels[o], image.pixels[o + 1], image.pixels[o + 2]});
        // Ease into the available gamut rather than clipping the added
        // structure abruptly into flat stellar highlights or black troughs.
        const double headroom = delta >= 0 ? std::max(0.0, 1 - detail::displayCodeStep - maximum)
                                          : std::max(0.0, minimum - detail::displayCodeStep);
        delta = headroom > 0 ? headroom * std::tanh(delta / headroom) : 0;
        const double chroma = 1 - (1 - options.starChroma) * protection[p];
        for (int c = 0; c < 3; ++c)
            output.pixels[o + c] = static_cast<float>(
                image.pixels[o + c] + delta + (static_cast<double>(image.pixels[o + c]) - luma[p]) * (chroma - 1));
        if (!mappedContinuum.empty()) {
            const double originalBase = std::clamp(static_cast<double>(coarse.pixels[p * 4]), 0., 1.);
            const double mappedBase = std::clamp(static_cast<double>(mappedContinuum.pixels[p * 4]), 0., 1.);
            // Preserve the residual profile on a locally constant continuum.
            // Unlike a per-pixel nonlinear curve, this is affine in the source
            // signal. One scale for all channels also preserves its color
            // differences. The scale is not a photometric correction.
            double residualScale = 1.;
            if (originalBase > 0)
                residualScale = std::min(residualScale, mappedBase / originalBase);
            if (originalBase < 1)
                residualScale = std::min(residualScale, (1 - mappedBase) / (1 - originalBase));
            for (int c = 0; c < 3; ++c)
                output.pixels[o + c] = static_cast<float>(std::clamp(
                    mappedBase + residualScale * (output.pixels[o + c] - originalBase), 0., 1.));
        }
    }
    LocalContrastResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

} // namespace

LocalContrastResult LocalContrast::coolContinuum(
    const ImageBuffer& image, const ContinuumCoolingOptions& options) const {
    if (image.empty() || (image.channels != 3 && image.channels != 4) ||
        image.pixels.size() != image.sampleCount() || detail::imageHasInvalidCoveredColor(image))
        return localContrastError("ImageBufferInvalid", "Continuum cooling requires finite RGB or RGBA samples");
    if (image.colorEncoding != ColorEncoding::SRGB)
        return localContrastError("ImageValueInvalid", "Continuum cooling requires an sRGB display image");
    if (!std::isfinite(options.amount) || options.amount < 0 || options.amount > .12 ||
        options.radius < 4 || options.radius > 64)
        return localContrastError("ArgumentInvalid", "Cooling amount must be in [0,0.12] and radius in [4,64]");
    if (options.estimator != CoolingContinuumEstimator::SourceExcluded &&
        options.estimator != CoolingContinuumEstimator::Opening)
        return localContrastError("ArgumentInvalid", "Unknown continuum estimator");
    LocalContrastOptions structure;
    structure.amount = 0;
    structure.radius = options.radius;
    structure.fineRadius = 3;
    structure.protectStructure = true;
    return protectedStructure(image, structure, options);
}

LocalContrastResult LocalContrast::apply(const ImageBuffer& image, const LocalContrastOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return localContrastError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (!std::isfinite(options.amount) || !std::isfinite(options.threshold) || options.amount < 0.0F ||
        options.amount > 2.0F || options.radius == 0 || options.radius > 64 || options.threshold < 0.0F) {
        return localContrastError("ArgumentInvalid", "Local contrast options are outside valid ranges");
    }
    if (detail::imageHasInvalidCoveredColor(image)) {
        return localContrastError("ImageBufferInvalid", "Covered image colors and alpha must be finite");
    }

    if (!std::isfinite(options.starChroma) || options.starChroma < 0 || options.starChroma > 1 ||
        (!options.protectStructure && options.starChroma != 1))
        return localContrastError("ArgumentInvalid",
                                  "star-chroma must be in [0,1] and requires protected structure mode");

    if (!options.continuumCurvePoints.empty()) {
        if (!options.protectStructure || options.continuumCurvePoints.size() < 2)
            return localContrastError("ArgumentInvalid", "Continuum curve requires protected structure and at least two points");
        auto points = options.continuumCurvePoints;
        for (const auto& point : points)
            if (!std::isfinite(point.input) || !std::isfinite(point.output) || point.input < 0 || point.input > 1 ||
                point.output < 0 || point.output > 1)
                return localContrastError("ArgumentInvalid", "Continuum curve points must be finite and in [0,1]");
        std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.input < b.input; });
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& point = points[i];
            if (i && (point.input - points[i - 1].input < 1e-6F || point.output < points[i - 1].output))
                return localContrastError("ArgumentInvalid", "Continuum curve must have finite, unique inputs and nondecreasing outputs in [0,1]");
        }
        if (points.front().input != 0 || points.front().output != 0 || points.back().input != 1 || points.back().output != 1)
            return localContrastError("ArgumentInvalid", "Continuum curve endpoints must be 0:0 and 1:1");
    }

    if (options.protectStructure)
        return protectedStructure(image, options);

    const auto blurred = gaussianBlur(image, options.radius);
    ImageBuffer output = image;
    const auto colorChannels = detail::colorChannelCount(image);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        if (!detail::pixelHasValidColor(image, pixel)) {
            detail::clearMaskedPixel(output, pixel);
            continue;
        }
        for (std::uint16_t c = 0; c < colorChannels; ++c) {
            const double detailValue = static_cast<double>(image.pixels[offset + c]) - blurred.pixels[offset + c];
            const double adjusted = std::fabs(detailValue) >= options.threshold
                                        ? static_cast<double>(image.pixels[offset + c]) + detailValue * options.amount
                                        : image.pixels[offset + c];
            if (!options.clampOutput &&
                (!std::isfinite(adjusted) || std::fabs(adjusted) > std::numeric_limits<float>::max())) {
                return localContrastError("ImageValueInvalid", "Local contrast output exceeds Float32 range");
            }
            output.pixels[offset + c] =
                options.clampOutput ? static_cast<float>(std::clamp(adjusted, 0.0, 1.0)) : static_cast<float>(adjusted);
        }
    }

    LocalContrastResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

} // namespace photonstack
