#include "photonstack/BackgroundExtractor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <vector>

namespace photonstack {
namespace {

BackgroundExtractionResult backgroundError(std::string code, std::string message) {
    BackgroundExtractionResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool validBackgroundMode(BackgroundMode mode) {
    switch (mode) {
    case BackgroundMode::Subtract:
    case BackgroundMode::Divide:
        return true;
    }
    return false;
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
    return static_cast<float>((static_cast<double>(*lower) + static_cast<double>(*middle)) * 0.5);
}

struct WeightedSample {
    float value = 0.0F;
    float weight = 0.0F;
};

std::optional<float> weightedQuantile(std::vector<WeightedSample>& samples, double quantile) {
    if (samples.empty()) {
        return std::nullopt;
    }
    std::sort(samples.begin(), samples.end(), [](const WeightedSample& left, const WeightedSample& right) {
        return left.value < right.value;
    });
    double totalWeight = 0.0;
    for (const auto& sample : samples) {
        totalWeight += sample.weight;
    }
    if (!std::isfinite(totalWeight) || totalWeight <= 0.0) {
        return std::nullopt;
    }

    double cumulative = 0.0;
    double previousPosition = 0.0;
    float previousValue = samples.front().value;
    bool hasPrevious = false;
    for (const auto& sample : samples) {
        cumulative += sample.weight;
        const double position = (cumulative - static_cast<double>(sample.weight) * 0.5) / totalWeight;
        if (position >= quantile) {
            if (!hasPrevious || position <= previousPosition) {
                return sample.value;
            }
            const double fraction = std::clamp(
                (quantile - previousPosition) / (position - previousPosition),
                0.0,
                1.0
            );
            return static_cast<float>(static_cast<double>(previousValue) +
                                      (static_cast<double>(sample.value) - previousValue) * fraction);
        }
        previousPosition = position;
        previousValue = sample.value;
        hasPrevious = true;
    }
    return samples.back().value;
}

std::optional<float> weightedMedian(std::vector<WeightedSample>& samples) {
    return weightedQuantile(samples, 0.5);
}

std::optional<float> gridBackgroundSample(std::vector<WeightedSample>& samples, bool protectBrightTargets) {
    return weightedQuantile(samples, protectBrightTargets ? 0.25 : 0.5);
}

float luminanceAt(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    if (image.channels >= 3) {
        return image.pixels[offset] * 0.2126F + image.pixels[offset + 1] * 0.7152F + image.pixels[offset + 2] * 0.0722F;
    }
    return image.pixels[offset];
}

float pixelCoverage(const ImageBuffer& image, std::size_t pixel) {
    if (image.channels < 4) {
        return 1.0F;
    }
    const float alpha = image.pixels[pixel * image.channels + 3];
    return std::isfinite(alpha) ? std::clamp(alpha, 0.0F, 1.0F) : 0.0F;
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
    return pixelCoverage(image, pixel) > 1.0e-6F && pixelHasFiniteColor(image, pixel);
}

std::optional<BackgroundExtractionResult> validateImageSamples(const ImageBuffer& image) {
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (image.channels >= 4) {
            const float alpha = image.pixels[pixel * image.channels + 3];
            if (!std::isfinite(alpha)) {
                return backgroundError("ImageBufferInvalid", "Input image contains a non-finite alpha value");
            }
        }
        if (pixelCoverage(image, pixel) > 1.0e-6F && !pixelHasFiniteColor(image, pixel)) {
            return backgroundError("ImageBufferInvalid", "Input image contains a non-finite visible color value");
        }
    }
    return std::nullopt;
}

std::optional<float> outputColor(double value, bool clampOutput) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    if (clampOutput) {
        return static_cast<float>(std::clamp(value, 0.0, 1.0));
    }
    constexpr double maximum = std::numeric_limits<float>::max();
    if (value < -maximum || value > maximum) {
        return std::nullopt;
    }
    return static_cast<float>(value);
}

std::uint16_t colorChannelCount(const ImageBuffer& image) {
    return std::min<std::uint16_t>(3, image.channels);
}

std::optional<float> channelMedian(const ImageBuffer& image, std::uint16_t channel) {
    std::vector<WeightedSample> samples;
    samples.reserve(image.pixelCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (!pixelIsValid(image, pixel)) {
            continue;
        }
        samples.push_back({image.pixels[pixel * image.channels + channel], pixelCoverage(image, pixel)});
    }
    return weightedMedian(samples);
}

void fillMissingGridCells(std::vector<float>& grid,
                          std::vector<float>& channelGrid,
                          std::vector<std::uint8_t>& sampled,
                          std::uint32_t columns,
                          std::uint32_t rows,
                          std::uint16_t colorChannels) {
    std::queue<std::size_t> pending;
    for (std::size_t cell = 0; cell < sampled.size(); ++cell) {
        if (sampled[cell]) {
            pending.push(cell);
        }
    }

    const auto visit = [&](std::size_t source, std::uint32_t x, std::uint32_t y) {
        const auto target = static_cast<std::size_t>(y) * columns + x;
        if (sampled[target]) {
            return;
        }
        sampled[target] = 1;
        grid[target] = grid[source];
        for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
            channelGrid[target * colorChannels + channel] = channelGrid[source * colorChannels + channel];
        }
        pending.push(target);
    };

    while (!pending.empty()) {
        const auto source = pending.front();
        pending.pop();
        const auto x = static_cast<std::uint32_t>(source % columns);
        const auto y = static_cast<std::uint32_t>(source / columns);
        if (x > 0) {
            visit(source, x - 1, y);
        }
        if (x + 1 < columns) {
            visit(source, x + 1, y);
        }
        if (y > 0) {
            visit(source, x, y - 1);
        }
        if (y + 1 < rows) {
            visit(source, x, y + 1);
        }
    }
}

} // namespace

BackgroundExtractionResult BackgroundExtractor::extractGlobal(const ImageBuffer& image,
                                                              const BackgroundExtractionOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return backgroundError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (const auto invalid = validateImageSamples(image); invalid.has_value()) {
        return *invalid;
    }
    if (!std::isfinite(options.strength) || options.strength < 0.0F || options.strength > 1.0F) {
        return backgroundError("ArgumentInvalid", "Background extraction strength must be between 0 and 1");
    }
    if (!validBackgroundMode(options.mode)) {
        return backgroundError("ArgumentInvalid", "Unsupported background extraction mode");
    }
    if (!std::isfinite(options.epsilon) || options.epsilon <= 0.0F) {
        return backgroundError("ArgumentInvalid", "epsilon must be greater than zero");
    }

    std::vector<WeightedSample> samples;
    samples.reserve(image.pixelCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (!pixelIsValid(image, pixel)) {
            continue;
        }
        const float luminance = luminanceAt(image, pixel);
        if (std::isfinite(luminance)) {
            samples.push_back({luminance, pixelCoverage(image, pixel)});
        }
    }
    const auto measuredBackground = weightedMedian(samples);
    if (!measuredBackground.has_value()) {
        return backgroundError("ImageCoverageEmpty", "Background extraction requires at least one valid pixel");
    }

    const float background = *measuredBackground;
    float channelBackgrounds[3] = {background, background, background};
    const auto colorChannels = colorChannelCount(image);
    for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
        const auto channelBackground = channelMedian(image, channel);
        if (!channelBackground.has_value()) {
            return backgroundError("ImageCoverageEmpty", "Background extraction requires at least one valid pixel");
        }
        channelBackgrounds[channel] = *channelBackground;
    }

    ImageBuffer output;
    output.width = image.width;
    output.height = image.height;
    output.channels = image.channels;
    output.format = image.format;
    output.colorEncoding = image.colorEncoding;
    output.sourceBitsPerChannel = image.sourceBitsPerChannel;
    output.pixels.assign(image.sampleCount(), 0.0F);

    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        if (!pixelIsValid(image, pixel)) {
            for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                output.pixels[offset + channel] = 0.0F;
            }
            continue;
        }
        for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
            if (channel >= colorChannels) {
                output.pixels[offset + channel] = image.pixels[offset + channel];
                continue;
            }

            const float localBackground = channelBackgrounds[channel];
            double corrected = image.pixels[offset + channel];
            if (options.strength == 0.0F) {
                corrected = image.pixels[offset + channel];
            } else if (options.mode == BackgroundMode::Subtract) {
                corrected = static_cast<double>(image.pixels[offset + channel]) -
                            static_cast<double>(localBackground) * options.strength;
            } else {
                if (localBackground <= options.epsilon) {
                    return backgroundError("BackgroundDivisionInvalid",
                                           "Divide mode requires a positive background model above epsilon");
                }
                const double targetBackground = options.preserveBrightness ? channelBackgrounds[channel] : 1.0;
                const double divided = static_cast<double>(image.pixels[offset + channel]) *
                                       targetBackground / localBackground;
                corrected = static_cast<double>(image.pixels[offset + channel]) * (1.0 - options.strength) +
                            divided * options.strength;
            }
            const auto encoded = outputColor(corrected, options.clampOutput);
            if (!encoded.has_value()) {
                return backgroundError("ImageValueInvalid", "Background extraction produced a non-finite or out-of-range value");
            }
            output.pixels[offset + channel] = *encoded;
        }
    }

    BackgroundExtractionResult result;
    result.ok = true;
    result.image = std::move(output);
    result.background = background;
    return result;
}

BackgroundExtractionResult BackgroundExtractor::extractGrid(const ImageBuffer& image,
                                                            const BackgroundGridOptions& options) const {
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return backgroundError("ImageBufferInvalid", "Input image must be a non-empty float image");
    }
    if (const auto invalid = validateImageSamples(image); invalid.has_value()) {
        return *invalid;
    }
    if (!std::isfinite(options.extraction.strength) || options.extraction.strength < 0.0F ||
        options.extraction.strength > 1.0F) {
        return backgroundError("ArgumentInvalid", "Background extraction strength must be between 0 and 1");
    }
    if (!std::isfinite(options.extraction.epsilon) || options.extraction.epsilon <= 0.0F) {
        return backgroundError("ArgumentInvalid", "epsilon must be greater than zero");
    }
    if (!validBackgroundMode(options.extraction.mode)) {
        return backgroundError("ArgumentInvalid", "Unsupported background extraction mode");
    }
    if (options.columns == 0 || options.rows == 0) {
        return backgroundError("ArgumentInvalid", "Background grid dimensions must be greater than zero");
    }
    if (options.columns > image.width || options.rows > image.height) {
        return backgroundError("ArgumentInvalid", "Background grid dimensions cannot exceed the image dimensions");
    }

    const auto gridSize = static_cast<std::size_t>(options.columns) * options.rows;
    const auto colorChannels = colorChannelCount(image);
    std::vector<WeightedSample> validLuminance;
    validLuminance.reserve(image.pixelCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (pixelIsValid(image, pixel)) {
            const float luminance = luminanceAt(image, pixel);
            if (std::isfinite(luminance)) {
                validLuminance.push_back({luminance, pixelCoverage(image, pixel)});
            }
        }
    }
    const auto measuredFallback = weightedMedian(validLuminance);
    if (!measuredFallback.has_value()) {
        return backgroundError("ImageCoverageEmpty", "Background extraction requires at least one valid pixel");
    }
    const float fallbackBackground = *measuredFallback;
    float fallbackChannelBackgrounds[3] = {fallbackBackground, fallbackBackground, fallbackBackground};
    for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
        const auto channelBackground = channelMedian(image, channel);
        if (!channelBackground.has_value()) {
            return backgroundError("ImageCoverageEmpty", "Background extraction requires at least one valid pixel");
        }
        fallbackChannelBackgrounds[channel] = *channelBackground;
    }
    std::vector<float> grid(gridSize, 0.0F);
    std::vector<float> channelGrid(gridSize * std::max<std::uint16_t>(1, colorChannels), 0.0F);
    std::vector<std::uint8_t> sampledCells(gridSize, 0);
    std::vector<WeightedSample> samples;
    for (std::uint32_t gy = 0; gy < options.rows; ++gy) {
        const auto y0 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(gy) * image.height) /
                                                   static_cast<std::uint64_t>(options.rows));
        const auto y1 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(gy + 1) * image.height) /
                                                   static_cast<std::uint64_t>(options.rows));
        for (std::uint32_t gx = 0; gx < options.columns; ++gx) {
            const auto x0 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(gx) * image.width) /
                                                       static_cast<std::uint64_t>(options.columns));
            const auto x1 = static_cast<std::uint32_t>((static_cast<std::uint64_t>(gx + 1) * image.width) /
                                                       static_cast<std::uint64_t>(options.columns));
            samples.clear();
            samples.reserve(static_cast<std::size_t>(std::max<std::uint32_t>(1, x1 - x0)) *
                            std::max<std::uint32_t>(1, y1 - y0));
            for (std::uint32_t y = y0; y < y1; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const auto pixel = static_cast<std::size_t>(y) * image.width + x;
                    if (pixelIsValid(image, pixel)) {
                        const float luminance = luminanceAt(image, pixel);
                        if (std::isfinite(luminance)) {
                            samples.push_back({luminance, pixelCoverage(image, pixel)});
                        }
                    }
                }
            }
            const auto cellIndex = static_cast<std::size_t>(gy) * options.columns + gx;
            const auto cellBackground = gridBackgroundSample(samples, options.protectBrightTargets);
            if (!cellBackground.has_value()) {
                if (!options.protectBrightTargets) {
                    grid[cellIndex] = fallbackBackground;
                }
                for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                    channelGrid[cellIndex * colorChannels + channel] = fallbackChannelBackgrounds[channel];
                }
                continue;
            }
            sampledCells[cellIndex] = 1;
            grid[cellIndex] = *cellBackground;

            for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                samples.clear();
                samples.reserve(static_cast<std::size_t>(std::max<std::uint32_t>(1, x1 - x0)) *
                                std::max<std::uint32_t>(1, y1 - y0));
                for (std::uint32_t y = y0; y < y1; ++y) {
                    for (std::uint32_t x = x0; x < x1; ++x) {
                        const auto pixel = static_cast<std::size_t>(y) * image.width + x;
                        if (pixelIsValid(image, pixel)) {
                            samples.push_back({image.pixels[pixel * image.channels + channel],
                                               pixelCoverage(image, pixel)});
                        }
                    }
                }
                channelGrid[cellIndex * colorChannels + channel] =
                    gridBackgroundSample(samples, options.protectBrightTargets)
                        .value_or(fallbackChannelBackgrounds[channel]);
            }
        }
    }

    const auto sampledGridCells = static_cast<std::uint32_t>(std::count(sampledCells.begin(), sampledCells.end(), 1));
    const auto filledGridCells = static_cast<std::uint32_t>(gridSize) - sampledGridCells;
    float targetChannelBackgrounds[3] = {
        fallbackChannelBackgrounds[0],
        fallbackChannelBackgrounds[1],
        fallbackChannelBackgrounds[2],
    };
    float globalBackground = fallbackBackground;
    if (options.protectBrightTargets) {
        std::vector<float> sampledGrid;
        sampledGrid.reserve(sampledGridCells);
        for (std::size_t cell = 0; cell < gridSize; ++cell) {
            if (sampledCells[cell]) {
                sampledGrid.push_back(grid[cell]);
            }
        }
        globalBackground = median(sampledGrid);
        for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
            std::vector<float> sampledChannelGrid;
            sampledChannelGrid.reserve(sampledGridCells);
            for (std::size_t cell = 0; cell < gridSize; ++cell) {
                if (sampledCells[cell]) {
                    sampledChannelGrid.push_back(channelGrid[cell * colorChannels + channel]);
                }
            }
            targetChannelBackgrounds[channel] = median(sampledChannelGrid);
        }
        fillMissingGridCells(
            grid,
            channelGrid,
            sampledCells,
            options.columns,
            options.rows,
            colorChannels
        );
    } else {
        std::vector<float> sortedGrid = grid;
        globalBackground = median(sortedGrid);
    }

    ImageBuffer output;
    output.width = image.width;
    output.height = image.height;
    output.channels = image.channels;
    output.format = image.format;
    output.colorEncoding = image.colorEncoding;
    output.sourceBitsPerChannel = image.sourceBitsPerChannel;
    output.pixels.assign(image.sampleCount(), 0.0F);

    auto sampleGrid = [&](const std::vector<float>& values, std::uint16_t channel, std::uint32_t x, std::uint32_t y) {
        const float fx =
            (static_cast<float>(x) + 0.5F) * static_cast<float>(options.columns) / static_cast<float>(image.width) -
            0.5F;
        const float fy =
            (static_cast<float>(y) + 0.5F) * static_cast<float>(options.rows) / static_cast<float>(image.height) - 0.5F;
        const auto x0 =
            static_cast<std::uint32_t>(std::floor(std::clamp(fx, 0.0F, static_cast<float>(options.columns - 1))));
        const auto y0 =
            static_cast<std::uint32_t>(std::floor(std::clamp(fy, 0.0F, static_cast<float>(options.rows - 1))));
        const auto x1 = std::min(x0 + 1, options.columns - 1);
        const auto y1 = std::min(y0 + 1, options.rows - 1);
        const float tx = std::clamp(fx - static_cast<float>(x0), 0.0F, 1.0F);
        const float ty = std::clamp(fy - static_cast<float>(y0), 0.0F, 1.0F);
        const auto index = [&](std::uint32_t gx, std::uint32_t gy, std::uint16_t sampleChannel) {
            const auto cell = static_cast<std::size_t>(gy) * options.columns + gx;
            return values.size() == gridSize ? cell : cell * colorChannels + sampleChannel;
        };
        const float top = values[index(x0, y0, channel)] + (values[index(x1, y0, channel)] - values[index(x0, y0, channel)]) * tx;
        const float bottom = values[index(x0, y1, channel)] + (values[index(x1, y1, channel)] - values[index(x0, y1, channel)]) * tx;
        return top + (bottom - top) * ty;
    };

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixelOffset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            if (!pixelIsValid(image, pixel)) {
                for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                    output.pixels[pixelOffset + channel] = 0.0F;
                }
                continue;
            }
            for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                const auto sample = pixelOffset + channel;
                if (channel >= colorChannels) {
                    output.pixels[sample] = image.pixels[sample];
                    continue;
                }

                const float localBackground = sampleGrid(channelGrid, channel, x, y);
                double corrected = image.pixels[sample];
                if (options.extraction.strength == 0.0F) {
                    corrected = image.pixels[sample];
                } else if (options.extraction.mode == BackgroundMode::Subtract) {
                    corrected = static_cast<double>(image.pixels[sample]) -
                                static_cast<double>(localBackground) * options.extraction.strength;
                } else {
                    if (localBackground <= options.extraction.epsilon) {
                        return backgroundError("BackgroundDivisionInvalid",
                                               "Divide mode requires a positive background model above epsilon");
                    }
                    const double targetBackground = options.extraction.preserveBrightness
                                                        ? targetChannelBackgrounds[channel]
                                                        : 1.0;
                    const double divided = static_cast<double>(image.pixels[sample]) *
                                           targetBackground / localBackground;
                    corrected = static_cast<double>(image.pixels[sample]) *
                                    (1.0 - options.extraction.strength) +
                                divided * options.extraction.strength;
                }
                const auto encoded = outputColor(corrected, options.extraction.clampOutput);
                if (!encoded.has_value()) {
                    return backgroundError("ImageValueInvalid", "Background extraction produced a non-finite or out-of-range value");
                }
                output.pixels[sample] = *encoded;
            }
        }
    }

    BackgroundExtractionResult result;
    result.ok = true;
    result.image = std::move(output);
    result.background = globalBackground;
    result.backgroundGrid = std::move(grid);
    result.gridColumns = options.columns;
    result.gridRows = options.rows;
    result.sampledGridCells = sampledGridCells;
    result.filledGridCells = filledGridCells;
    return result;
}

} // namespace photonstack
