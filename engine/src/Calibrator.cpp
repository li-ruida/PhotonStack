#include "photonstack/Calibrator.hpp"

#include "photonstack/StageProfile.hpp"
#ifdef __APPLE__
#include "MetalCompute.hpp"
#endif
#include "photonstack/ParallelRanges.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {

CalibrationResult calibrationError(std::string code, std::string message) {
    CalibrationResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool sameShape(const ImageBuffer& left, const ImageBuffer& right) {
    return left.width == right.width && left.height == right.height && left.channels == right.channels &&
           left.pixels.size() == right.pixels.size();
}

bool hasAlpha(const ImageBuffer& image) {
    return image.channels == 4;
}

bool invalidCalibrationSamples(const std::array<const ImageBuffer*, 4>& images, std::size_t count) {
    std::atomic<bool> invalid{false};
    detail::parallelRanges(count, 32768, [&](std::size_t first, std::size_t last) noexcept {
        if (invalid.load(std::memory_order_relaxed)) return;
        for (const auto* image : images) {
            if (!image) continue;
            for (auto pixel = first; pixel < last; ++pixel) {
                const auto offset = pixel * image->channels;
                if ((hasAlpha(*image) && !std::isfinite(image->pixels[offset + 3])) ||
                    (detail::pixelCoverage(*image, pixel) > 1.e-6F && !detail::pixelHasValidColor(*image, pixel))) {
                    invalid.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        }
    });
    return invalid.load(std::memory_order_relaxed);
}

std::optional<double> positiveChannelMean(
    const ImageBuffer& image,
    const ImageBuffer* bias,
    std::uint16_t channel
) {
    double weightedSum = 0.0;
    double totalCoverage = 0.0;
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto sample = pixel * image.channels + channel;
        const double value = static_cast<double>(image.pixels[sample]) -
                             (bias == nullptr ? 0.0 : static_cast<double>(bias->pixels[sample]));
        float coverage = detail::pixelCoverage(image, pixel);
        if (bias != nullptr) {
            coverage = std::min(coverage, detail::pixelCoverage(*bias, pixel));
        }
        if (coverage <= 1.0e-6F || value <= 0.0F) {
            continue;
        }
        weightedSum += value * coverage;
        totalCoverage += coverage;
    }
    if (totalCoverage <= 0.0) {
        return std::nullopt;
    }
    return weightedSum / totalCoverage;
}

bool isAlphaChannel(const ImageBuffer& image, std::uint16_t channel) {
    return hasAlpha(image) && channel == 3;
}

} // namespace

const char* calibrationBiasStateName(CalibrationBiasState state) {
    switch (state) {
        case CalibrationBiasState::Unknown:
            return "unknown";
        case CalibrationBiasState::Included:
            return "included";
        case CalibrationBiasState::Removed:
            return "removed";
    }
    return "unknown";
}

CalibrationMasterValidation Calibrator::validateMasters(const CalibrationOptions& options) {
    CalibrationMasterValidation result;
    result.masters_ = {options.dark, options.bias, options.flat};
    result.valid_ = std::all_of(result.masters_.begin(), result.masters_.end(), [](const auto* image) {
        return !image || (!image->empty() && (image->channels == 1 || image->channels == 3 || image->channels == 4) &&
            image->pixels.size() == image->sampleCount() &&
            !invalidCalibrationSamples({image, nullptr, nullptr, nullptr}, image->pixelCount()));
    });
    return result;
}

CalibrationFlatMeans Calibrator::measureFlat(const CalibrationOptions& options) {
    CalibrationFlatMeans result;
    result.flat = options.flat;
    result.bias = options.flatBiasState == CalibrationBiasState::Included ? options.bias : nullptr;
    if (!result.flat || result.flat->empty() || result.flat->pixels.size() != result.flat->sampleCount() ||
        (result.bias && !sameShape(*result.flat, *result.bias))) return result;
    result.channels.assign(result.flat->channels, 1.);
    for (std::uint16_t c = 0; c < result.flat->channels; ++c) {
        if (isAlphaChannel(*result.flat, c)) continue;
        const auto mean = positiveChannelMean(*result.flat, result.bias, c);
        if (!mean || !std::isfinite(*mean) || *mean <= 0) { result.channels.clear(); break; }
        result.channels[c] = *mean;
    }
    return result;
}

CalibrationResult Calibrator::calibrate(const ImageBuffer& light, const CalibrationOptions& options) const {
    const detail::StageScope timer(detail::Stage::Calibration);
    if (light.empty() || (light.channels != 1 && light.channels != 3 && light.channels != 4) ||
        light.pixels.size() != light.sampleCount()) {
        return calibrationError("ImageBufferInvalid", "Light frame must be a non-empty float image");
    }

    if (options.dark != nullptr && !sameShape(light, *options.dark)) {
        return calibrationError("CalibrationShapeMismatch", "Dark frame dimensions must match the light frame");
    }
    if (options.bias != nullptr && !sameShape(light, *options.bias)) {
        return calibrationError("CalibrationShapeMismatch", "Bias frame dimensions must match the light frame");
    }
    if (options.flat != nullptr && !sameShape(light, *options.flat)) {
        return calibrationError("CalibrationShapeMismatch", "Flat frame dimensions must match the light frame");
    }
    if ((options.dark != nullptr && light.colorEncoding != options.dark->colorEncoding) ||
        (options.bias != nullptr && light.colorEncoding != options.bias->colorEncoding) ||
        (options.flat != nullptr && light.colorEncoding != options.flat->colorEncoding)) {
        return calibrationError("ImageColorEncodingMismatch",
                                "Calibration frames must use the same color encoding as the light frame");
    }
    if (light.colorEncoding != ColorEncoding::Linear) {
        return calibrationError(
            "ImageColorEncodingMismatch",
            "Calibration requires linear light, dark, bias, and flat frames"
        );
    }
    const auto* validation = options.masterValidation;
    const bool known = validation && validation->masters_ ==
        std::array<const ImageBuffer*, 3>{options.dark, options.bias, options.flat};
    if ((known && !validation->valid_) || invalidCalibrationSamples(
            {&light, known ? nullptr : options.dark, known ? nullptr : options.bias, known ? nullptr : options.flat},
            light.pixelCount())) {
        return calibrationError("ImageBufferInvalid", "Covered calibration frame samples must be finite");
    }
    if (!std::isfinite(options.flatEpsilon) || options.flatEpsilon <= 0.0F || options.flatEpsilon >= 1.0F) {
        return calibrationError("ArgumentInvalid", "flatEpsilon must be greater than zero and less than one");
    }
    const auto validBiasState = [](CalibrationBiasState state) {
        return state == CalibrationBiasState::Unknown || state == CalibrationBiasState::Included ||
               state == CalibrationBiasState::Removed;
    };
    if (!validBiasState(options.darkBiasState) || !validBiasState(options.flatBiasState)) {
        return calibrationError("ArgumentInvalid", "Calibration bias state is invalid");
    }
    if (options.dark != nullptr && options.bias != nullptr &&
        options.darkBiasState == CalibrationBiasState::Unknown) {
        return calibrationError(
            "CalibrationStateUnknown",
            "Specify whether the dark frame includes bias before using dark and bias together"
        );
    }
    if (options.flat != nullptr && options.bias != nullptr &&
        options.flatBiasState == CalibrationBiasState::Unknown) {
        return calibrationError(
            "CalibrationStateUnknown",
            "Specify whether the flat frame includes bias before using flat and bias together"
        );
    }
    if (options.flat != nullptr && options.bias == nullptr &&
        options.flatBiasState == CalibrationBiasState::Included) {
        return calibrationError(
            "CalibrationBiasMissing",
            "A flat frame marked as including bias requires a bias frame"
        );
    }

    const bool subtractBiasFromLight = options.bias != nullptr &&
                                       (options.dark == nullptr ||
                                        options.darkBiasState == CalibrationBiasState::Removed);
    const bool subtractBiasFromFlat = options.flat != nullptr && options.bias != nullptr &&
                                      options.flatBiasState == CalibrationBiasState::Included;

    ImageBuffer output{light.width, light.height, light.channels, light.format, light.colorEncoding,
                       light.sourceBitsPerChannel, {}};
    std::vector<double> flatMeans(light.channels, 1.);
    if (options.flat) {
        const auto* known = options.flatMeans;
        if (known && known->flat == options.flat && known->bias == (subtractBiasFromFlat ? options.bias : nullptr))
            flatMeans = known->channels;
        else
            flatMeans = measureFlat(options).channels;
        if (flatMeans.size() != light.channels)
            return calibrationError("CalibrationFlatInvalid", "Flat frame has no valid positive samples");
    }

#ifdef __APPLE__
    if (detail::currentStage.preferMetal && light.channels == 4 && light.pixelCount() >= 65536 &&
        light.pixelCount() <= UINT32_MAX &&
        detail::metalCalibrate(light, options, subtractBiasFromLight, subtractBiasFromFlat, flatMeans, output))
        return {true, std::move(output), subtractBiasFromLight, subtractBiasFromFlat, {}, {}};
#endif
    // Metal lends its destination pages directly to the result. Allocate CPU
    // storage only on fallback, avoiding two live full-size output planes.
    output.pixels.resize(light.sampleCount());
    std::atomic<bool> overflow{false};
    detail::parallelRanges(light.pixelCount(), 32768, [&](std::size_t begin, std::size_t end) noexcept {
        std::array<double, 4> calibrated{};
        for (auto pixel = begin; pixel < end; ++pixel) {
            const auto offset = pixel * light.channels;
            float coverage = detail::pixelCoverage(light, pixel);
            if (options.dark != nullptr) {
                coverage = std::min(coverage, detail::pixelCoverage(*options.dark, pixel));
            }
            if (subtractBiasFromLight || subtractBiasFromFlat) {
                coverage = std::min(coverage, detail::pixelCoverage(*options.bias, pixel));
            }
            if (options.flat != nullptr) {
                coverage = std::min(coverage, detail::pixelCoverage(*options.flat, pixel));
            }
            bool validPixel = coverage > 1.0e-6F;
            std::fill(calibrated.begin(), calibrated.end(), 0.0F);
            for (std::uint16_t channel = 0; channel < light.channels && validPixel; ++channel) {
                if (isAlphaChannel(light, channel)) {
                    continue;
                }
                const auto sample = offset + channel;
                double value = light.pixels[sample];
                if (options.dark != nullptr) {
                    value -= static_cast<double>(options.dark->pixels[sample]);
                }
                if (subtractBiasFromLight) {
                    value -= static_cast<double>(options.bias->pixels[sample]);
                }
                if (options.flat != nullptr) {
                    const double flat = static_cast<double>(options.flat->pixels[sample]) -
                                        (subtractBiasFromFlat
                                             ? static_cast<double>(options.bias->pixels[sample])
                                             : 0.0);
                    const double normalizedFlat = flat / flatMeans[channel];
                    validPixel = validPixel && std::isfinite(normalizedFlat) && normalizedFlat > options.flatEpsilon;
                    if (validPixel) {
                        value /= normalizedFlat;
                    }
                }
                if (!validPixel) {
                    continue;
                }
                value = options.clampNegativeValues ? std::max(0.0, value) : value;
                constexpr double maximum = std::numeric_limits<float>::max();
                if (!std::isfinite(value) || value < -maximum || value > maximum) {
                    overflow.store(true, std::memory_order_relaxed);
                    return;
                }
                calibrated[channel] = value;
            }

            if (!validPixel) {
                for (std::uint16_t channel = 0; channel < light.channels; ++channel) {
                    output.pixels[offset + channel] = 0.0F;
                }
                continue;
            }
            for (std::uint16_t channel = 0; channel < light.channels; ++channel) {
                output.pixels[offset + channel] = isAlphaChannel(light, channel)
                                                      ? coverage
                                                      : static_cast<float>(calibrated[channel]);
            }
        }

    });
    if (overflow.load(std::memory_order_relaxed))
        return calibrationError("ImageValueInvalid", "Calibrated sample exceeds the Float32 scientific value range");

    CalibrationResult result;
    result.ok = true;
    result.image = std::move(output);
    result.subtractedBiasFromLight = subtractBiasFromLight;
    result.subtractedBiasFromFlat = subtractBiasFromFlat;
    return result;
}

} // namespace photonstack
