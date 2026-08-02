#include "photonstack/FrameNormalizer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace photonstack {
namespace {

FrameNormalizationResult normalizationError(std::string code, std::string message) {
    FrameNormalizationResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

float luminanceAt(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    if (image.channels >= 3) {
        return image.pixels[offset] * 0.2126F + image.pixels[offset + 1] * 0.7152F + image.pixels[offset + 2] * 0.0722F;
    }
    return image.pixels[offset];
}

bool pixelHasCoverage(const ImageBuffer& image, std::size_t pixel) {
    if (image.channels < 4) {
        return true;
    }
    const float alpha = image.pixels[pixel * image.channels + 3];
    return std::isfinite(alpha) && alpha > 1.0e-6F;
}

bool pixelHasFiniteColor(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    const auto colorChannels = std::min<std::uint16_t>(3, image.channels);
    for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
        if (!std::isfinite(image.pixels[offset + channel])) {
            return false;
        }
    }
    return true;
}

bool pixelIsValid(const ImageBuffer& image, std::size_t pixel) {
    return pixelHasCoverage(image, pixel) && pixelHasFiniteColor(image, pixel);
}

float outputColor(float value, bool clampOutput) {
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    return clampOutput ? std::clamp(value, 0.0F, 1.0F) : value;
}

float median(std::vector<float>& values) {
    if (values.empty()) {
        return 0.0F;
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    if (values.size() % 2 == 1) {
        return *middle;
    }
    const auto lower = std::max_element(values.begin(), middle);
    return (*lower + *middle) * 0.5F;
}

float backgroundMedian(const ImageBuffer& image) {
    std::vector<float> values;
    values.reserve(image.pixelCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (pixelIsValid(image, pixel)) {
            const float value = luminanceAt(image, pixel);
            if (std::isfinite(value)) {
                values.push_back(value);
            }
        }
    }
    return values.empty() ? std::numeric_limits<float>::quiet_NaN() : median(values);
}

float robustScale(const ImageBuffer& image, float center) {
    std::vector<float> values;
    values.reserve(image.pixelCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (pixelIsValid(image, pixel)) {
            const float value = std::fabs(luminanceAt(image, pixel) - center);
            if (std::isfinite(value)) {
                values.push_back(value);
            }
        }
    }
    return median(values) * 1.4826F;
}

bool validNormalizationOptions(const FrameNormalizationOptions& options) {
    return std::isfinite(options.targetBackground) && options.targetBackground >= 0.0F &&
           options.targetBackground <= 1.0F && std::isfinite(options.targetScale) &&
           options.targetScale >= 0.0F && std::isfinite(options.epsilon) && options.epsilon > 0.0F;
}

struct PairedStatistics {
    bool ok = false;
    float inputBackground = 0.0F;
    float referenceBackground = 0.0F;
    float inputScale = 0.0F;
    float referenceScale = 0.0F;
};

PairedStatistics pairedStatistics(const ImageBuffer& image, const ImageBuffer& reference) {
    std::vector<float> inputValues;
    std::vector<float> referenceValues;
    inputValues.reserve(image.pixelCount());
    referenceValues.reserve(reference.pixelCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (!pixelIsValid(image, pixel) || !pixelIsValid(reference, pixel)) {
            continue;
        }
        const float inputValue = luminanceAt(image, pixel);
        const float referenceValue = luminanceAt(reference, pixel);
        if (!std::isfinite(inputValue) || !std::isfinite(referenceValue)) {
            continue;
        }
        inputValues.push_back(inputValue);
        referenceValues.push_back(referenceValue);
    }
    if (inputValues.empty()) {
        return {};
    }

    PairedStatistics result;
    result.inputBackground = median(inputValues);
    result.referenceBackground = median(referenceValues);
    for (auto& value : inputValues) {
        value = std::fabs(value - result.inputBackground);
    }
    for (auto& value : referenceValues) {
        value = std::fabs(value - result.referenceBackground);
    }
    result.inputScale = median(inputValues) * 1.4826F;
    result.referenceScale = median(referenceValues) * 1.4826F;
    result.ok = std::isfinite(result.inputScale) && std::isfinite(result.referenceScale);
    return result;
}

struct PairedBackgroundGrids {
    std::vector<float> input;
    std::vector<float> reference;
};

PairedBackgroundGrids backgroundGridPair(const ImageBuffer& image, const ImageBuffer& reference,
                                         std::uint32_t columns, std::uint32_t rows,
                                         float inputFallback, float referenceFallback) {
    PairedBackgroundGrids grids;
    grids.input.assign(static_cast<std::size_t>(columns) * rows, 0.0F);
    grids.reference.assign(static_cast<std::size_t>(columns) * rows, 0.0F);
    std::vector<float> inputValues;
    std::vector<float> referenceValues;
    for (std::uint32_t gy = 0; gy < rows; ++gy) {
        const auto y0 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(gy) * image.height) /
                                                   static_cast<std::uint64_t>(rows));
        const auto y1 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(gy + 1) * image.height) /
                                                   static_cast<std::uint64_t>(rows));
        for (std::uint32_t gx = 0; gx < columns; ++gx) {
            const auto x0 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(gx) * image.width) /
                                                       static_cast<std::uint64_t>(columns));
            const auto x1 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(gx + 1) * image.width) /
                                                       static_cast<std::uint64_t>(columns));
            inputValues.clear();
            referenceValues.clear();
            for (std::uint32_t y = y0; y < y1; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const auto pixel = static_cast<std::size_t>(y) * image.width + x;
                    if (!pixelIsValid(image, pixel) || !pixelIsValid(reference, pixel)) {
                        continue;
                    }
                    const float inputValue = luminanceAt(image, pixel);
                    const float referenceValue = luminanceAt(reference, pixel);
                    if (std::isfinite(inputValue) && std::isfinite(referenceValue)) {
                        inputValues.push_back(inputValue);
                        referenceValues.push_back(referenceValue);
                    }
                }
            }
            if (inputValues.empty()) {
                inputValues.push_back(inputFallback);
                referenceValues.push_back(referenceFallback);
            }
            const auto index = static_cast<std::size_t>(gy) * columns + gx;
            grids.input[index] = median(inputValues);
            grids.reference[index] = median(referenceValues);
        }
    }
    return grids;
}

float sampleGrid(const std::vector<float>& grid, std::uint32_t columns, std::uint32_t rows, const ImageBuffer& image,
                 std::uint32_t x, std::uint32_t y) {
    const float fx =
        (static_cast<float>(x) + 0.5F) * static_cast<float>(columns) / static_cast<float>(image.width) - 0.5F;
    const float fy =
        (static_cast<float>(y) + 0.5F) * static_cast<float>(rows) / static_cast<float>(image.height) - 0.5F;
    const auto x0 = static_cast<std::uint32_t>(std::floor(std::clamp(fx, 0.0F, static_cast<float>(columns - 1))));
    const auto y0 = static_cast<std::uint32_t>(std::floor(std::clamp(fy, 0.0F, static_cast<float>(rows - 1))));
    const auto x1 = std::min(x0 + 1, columns - 1);
    const auto y1 = std::min(y0 + 1, rows - 1);
    const float tx = std::clamp(fx - static_cast<float>(x0), 0.0F, 1.0F);
    const float ty = std::clamp(fy - static_cast<float>(y0), 0.0F, 1.0F);
    const auto index = [&](std::uint32_t gx, std::uint32_t gy) { return static_cast<std::size_t>(gy) * columns + gx; };
    const float top = grid[index(x0, y0)] + (grid[index(x1, y0)] - grid[index(x0, y0)]) * tx;
    const float bottom = grid[index(x0, y1)] + (grid[index(x1, y1)] - grid[index(x0, y1)]) * tx;
    return top + (bottom - top) * ty;
}

FrameNormalizationResult applyNormalization(const ImageBuffer& image, float inputBackground, float inputScale,
                                            float targetBackground, float targetScale,
                                            const FrameNormalizationOptions& options) {
    const float scale =
        targetScale > 0.0F ? std::max(options.epsilon, targetScale) / std::max(options.epsilon, inputScale) : 1.0F;
    if (!std::isfinite(inputBackground) || !std::isfinite(inputScale) || !std::isfinite(scale)) {
        return normalizationError("ImageValueInvalid", "Frame normalization statistics must be finite");
    }

    ImageBuffer output = image;
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        if (!pixelIsValid(image, pixel)) {
            for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                output.pixels[offset + channel] = 0.0F;
            }
            continue;
        }
        for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
            const auto sample = offset + channel;
            if (image.channels >= 4 && channel == 3) {
                output.pixels[sample] = image.pixels[sample];
                continue;
            }
            const float normalized = (image.pixels[sample] - inputBackground) * scale + targetBackground;
            output.pixels[sample] = outputColor(normalized, options.clampOutput);
        }
    }

    FrameNormalizationResult result;
    result.ok = true;
    result.image = std::move(output);
    result.inputBackground = inputBackground;
    result.outputBackground = targetBackground;
    result.scale = scale;
    return result;
}

} // namespace

FrameNormalizationResult FrameNormalizer::normalize(const ImageBuffer& image,
                                                    const FrameNormalizationOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return normalizationError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (!validNormalizationOptions(options)) {
        return normalizationError("ArgumentInvalid", "Frame normalization options are outside valid ranges");
    }

    const float inputBackground = backgroundMedian(image);
    if (!std::isfinite(inputBackground)) {
        return normalizationError("ImageCoverageEmpty", "Frame normalization requires at least one valid pixel");
    }
    const float targetBackground = options.targetBackground > 0.0F ? options.targetBackground : inputBackground;
    return applyNormalization(
        image,
        inputBackground,
        robustScale(image, inputBackground),
        targetBackground,
        options.targetScale,
        options
    );
}

FrameNormalizationResult FrameNormalizer::matchReference(const ImageBuffer& image, const ImageBuffer& reference,
                                                         const FrameNormalizationOptions& options) const {
    if (image.empty() || reference.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount() ||
        reference.pixels.size() != reference.sampleCount()) {
        return normalizationError("ImageBufferInvalid", "Input and reference images must be non-empty float images");
    }
    if (image.channels != reference.channels) {
        return normalizationError("ImageDimensionsMismatch", "Input and reference channel counts must match");
    }
    if (image.colorEncoding != reference.colorEncoding) {
        return normalizationError("ImageColorEncodingMismatch",
                                  "Input and reference images must use the same color encoding");
    }
    if (!validNormalizationOptions(options)) {
        return normalizationError("ArgumentInvalid", "Frame normalization options are outside valid ranges");
    }

    float inputBackground = backgroundMedian(image);
    float measuredReferenceBackground = backgroundMedian(reference);
    float inputScale = robustScale(image, inputBackground);
    float measuredReferenceScale = robustScale(reference, measuredReferenceBackground);
    if (image.width == reference.width && image.height == reference.height) {
        const auto paired = pairedStatistics(image, reference);
        if (!paired.ok) {
            return normalizationError("ImageCoverageEmpty", "Frame normalization requires shared valid coverage");
        }
        inputBackground = paired.inputBackground;
        measuredReferenceBackground = paired.referenceBackground;
        inputScale = paired.inputScale;
        measuredReferenceScale = paired.referenceScale;
    }
    if (!std::isfinite(inputBackground) || !std::isfinite(measuredReferenceBackground) ||
        !std::isfinite(inputScale) || !std::isfinite(measuredReferenceScale)) {
        return normalizationError("ImageCoverageEmpty", "Frame normalization requires valid input and reference pixels");
    }
    const float referenceBackground =
        options.targetBackground > 0.0F ? options.targetBackground : measuredReferenceBackground;
    const float referenceScale = options.targetScale > 0.0F ? options.targetScale : measuredReferenceScale;
    return applyNormalization(image, inputBackground, inputScale, referenceBackground, referenceScale, options);
}

FrameNormalizationResult FrameNormalizer::matchReferenceLocal(const ImageBuffer& image, const ImageBuffer& reference,
                                                              const LocalNormalizationOptions& options) const {
    if (image.empty() || reference.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount() ||
        reference.pixels.size() != reference.sampleCount()) {
        return normalizationError("ImageBufferInvalid", "Input and reference images must be non-empty float images");
    }
    if (image.width != reference.width || image.height != reference.height || image.channels != reference.channels) {
        return normalizationError("ImageDimensionsMismatch", "Input and reference dimensions must match");
    }
    if (image.colorEncoding != reference.colorEncoding) {
        return normalizationError("ImageColorEncodingMismatch",
                                  "Input and reference images must use the same color encoding");
    }
    if (options.columns == 0 || options.rows == 0 || options.columns > image.width ||
        options.rows > image.height || !validNormalizationOptions(options.normalization)) {
        return normalizationError("ArgumentInvalid", "Local normalization options are outside valid ranges");
    }

    const auto paired = pairedStatistics(image, reference);
    if (!paired.ok) {
        return normalizationError("ImageCoverageEmpty", "Frame normalization requires shared valid coverage");
    }
    const float inputBackground = paired.inputBackground;
    const float measuredReferenceBackground = paired.referenceBackground;
    const float referenceBackground = options.normalization.targetBackground > 0.0F
                                          ? options.normalization.targetBackground
                                          : measuredReferenceBackground;
    auto grids = backgroundGridPair(
        image,
        reference,
        options.columns,
        options.rows,
        inputBackground,
        measuredReferenceBackground
    );
    const float referenceOffset = referenceBackground - measuredReferenceBackground;
    for (auto& value : grids.reference) {
        value += referenceOffset;
    }
    const float referenceScale = options.normalization.targetScale > 0.0F ? options.normalization.targetScale
                                                                          : paired.referenceScale;
    const float inputScale = std::max(options.normalization.epsilon, paired.inputScale);
    const float scale = std::max(options.normalization.epsilon, referenceScale) / inputScale;
    if (!std::isfinite(scale)) {
        return normalizationError("ImageValueInvalid", "Local frame normalization scale must be finite");
    }

    ImageBuffer output = image;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float localInput = sampleGrid(grids.input, options.columns, options.rows, image, x, y);
            const float localReference = sampleGrid(grids.reference, options.columns, options.rows, reference, x, y);
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            if (!pixelIsValid(image, pixel)) {
                for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                    output.pixels[offset + channel] = 0.0F;
                }
                continue;
            }
            for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                if (image.channels >= 4 && channel == 3) {
                    output.pixels[offset + channel] = image.pixels[offset + channel];
                } else {
                    const float normalized = (image.pixels[offset + channel] - localInput) * scale + localReference;
                    output.pixels[offset + channel] = outputColor(normalized, options.normalization.clampOutput);
                }
            }
        }
    }

    FrameNormalizationResult result;
    result.ok = true;
    result.image = std::move(output);
    result.inputBackground = inputBackground;
    result.outputBackground = referenceBackground;
    result.scale = scale;
    return result;
}

} // namespace photonstack
