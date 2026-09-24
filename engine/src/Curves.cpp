#include "photonstack/Curves.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

constexpr std::size_t kCurveLutResolution = 4096;

CurvesResult curvesError(std::string code, std::string message) {
    CurvesResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool validCurveChannel(CurveChannel channel) {
    switch (channel) {
    case CurveChannel::RGB:
    case CurveChannel::Red:
    case CurveChannel::Green:
    case CurveChannel::Blue:
    case CurveChannel::Luminance:
        return true;
    }
    return false;
}

struct PreparedCurve {
    std::vector<CurvePoint> points;
    std::vector<float> tangents;
    std::array<float, kCurveLutResolution + 1> lookup = {};
};

template <typename Callback> void parallelForPixels(const ImageBuffer& image, Callback callback) {
    const std::size_t pixels = image.pixelCount();
    if (pixels < 262144) {
        callback(0, pixels);
        return;
    }

    // Curve application streams several float planes and becomes memory-bandwidth
    // bound well before every performance core is occupied. On Apple silicon the
    // measured sweet spot is eight workers; additional threads increase contention.
    const unsigned int workerCount = std::min(
        std::max(1U, std::thread::hardware_concurrency()),
        8U
    );
    const std::size_t blockSize = (pixels + workerCount - 1) / workerCount;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned int worker = 0; worker < workerCount; ++worker) {
        const std::size_t begin = std::min<std::size_t>(pixels, static_cast<std::size_t>(worker) * blockSize);
        const std::size_t end = std::min<std::size_t>(pixels, begin + blockSize);
        if (begin >= end) {
            break;
        }
        workers.emplace_back([=, &callback]() {
            callback(begin, end);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

float limitedTangent(float leftSlope, float rightSlope) {
    if (leftSlope * rightSlope <= 0.0F) {
        return 0.0F;
    }
    const float sign = leftSlope < 0.0F ? -1.0F : 1.0F;
    const float average = 0.5F * (leftSlope + rightSlope);
    const float limit = 3.0F * std::min(std::fabs(leftSlope), std::fabs(rightSlope));
    return sign * std::min(std::fabs(average), limit);
}

std::vector<float> curveTangents(const std::vector<CurvePoint>& points) {
    std::vector<float> tangents(points.size(), 0.0F);
    if (points.size() < 2) {
        return tangents;
    }

    std::vector<float> slopes(points.size() - 1, 0.0F);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const float span = std::max(1.0e-6F, points[i + 1].input - points[i].input);
        slopes[i] = (points[i + 1].output - points[i].output) / span;
    }

    tangents.front() = slopes.front();
    tangents.back() = slopes.back();
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        tangents[i] = limitedTangent(slopes[i - 1], slopes[i]);
    }
    return tangents;
}

float hermiteInterpolate(float input, const CurvePoint& start, const CurvePoint& end, float startTangent, float endTangent) {
    const float span = std::max(1.0e-6F, end.input - start.input);
    const float t = std::clamp((input - start.input) / span, 0.0F, 1.0F);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0F * t3 - 3.0F * t2 + 1.0F;
    const float h10 = t3 - 2.0F * t2 + t;
    const float h01 = -2.0F * t3 + 3.0F * t2;
    const float h11 = t3 - t2;
    return h00 * start.output + h10 * span * startTangent + h01 * end.output + h11 * span * endTangent;
}

float interpolateCurve(float value, const PreparedCurve& curve) {
    const auto& points = curve.points;
    const float x = std::clamp(value, 0.0F, 1.0F);
    if (x <= points.front().input) {
        return std::clamp(points.front().output, 0.0F, 1.0F);
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (x <= points[i].input) {
            return std::clamp(
                hermiteInterpolate(x, points[i - 1], points[i], curve.tangents[i - 1], curve.tangents[i]),
                0.0F,
                1.0F);
        }
    }
    return std::clamp(points.back().output, 0.0F, 1.0F);
}

float sampleCurve(float value, const PreparedCurve& curve) {
    const float x = std::clamp(value, 0.0F, 1.0F);
    const float scaled = x * static_cast<float>(kCurveLutResolution);
    const std::size_t lowerIndex = static_cast<std::size_t>(scaled);
    const std::size_t upperIndex = std::min(lowerIndex + 1, kCurveLutResolution);
    const float fraction = scaled - static_cast<float>(lowerIndex);
    const float lower = curve.lookup[lowerIndex];
    const float upper = curve.lookup[upperIndex];
    return lower + (upper - lower) * fraction;
}

std::array<float, kCurveLutResolution + 1> buildCurveLookup(const PreparedCurve& curve) {
    std::array<float, kCurveLutResolution + 1> lookup = {};
    for (std::size_t index = 0; index <= kCurveLutResolution; ++index) {
        const float x = static_cast<float>(index) / static_cast<float>(kCurveLutResolution);
        lookup[index] = interpolateCurve(x, curve);
    }
    return lookup;
}

void applyLuminanceCurve(const ImageBuffer& image, ImageBuffer& output, std::size_t offset, const PreparedCurve& curve, bool preserveGamut) {
    if (image.channels < 3) {
        output.pixels[offset] = sampleCurve(image.pixels[offset], curve);
        return;
    }

    const float red = image.pixels[offset];
    const float green = image.pixels[offset + 1];
    const float blue = image.pixels[offset + 2];
    const float luminance = std::clamp(0.2126F * red + 0.7152F * green + 0.0722F * blue, 0.0F, 1.0F);
    const float mapped = sampleCurve(luminance, curve);
    if (preserveGamut) {
        const double center = .2126 * red + .7152 * green + .0722 * blue;
        double scale = 1;
        if (center > 0)
            scale = std::min(scale, mapped / center);
        if (center < 1)
            scale = std::min(scale, (1 - static_cast<double>(mapped)) / (1 - center));
        for (int channel = 0; channel < 3; ++channel)
            output.pixels[offset + channel] = static_cast<float>(
                mapped + scale * (image.pixels[offset + channel] - center));
        return;
    }
    const float ratio = luminance <= 1.0e-6F ? mapped : mapped / luminance;
    output.pixels[offset] = std::clamp(red * ratio, 0.0F, 1.0F);
    output.pixels[offset + 1] = std::clamp(green * ratio, 0.0F, 1.0F);
    output.pixels[offset + 2] = std::clamp(blue * ratio, 0.0F, 1.0F);
}

void applyRGBToneCurve(const ImageBuffer& image, ImageBuffer& output, std::size_t offset, const PreparedCurve& curve) {
    for (std::uint16_t channel = 0; channel < detail::colorChannelCount(image); ++channel) {
        output.pixels[offset + channel] = sampleCurve(image.pixels[offset + channel], curve);
    }
}

} // namespace

CurvesResult Curves::apply(const ImageBuffer& image, const CurvesOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return curvesError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (options.points.size() < 2) {
        return curvesError("ArgumentInvalid", "Curve requires at least two points");
    }
    if (!validCurveChannel(options.channel)) {
        return curvesError("ArgumentInvalid", "Unsupported curve channel");
    }
    if (options.preserveAlpha) {
        if (detail::imageHasInvalidCoveredColor(image)) {
            return curvesError("ImageBufferInvalid", "Input image contains non-finite visible color samples");
        }
    } else if (std::any_of(image.pixels.begin(), image.pixels.end(), [](float sample) {
                   return !std::isfinite(sample);
               })) {
        return curvesError("ImageBufferInvalid", "Input image contains non-finite samples");
    }

    if (options.preserveLuminanceGamut) {
        if (options.channel != CurveChannel::Luminance)
            return curvesError("ArgumentInvalid", "Gamut preservation requires the luminance channel");
        for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
            if (!detail::pixelHasValidColor(image, pixel))
                continue;
            for (std::uint16_t channel = 0; channel < detail::colorChannelCount(image); ++channel) {
                const float value = image.pixels[pixel * image.channels + channel];
                if (value < 0 || value > 1)
                    return curvesError("ImageValueInvalid", "Gamut-preserving curves require samples in [0,1]");
            }
        }
    }

    auto points = options.points;
    std::sort(points.begin(), points.end(),
              [](const CurvePoint& left, const CurvePoint& right) { return left.input < right.input; });
    for (const auto& point : points) {
        if (!std::isfinite(point.input) || !std::isfinite(point.output) || point.input < 0.0F ||
            point.input > 1.0F || point.output < 0.0F || point.output > 1.0F) {
            return curvesError("ArgumentInvalid", "Curve points must be in the 0..1 range");
        }
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (std::fabs(points[i].input - points[i - 1].input) < 1.0e-6F) {
            return curvesError("ArgumentInvalid", "Curve input points must be unique");
        }
    }
    PreparedCurve curve{.points = points, .tangents = curveTangents(points)};
    curve.lookup = buildCurveLookup(curve);

    ImageBuffer output = image;
    parallelForPixels(image, [&](std::size_t begin, std::size_t end) {
        for (std::size_t pixel = begin; pixel < end; ++pixel) {
            const auto offset = pixel * image.channels;
            bool validPixel = detail::pixelHasValidColor(image, pixel);
            if (!options.preserveAlpha && image.channels == 4) {
                validPixel = true;
                for (std::uint16_t channel = 0; channel < 4; ++channel) {
                    validPixel = validPixel && std::isfinite(image.pixels[offset + channel]);
                }
            }
            if (!validPixel) {
                detail::clearMaskedPixel(output, pixel);
                continue;
            }
            switch (options.channel) {
            case CurveChannel::RGB:
                applyRGBToneCurve(image, output, offset, curve);
                break;
            case CurveChannel::Red:
                output.pixels[offset] = sampleCurve(image.pixels[offset], curve);
                break;
            case CurveChannel::Green:
                if (image.channels >= 2) {
                    output.pixels[offset + 1] = sampleCurve(image.pixels[offset + 1], curve);
                } else {
                    output.pixels[offset] = sampleCurve(image.pixels[offset], curve);
                }
                break;
            case CurveChannel::Blue:
                if (image.channels >= 3) {
                    output.pixels[offset + 2] = sampleCurve(image.pixels[offset + 2], curve);
                } else {
                    output.pixels[offset] = sampleCurve(image.pixels[offset], curve);
                }
                break;
            case CurveChannel::Luminance:
                applyLuminanceCurve(image, output, offset, curve, options.preserveLuminanceGamut);
                break;
            }
            if (!options.preserveAlpha && image.channels >= 4) {
                output.pixels[offset + 3] = sampleCurve(image.pixels[offset + 3], curve);
            }
        }
    });

    CurvesResult result;
    result.ok = true;
    result.image = std::move(output);
    return result;
}

} // namespace photonstack
