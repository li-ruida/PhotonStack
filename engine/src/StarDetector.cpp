#include "photonstack/StarDetector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

StarDetectionResult detectionError(std::string code, std::string message) {
    StarDetectionResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

float luminanceAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
    if (image.channels < 3) {
        return image.pixels[offset];
    }
    return image.pixels[offset] * 0.2126F + image.pixels[offset + 1] * 0.7152F + image.pixels[offset + 2] * 0.0722F;
}

StarDetectionResult detectLuminanceBuffer(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const float> luminance,
    const StarDetectionOptions& options,
    std::span<const float> coverage = {}
) {
    if (width == 0 || height == 0 || luminance.size() != static_cast<std::size_t>(width) * height) {
        return detectionError("ImageBufferInvalid", "Input luminance must match the image dimensions");
    }
    if (!coverage.empty() && coverage.size() != luminance.size()) {
        return detectionError("ImageBufferInvalid", "Input coverage must match the image dimensions");
    }
    if (std::any_of(coverage.begin(), coverage.end(), [](float value) { return !std::isfinite(value); })) {
        return detectionError("ImageBufferInvalid", "Input coverage must be finite");
    }
    const auto requiredSize = static_cast<std::uint64_t>(options.border) * 2U + 1U;
    if (width < requiredSize || height < requiredSize) {
        return detectionError("ImageTooSmall", "Input image is too small for star detection");
    }
    if (!std::isfinite(options.sigmaThreshold) || !std::isfinite(options.minPeak) ||
        options.sigmaThreshold <= 0.0F) {
        return detectionError("ArgumentInvalid", "Star detection thresholds must be finite and sigmaThreshold positive");
    }
    if (options.maxStars == 0) {
        return detectionError("ArgumentInvalid", "maxStars must be greater than zero");
    }

    const auto coverageAt = [&](std::size_t index) {
        return coverage.empty() ? 1.0F : coverage[index];
    };
    const auto isValid = [&](std::size_t index) {
        return std::isfinite(luminance[index]) && std::isfinite(coverageAt(index)) &&
               coverageAt(index) > 1.0e-6F;
    };
    double sum = 0.0;
    double totalCoverage = 0.0;
    for (std::size_t index = 0; index < luminance.size(); ++index) {
        if (!isValid(index)) {
            continue;
        }
        const double weight = std::clamp(static_cast<double>(coverageAt(index)), 0.0, 1.0);
        sum += static_cast<double>(luminance[index]) * weight;
        totalCoverage += weight;
    }
    if (!(totalCoverage > 1.0e-6) || !std::isfinite(totalCoverage)) {
        return detectionError("ImageCoverageEmpty", "Star detection requires at least one valid pixel");
    }
    const double mean = sum / totalCoverage;
    double variance = 0.0;
    for (std::size_t index = 0; index < luminance.size(); ++index) {
        if (!isValid(index)) {
            continue;
        }
        const double delta = static_cast<double>(luminance[index]) - mean;
        variance += delta * delta * std::clamp(static_cast<double>(coverageAt(index)), 0.0, 1.0);
    }
    variance /= totalCoverage;
    const double standardDeviation = std::sqrt(std::max(0.0, variance));
    const double threshold = std::max(static_cast<double>(options.minPeak),
                                      mean + static_cast<double>(options.sigmaThreshold) * standardDeviation);

    const auto effectiveLuminance = [&](std::size_t index) {
        const double weight = std::clamp(static_cast<double>(coverageAt(index)), 0.0, 1.0);
        return mean + (static_cast<double>(luminance[index]) - mean) * weight;
    };

    struct RankedStar {
        Star star;
        double flux = 0.0;
    };
    std::vector<RankedStar> stars;
    for (std::uint32_t y = options.border; y < height - options.border; ++y) {
        for (std::uint32_t x = options.border; x < width - options.border; ++x) {
            const auto centerIndex = static_cast<std::size_t>(y) * width + x;
            if (!isValid(centerIndex)) {
                continue;
            }
            const double center = effectiveLuminance(centerIndex);
            if (center < threshold) {
                continue;
            }

            bool isLocalMaximum = true;
            for (int dy = -1; dy <= 1 && isLocalMaximum; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    const auto neighborX = static_cast<std::uint32_t>(static_cast<int>(x) + dx);
                    const auto neighborY = static_cast<std::uint32_t>(static_cast<int>(y) + dy);
                    const auto neighborIndex = static_cast<std::size_t>(neighborY) * width + neighborX;
                    if (!isValid(neighborIndex)) {
                        continue;
                    }
                    const double neighbor = effectiveLuminance(neighborIndex);
                    const bool equalEarlierPixel = neighbor == center && (neighborY < y || (neighborY == y && neighborX < x));
                    if (neighbor > center || equalEarlierPixel) {
                        isLocalMaximum = false;
                        break;
                    }
                }
            }
            if (!isLocalMaximum) {
                continue;
            }

            double weightedX = 0.0;
            double weightedY = 0.0;
            double flux = 0.0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto sampleX = static_cast<std::uint32_t>(static_cast<int>(x) + dx);
                    const auto sampleY = static_cast<std::uint32_t>(static_cast<int>(y) + dy);
                    const auto sampleIndex = static_cast<std::size_t>(sampleY) * width + sampleX;
                    if (!isValid(sampleIndex)) {
                        continue;
                    }
                    const double sample = luminance[sampleIndex];
                    const double signal = std::max(0.0, sample - mean) *
                                          std::clamp(static_cast<double>(coverageAt(sampleIndex)), 0.0, 1.0);
                    weightedX += static_cast<double>(sampleX) * signal;
                    weightedY += static_cast<double>(sampleY) * signal;
                    flux += signal;
                }
            }

            if (!(flux > 0.0) || !std::isfinite(flux)) {
                continue;
            }

            const double centroidX = weightedX / flux;
            const double centroidY = weightedY / flux;
            double momentXX = 0.0;
            double momentYY = 0.0;
            double momentXY = 0.0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto sampleX = static_cast<std::uint32_t>(static_cast<int>(x) + dx);
                    const auto sampleY = static_cast<std::uint32_t>(static_cast<int>(y) + dy);
                    const auto sampleIndex = static_cast<std::size_t>(sampleY) * width + sampleX;
                    if (!isValid(sampleIndex)) {
                        continue;
                    }
                    const double sample = luminance[sampleIndex];
                    const double signal = std::max(0.0, sample - mean) *
                                          std::clamp(static_cast<double>(coverageAt(sampleIndex)), 0.0, 1.0);
                    const double offsetX = static_cast<double>(sampleX) - centroidX;
                    const double offsetY = static_cast<double>(sampleY) - centroidY;
                    momentXX += offsetX * offsetX * signal;
                    momentYY += offsetY * offsetY * signal;
                    momentXY += offsetX * offsetY * signal;
                }
            }
            momentXX /= flux;
            momentYY /= flux;
            momentXY /= flux;
            const double trace = momentXX + momentYY;
            const double determinant = std::max(0.0, momentXX * momentYY - momentXY * momentXY);
            const double discriminant = std::max(0.0, trace * trace * 0.25 - determinant);
            const double majorVariance = std::max(0.0, trace * 0.5 + std::sqrt(discriminant));
            const double minorVariance = std::max(0.0, trace * 0.5 - std::sqrt(discriminant));
            const double sigmaMajor = std::sqrt(majorVariance);
            const double sigmaMinor = std::sqrt(minorVariance);
            const double fwhm = 2.35482 * sigmaMajor;
            const double eccentricity =
                sigmaMajor <= 0.0
                    ? 0.0
                    : std::sqrt(std::max(0.0, 1.0 - (sigmaMinor * sigmaMinor) / (sigmaMajor * sigmaMajor)));

            stars.push_back({
                .star = {
                    .x = static_cast<float>(centroidX),
                    .y = static_cast<float>(centroidY),
                    .flux = static_cast<float>(std::min(flux, static_cast<double>(std::numeric_limits<float>::max()))),
                    .peak = luminance[centerIndex],
                    .fwhm = static_cast<float>(fwhm),
                    .eccentricity = static_cast<float>(eccentricity),
                },
                .flux = flux,
            });
        }
    }

    std::sort(stars.begin(), stars.end(), [](const RankedStar& left, const RankedStar& right) {
        return left.flux > right.flux;
    });
    std::vector<Star> filteredStars;
    filteredStars.reserve(std::min(stars.size(), options.maxStars));
    constexpr float minSeparationSquared = 3.0F * 3.0F;
    for (const auto& ranked : stars) {
        const auto& star = ranked.star;
        const bool tooClose = std::any_of(filteredStars.begin(), filteredStars.end(), [&](const Star& selected) {
            const float dx = selected.x - star.x;
            const float dy = selected.y - star.y;
            return dx * dx + dy * dy < minSeparationSquared;
        });
        if (tooClose) {
            continue;
        }
        filteredStars.push_back(star);
        if (filteredStars.size() >= options.maxStars) {
            break;
        }
    }

    StarDetectionResult result;
    result.ok = true;
    result.stars = std::move(filteredStars);
    return result;
}

} // namespace

StarDetectionResult StarDetector::detect(const ImageBuffer& image, const StarDetectionOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return detectionError("ImageBufferInvalid", "Input image must be a non-empty float buffer");
    }
    const auto requiredSize = static_cast<std::uint64_t>(options.border) * 2U + 1U;
    if (image.width < requiredSize || image.height < requiredSize) {
        return detectionError("ImageTooSmall", "Input image is too small for star detection");
    }
    if (!std::isfinite(options.sigmaThreshold) || !std::isfinite(options.minPeak) ||
        options.sigmaThreshold <= 0.0F) {
        return detectionError("ArgumentInvalid", "Star detection thresholds must be finite and sigmaThreshold positive");
    }
    if (options.maxStars == 0) {
        return detectionError("ArgumentInvalid", "maxStars must be greater than zero");
    }
    std::vector<float> luminance(image.pixelCount(), 0.0F);
    std::vector<float> coverage(image.pixelCount(), 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            const auto offset = pixel * image.channels;
            float pixelCoverage = 1.0F;
            if (image.channels == 4) {
                const float alpha = image.pixels[offset + 3];
                if (!std::isfinite(alpha)) {
                    return detectionError("ImageBufferInvalid", "Input image alpha must be finite");
                }
                pixelCoverage = std::clamp(alpha, 0.0F, 1.0F);
            }
            if (pixelCoverage <= 1.0e-6F) {
                continue;
            }
            for (std::uint16_t channel = 0; channel < detail::colorChannelCount(image); ++channel) {
                if (!std::isfinite(image.pixels[offset + channel])) {
                    return detectionError("ImageBufferInvalid", "Covered image colors must be finite");
                }
            }
            const float value = luminanceAt(image, x, y);
            if (!std::isfinite(value)) {
                return detectionError("ImageBufferInvalid", "Covered image luminance must be finite");
            }
            luminance[pixel] = value;
            coverage[pixel] = pixelCoverage;
        }
    }

    return detectLuminanceBuffer(image.width, image.height, luminance, options, coverage);
}

StarDetectionResult StarDetector::detectLuminance(std::uint32_t width, std::uint32_t height,
                                                  std::span<const float> luminance,
                                                  const StarDetectionOptions& options) const {
    return detectLuminanceBuffer(width, height, luminance, options);
}

StarDetectionResult StarDetector::detectLuminanceWithCoverage(std::uint32_t width, std::uint32_t height,
                                                              std::span<const float> luminance,
                                                              std::span<const float> coverage,
                                                              const StarDetectionOptions& options) const {
    return detectLuminanceBuffer(width, height, luminance, options, coverage);
}

} // namespace photonstack
