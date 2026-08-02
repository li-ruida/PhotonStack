#include "photonstack/CloudRemoval.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_set>
#include <utility>

namespace photonstack {
namespace {

CloudRemovalResult cloudError(std::string code, std::string message) {
    CloudRemovalResult result;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool validImage(const ImageBuffer& image) {
    if (image.empty() || image.channels < 3 || image.pixels.size() != image.sampleCount()) {
        return false;
    }
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        if (image.channels >= 4) {
            const float alpha = image.pixels[offset + 3];
            if (!std::isfinite(alpha)) {
                return false;
            }
            if (alpha <= 1.0e-6F) {
                continue;
            }
        }
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            if (!std::isfinite(image.pixels[offset + channel])) {
                return false;
            }
        }
    }
    return true;
}

float validPixelCoverage(const ImageBuffer& image, std::size_t offset) {
    for (std::uint16_t channel = 0; channel < 3; ++channel) {
        if (!std::isfinite(image.pixels[offset + channel])) {
            return 0.0F;
        }
    }
    if (image.channels < 4) {
        return 1.0F;
    }
    const float alpha = image.pixels[offset + 3];
    return std::isfinite(alpha) ? std::clamp(alpha, 0.0F, 1.0F) : 0.0F;
}

float luminanceAt(const ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
    return image.pixels[offset] * 0.2126F + image.pixels[offset + 1] * 0.7152F + image.pixels[offset + 2] * 0.0722F;
}

float median(std::vector<float> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](float value) {
        return !std::isfinite(value);
    }), values.end());
    if (values.empty()) {
        return 0.0F;
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

struct TileStats {
    float contrast = 0.0F;
    float starDensity = 0.0F;
    float lowerLuminance = 0.0F;
    std::array<float, 3> lowerColor = {0.0F, 0.0F, 0.0F};
    double sampleWeight = 0.0;
};

template <std::size_t BinCount>
float histogramQuantile(const std::array<double, BinCount>& histogram, double count, float quantile) {
    if (!(count > 1.0e-8)) {
        return 0.0F;
    }
    const double target = std::clamp(static_cast<double>(quantile), 0.0, 1.0) * count;
    double cumulative = 0.0;
    for (std::size_t bin = 0; bin < histogram.size(); ++bin) {
        cumulative += histogram[bin];
        if (cumulative >= std::max(1.0e-8, target)) {
            return static_cast<float>(bin) / static_cast<float>(BinCount - 1);
        }
    }
    return 1.0F;
}

std::vector<TileStats> computeTiles(const ImageBuffer& image, std::uint32_t columns, std::uint32_t rows) {
    std::vector<TileStats> tiles(static_cast<std::size_t>(columns) * rows);
    for (std::uint32_t row = 0; row < rows; ++row) {
        const std::uint32_t y0 = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(row) * image.height / rows
        );
        const std::uint32_t y1 = std::max<std::uint32_t>(
            y0 + 1,
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(row + 1) * image.height / rows)
        );
        for (std::uint32_t column = 0; column < columns; ++column) {
            const std::uint32_t x0 = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(column) * image.width / columns
            );
            const std::uint32_t x1 = std::max<std::uint32_t>(
                x0 + 1,
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(column + 1) * image.width / columns)
            );
            double sum = 0.0;
            double sumSquared = 0.0;
            double count = 0.0;
            constexpr std::size_t colorBins = 1024;
            std::array<double, colorBins> redHistogram = {};
            std::array<double, colorBins> greenHistogram = {};
            std::array<double, colorBins> blueHistogram = {};
            std::array<double, colorBins> luminanceHistogram = {};
            const auto colorBin = [](float value) {
                return static_cast<std::size_t>(std::clamp(
                    static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * static_cast<float>(colorBins - 1))),
                    0,
                    static_cast<int>(colorBins - 1)
                ));
            };
            for (std::uint32_t y = y0; y < y1; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
                    const float coverage = validPixelCoverage(image, offset);
                    if (coverage <= 1.0e-6F) {
                        continue;
                    }
                    const float value = luminanceAt(image, x, y);
                    sum += static_cast<double>(value) * coverage;
                    sumSquared += static_cast<double>(value) * value * coverage;
                    redHistogram[colorBin(image.pixels[offset])] += coverage;
                    greenHistogram[colorBin(image.pixels[offset + 1])] += coverage;
                    blueHistogram[colorBin(image.pixels[offset + 2])] += coverage;
                    luminanceHistogram[colorBin(value)] += coverage;
                    count += coverage;
                }
            }
            const auto tileIndex = static_cast<std::size_t>(row) * columns + column;
            if (!(count > 1.0e-8)) {
                const float invalid = std::numeric_limits<float>::quiet_NaN();
                tiles[tileIndex] = {
                    .contrast = invalid,
                    .starDensity = invalid,
                    .lowerLuminance = invalid,
                    .lowerColor = {invalid, invalid, invalid},
                    .sampleWeight = 0.0,
                };
                continue;
            }
            const float mean = static_cast<float>(sum / count);
            const float variance = static_cast<float>(sumSquared / count - mean * mean);
            double brightPointCount = 0.0;
            const float starThreshold = mean + std::max(0.025F, std::sqrt(std::max(0.0F, variance)) * 2.15F);
            for (std::uint32_t y = y0; y < y1; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
                    const float coverage = validPixelCoverage(image, offset);
                    if (coverage <= 1.0e-6F) {
                        continue;
                    }
                    if (luminanceAt(image, x, y) > starThreshold) {
                        brightPointCount += coverage;
                    }
                }
            }
            tiles[tileIndex] = {
                .contrast = histogramQuantile(luminanceHistogram, count, 0.70F) -
                            histogramQuantile(luminanceHistogram, count, 0.20F),
                .starDensity = static_cast<float>(brightPointCount / count),
                .lowerLuminance = histogramQuantile(luminanceHistogram, count, 0.30F),
                .lowerColor = {
                    histogramQuantile(redHistogram, count, 0.30F),
                    histogramQuantile(greenHistogram, count, 0.30F),
                    histogramQuantile(blueHistogram, count, 0.30F),
                },
                .sampleWeight = count,
            };
        }
    }
    return tiles;
}

float smoothstep(float edge0, float edge1, float value) {
    if (edge1 <= edge0) {
        return value >= edge1 ? 1.0F : 0.0F;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

float lowerQuantile(std::vector<float> values, float quantile) {
    values.erase(std::remove_if(values.begin(), values.end(), [](float value) {
        return !std::isfinite(value);
    }), values.end());
    if (values.empty()) {
        return 0.0F;
    }
    const std::size_t index = static_cast<std::size_t>(std::floor(
        std::clamp(quantile, 0.0F, 1.0F) * static_cast<float>(values.size() - 1)
    ));
    const auto position = values.begin() + static_cast<std::ptrdiff_t>(index);
    std::nth_element(values.begin(), position, values.end());
    return *position;
}

std::vector<float> estimateBackgroundSurface(
    const std::vector<float>& levels,
    std::uint32_t columns,
    std::uint32_t rows,
    float quantile = 0.25F
) {
    const float globalLevel = lowerQuantile(levels, quantile);

    std::vector<float> rowLevels(rows, globalLevel);
    for (std::uint32_t row = 0; row < rows; ++row) {
        std::vector<float> values;
        values.reserve(columns);
        for (std::uint32_t column = 0; column < columns; ++column) {
            const float value = levels[static_cast<std::size_t>(row) * columns + column];
            if (std::isfinite(value)) {
                values.push_back(value);
            }
        }
        rowLevels[row] = values.empty() ? globalLevel : lowerQuantile(std::move(values), quantile);
    }

    std::vector<float> columnLevels(columns, globalLevel);
    for (std::uint32_t column = 0; column < columns; ++column) {
        std::vector<float> values;
        values.reserve(rows);
        for (std::uint32_t row = 0; row < rows; ++row) {
            const float value = levels[static_cast<std::size_t>(row) * columns + column];
            if (std::isfinite(value)) {
                values.push_back(value);
            }
        }
        columnLevels[column] = values.empty() ? globalLevel : lowerQuantile(std::move(values), quantile);
    }

    std::vector<float> surface(levels.size(), 0.0F);
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const auto row = static_cast<std::uint32_t>(index / columns);
        const auto column = static_cast<std::uint32_t>(index % columns);
        surface[index] = std::clamp(rowLevels[row] + columnLevels[column] - globalLevel, 0.0F, 1.0F);
    }
    return surface;
}

void interpolateMissingLevels(std::vector<float>& levels, float fallback) {
    std::size_t first = 0;
    while (first < levels.size() && !std::isfinite(levels[first])) {
        ++first;
    }
    if (first == levels.size()) {
        std::fill(levels.begin(), levels.end(), fallback);
        return;
    }
    std::fill(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(first), levels[first]);
    std::size_t previous = first;
    for (std::size_t index = first + 1; index < levels.size(); ++index) {
        if (!std::isfinite(levels[index])) {
            continue;
        }
        const float start = levels[previous];
        const float end = levels[index];
        const std::size_t distance = index - previous;
        for (std::size_t fill = previous + 1; fill < index; ++fill) {
            const float fraction = static_cast<float>(fill - previous) / static_cast<float>(distance);
            levels[fill] = start * (1.0F - fraction) + end * fraction;
        }
        previous = index;
    }
    std::fill(levels.begin() + static_cast<std::ptrdiff_t>(previous + 1), levels.end(), levels[previous]);
}

// Estimate the unobscured background after cloud detection.  The original estimator
// intentionally uses a low quantile, but a broad veil can still dominate that
// quantile.  Excluding detected cells prevents the correction from using haze as
// its own reference; interpolating fully-covered rows/columns preserves gradients.
std::vector<float> estimateUnobscuredBackgroundSurface(
    const std::vector<float>& levels,
    const std::vector<float>& obscuredMask,
    std::uint32_t columns,
    std::uint32_t rows,
    float quantile
) {
    std::vector<float> clearLevels;
    clearLevels.reserve(levels.size());
    for (std::size_t index = 0; index < levels.size(); ++index) {
        if (obscuredMask[index] < 0.5F && std::isfinite(levels[index])) {
            clearLevels.push_back(levels[index]);
        }
    }
    if (clearLevels.empty()) {
        return estimateBackgroundSurface(levels, columns, rows, quantile);
    }
    const float globalLevel = lowerQuantile(clearLevels, quantile);
    std::vector<float> rowLevels(rows, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> columnLevels(columns, std::numeric_limits<float>::quiet_NaN());
    for (std::uint32_t row = 0; row < rows; ++row) {
        std::vector<float> values;
        values.reserve(columns);
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto index = static_cast<std::size_t>(row) * columns + column;
            if (obscuredMask[index] < 0.5F && std::isfinite(levels[index])) {
                values.push_back(levels[index]);
            }
        }
        if (!values.empty()) {
            rowLevels[row] = lowerQuantile(std::move(values), quantile);
        }
    }
    for (std::uint32_t column = 0; column < columns; ++column) {
        std::vector<float> values;
        values.reserve(rows);
        for (std::uint32_t row = 0; row < rows; ++row) {
            const auto index = static_cast<std::size_t>(row) * columns + column;
            if (obscuredMask[index] < 0.5F && std::isfinite(levels[index])) {
                values.push_back(levels[index]);
            }
        }
        if (!values.empty()) {
            columnLevels[column] = lowerQuantile(std::move(values), quantile);
        }
    }
    interpolateMissingLevels(rowLevels, globalLevel);
    interpolateMissingLevels(columnLevels, globalLevel);

    std::vector<float> surface(levels.size(), globalLevel);
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const auto row = static_cast<std::uint32_t>(index / columns);
        const auto column = static_cast<std::uint32_t>(index % columns);
        surface[index] = std::clamp(rowLevels[row] + columnLevels[column] - globalLevel, 0.0F, 1.0F);
    }
    return surface;
}

void removeBoundaryCells(
    std::vector<std::uint8_t>& mask,
    std::uint32_t columns,
    std::uint32_t rows
) {
    for (std::uint32_t x = 0; x < columns; ++x) {
        mask[x] = 0;
        mask[static_cast<std::size_t>(rows - 1) * columns + x] = 0;
    }
    for (std::uint32_t y = 0; y < rows; ++y) {
        mask[static_cast<std::size_t>(y) * columns] = 0;
        mask[static_cast<std::size_t>(y) * columns + columns - 1] = 0;
    }
}

std::size_t largestConnectedComponentSize(
    const std::vector<std::uint8_t>& mask,
    std::uint32_t columns,
    std::uint32_t rows
) {
    std::vector<std::uint8_t> visited(mask.size(), 0);
    std::size_t largest = 0;
    constexpr int offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto start = static_cast<std::size_t>(row) * columns + column;
            if (!mask[start] || visited[start]) {
                continue;
            }
            std::queue<std::pair<std::uint32_t, std::uint32_t>> queue;
            queue.emplace(column, row);
            visited[start] = 1;
            std::size_t count = 0;
            while (!queue.empty()) {
                const auto [x, y] = queue.front();
                queue.pop();
                ++count;
                for (const auto& offset : offsets) {
                    const int nx = static_cast<int>(x) + offset[0];
                    const int ny = static_cast<int>(y) + offset[1];
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(columns) || ny >= static_cast<int>(rows)) {
                        continue;
                    }
                    const auto neighbor = static_cast<std::size_t>(ny) * columns + static_cast<std::size_t>(nx);
                    if (mask[neighbor] && !visited[neighbor]) {
                        visited[neighbor] = 1;
                        queue.emplace(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny));
                    }
                }
            }
            largest = std::max(largest, count);
        }
    }
    return largest;
}

std::vector<CloudMaskRect> makeMaskRects(
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cells,
    const ImageBuffer& image,
    std::uint32_t columns,
    std::uint32_t rows
) {
    std::sort(cells.begin(), cells.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second == rhs.second ? lhs.first < rhs.first : lhs.second < rhs.second;
    });
    std::vector<CloudMaskRect> rects;
    std::size_t index = 0;
    while (index < cells.size()) {
        const std::uint32_t row = cells[index].second;
        std::uint32_t firstColumn = cells[index].first;
        std::uint32_t lastColumn = firstColumn;
        ++index;
        while (index < cells.size() && cells[index].second == row && cells[index].first == lastColumn + 1) {
            lastColumn = cells[index].first;
            ++index;
        }
        const float x = static_cast<float>(firstColumn) * static_cast<float>(image.width) / static_cast<float>(columns);
        const float right =
            static_cast<float>(lastColumn + 1) * static_cast<float>(image.width) / static_cast<float>(columns);
        const float y = static_cast<float>(row) * static_cast<float>(image.height) / static_cast<float>(rows);
        const float bottom = static_cast<float>(row + 1) * static_cast<float>(image.height) / static_cast<float>(rows);
        rects.push_back({.x = x, .y = y, .width = right - x, .height = bottom - y});
    }
    return rects;
}

float sampleTileField(
    const std::vector<float>& field,
    std::uint32_t columns,
    std::uint32_t rows,
    const ImageBuffer& image,
    float px,
    float py
) {
    const float gridX = px * static_cast<float>(columns) / static_cast<float>(image.width) - 0.5F;
    const float gridY = py * static_cast<float>(rows) / static_cast<float>(image.height) - 0.5F;
    const int rawX0 = static_cast<int>(std::floor(gridX));
    const int rawY0 = static_cast<int>(std::floor(gridY));
    const float tx = gridX - static_cast<float>(rawX0);
    const float ty = gridY - static_cast<float>(rawY0);
    const auto clampX = [columns](int value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0, static_cast<int>(columns) - 1));
    };
    const auto clampY = [rows](int value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0, static_cast<int>(rows) - 1));
    };
    const std::uint32_t x0 = clampX(rawX0);
    const std::uint32_t x1 = clampX(rawX0 + 1);
    const std::uint32_t y0 = clampY(rawY0);
    const std::uint32_t y1 = clampY(rawY0 + 1);
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return field[static_cast<std::size_t>(y) * columns + x];
    };
    const float top = at(x0, y0) * (1.0F - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.0F - tx) + at(x1, y1) * tx;
    return top * (1.0F - ty) + bottom * ty;
}

} // namespace

CloudRemovalResult CloudRemoval::detect(const ImageBuffer& image, const CloudRemovalOptions& options) const {
    if (!validImage(image)) {
        return cloudError("ImageBufferInvalid", "Input image must be a non-empty RGB/RGBA float image");
    }
    if (!std::isfinite(options.minBrightnessDelta) || !std::isfinite(options.minCoverage) ||
        !std::isfinite(options.strength) || !std::isfinite(options.featherRadius) ||
        options.minBrightnessDelta < 0.0F || options.minCoverage < 0.0F || options.minCoverage > 1.0F ||
        options.strength < 0.0F || options.strength > 1.0F || options.featherRadius < 0.0F ||
        options.columns == 0 || options.columns > 96 || options.rows == 0 || options.rows > 64) {
        return cloudError("ArgumentInvalid", "Cloud removal options are outside valid ranges");
    }

    const std::uint32_t columns = std::min(options.columns, image.width);
    const std::uint32_t rows = std::min(options.rows, image.height);
    const auto tiles = computeTiles(image, columns, rows);
    std::vector<float> tileLevels;
    tileLevels.reserve(tiles.size());
    std::vector<float> starDensities;
    starDensities.reserve(tiles.size());
    std::vector<float> contrasts;
    contrasts.reserve(tiles.size());
    for (const auto& tile : tiles) {
        tileLevels.push_back(tile.lowerLuminance);
        if (tile.sampleWeight > 1.0e-8) {
            starDensities.push_back(tile.starDensity);
            contrasts.push_back(tile.contrast);
        }
    }
    if (starDensities.empty()) {
        return cloudError("ImageCoverageEmpty", "Cloud detection requires at least one valid covered pixel");
    }
    auto backgroundSurface = estimateBackgroundSurface(tileLevels, columns, rows);
    // Bright haze raises the lower luminance quantile, whereas opaque cloud lowers
    // it and hides stars.  Use a median background for the latter so a dark patch
    // cannot become its own baseline merely because it lies on an image edge.
    const auto darkCloudBackgroundSurface = estimateBackgroundSurface(tileLevels, columns, rows, 0.70F);
    const float medianStarDensity = median(starDensities);
    const float stellarReferenceDensity = lowerQuantile(starDensities, 0.70F);
    const float medianContrast = median(contrasts);
    std::vector<float> contrastDeviations;
    contrastDeviations.reserve(contrasts.size());
    for (float contrast : contrasts) {
        contrastDeviations.push_back(std::abs(contrast - medianContrast));
    }
    const float contrastMad = median(contrastDeviations);
    const float maxContrast = std::max(0.035F, medianContrast + std::max(0.015F, contrastMad * 2.8F));
    const float sparseStarLimit = std::max(0.0015F, medianStarDensity * 0.72F + 0.0008F);
    const float brightStarLimit = std::max(0.0025F, medianStarDensity * 1.18F + 0.0012F);
    std::vector<float> neighboringStarDensity(tiles.size(), 0.0F);
    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            std::vector<float> neighbors;
            neighbors.reserve(8);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    const int neighborX = static_cast<int>(column) + dx;
                    const int neighborY = static_cast<int>(row) + dy;
                    if (neighborX < 0 || neighborY < 0 || neighborX >= static_cast<int>(columns) ||
                        neighborY >= static_cast<int>(rows)) {
                        continue;
                    }
                    const float density = tiles[static_cast<std::size_t>(neighborY) * columns +
                                                static_cast<std::size_t>(neighborX)].starDensity;
                    if (std::isfinite(density)) {
                        neighbors.push_back(density);
                    }
                }
            }
            neighboringStarDensity[static_cast<std::size_t>(row) * columns + column] = median(std::move(neighbors));
        }
    }
    const auto classify = [&](const std::vector<float>& surface, bool majorityFallback, std::vector<std::uint8_t>* darkMask) {
        std::vector<float> residuals;
        residuals.reserve(tileLevels.size());
        std::vector<float> darkResiduals;
        darkResiduals.reserve(tileLevels.size());
        for (std::size_t index = 0; index < tileLevels.size(); ++index) {
            residuals.push_back(std::isfinite(tileLevels[index])
                ? tileLevels[index] - surface[index]
                : std::numeric_limits<float>::quiet_NaN());
            darkResiduals.push_back(std::isfinite(tileLevels[index])
                ? tileLevels[index] - darkCloudBackgroundSurface[index]
                : std::numeric_limits<float>::quiet_NaN());
        }
        const float residualCenter = majorityFallback
                                         ? lowerQuantile(residuals, 0.20F)
                                         : median(residuals);
        std::vector<float> residualDeviations;
        residualDeviations.reserve(residuals.size());
        for (float residual : residuals) {
            if (std::isfinite(residual)) {
                residualDeviations.push_back(std::abs(residual - residualCenter));
            }
        }
        const float residualMad = majorityFallback
                                      ? lowerQuantile(residualDeviations, 0.25F)
                                      : median(residualDeviations);
        const float threshold = residualCenter + std::max(options.minBrightnessDelta, residualMad * 2.8F);
        const float softThreshold =
            residualCenter + std::max(options.minBrightnessDelta * 0.38F, residualMad * 1.65F + 0.001F);
        const float darkResidualCenter = median(darkResiduals);
        std::vector<float> darkResidualDeviations;
        darkResidualDeviations.reserve(darkResiduals.size());
        for (float residual : darkResiduals) {
            if (std::isfinite(residual)) {
                darkResidualDeviations.push_back(std::abs(residual - darkResidualCenter));
            }
        }
        const float darkResidualMad = median(darkResidualDeviations);
        const float darkThreshold = darkResidualCenter -
                                    std::max(options.minBrightnessDelta * 0.20F, darkResidualMad * 1.35F);
        // Do not infer an opaque cloud from darkness alone: a real star field must
        // lose stars behind the dark patch.  This keeps sensor masks and generic
        // dark obstructions out of the cloud report.
        const bool hasStellarReference = stellarReferenceDensity >= 8.0e-5F;
        const float darkStarLimit = std::max(5.0e-5F, stellarReferenceDensity * 0.85F);
        const float darkEdgeThreshold = darkResidualCenter -
                                        std::max(options.minBrightnessDelta * 0.45F, darkResidualMad * 2.5F);
        const float darkEdgeSoftThreshold = darkResidualCenter -
                                            std::max(options.minBrightnessDelta * 0.05F, darkResidualMad * 0.40F);

        std::vector<std::uint8_t> cloudy(tiles.size(), 0);
        std::vector<std::uint8_t> darkCloudy(tiles.size(), 0);
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (!std::isfinite(residuals[i])) {
                continue;
            }
            const bool smooth = tiles[i].contrast <= maxContrast;
            const bool brightHaze = residuals[i] >= threshold && tiles[i].starDensity <= brightStarLimit;
            const bool sparseVeil = residuals[i] >= softThreshold &&
                                    tiles[i].contrast <= maxContrast * 0.78F &&
                                    tiles[i].starDensity <= sparseStarLimit;
            const bool localStarDeficit = neighboringStarDensity[i] >= 1.0e-4F &&
                                          tiles[i].starDensity <= neighboringStarDensity[i] * 0.55F;
            const bool darkCloud = hasStellarReference &&
                                   darkResiduals[i] <= darkThreshold &&
                                   tiles[i].contrast <= maxContrast * 1.15F &&
                                   tiles[i].starDensity <= darkStarLimit && localStarDeficit;
            const auto row = static_cast<std::uint32_t>(i / columns);
            const auto column = static_cast<std::uint32_t>(i % columns);
            const bool boundaryCell = row == 0 || row + 1 == rows || column == 0 || column + 1 == columns;
            // A bank of opaque cloud frequently enters from the frame edge, where
            // cloud texture can be counted as star-like detail.  Admit only very
            // dark, locally star-suppressed edge cores here; they may grow along
            // the same edge below. This rejects ordinary edge vignetting.
            const bool darkEdgeCore = boundaryCell && darkResiduals[i] <= darkEdgeThreshold &&
                                      tiles[i].contrast <= maxContrast * 1.25F && localStarDeficit;
            if (smooth && (brightHaze || sparseVeil || darkCloud || darkEdgeCore)) {
                cloudy[i] = 1;
            }
            darkCloudy[i] = (darkCloud || darkEdgeCore) ? 1 : 0;
        }

        // Stars and faint cloud edges can make an otherwise continuous veil tile fail
        // the seed gate. Grow only from accepted cloud cores so Milky Way structure
        // cannot create a region by itself, while weak residuals can close mask holes.
        for (int pass = 0; pass < 4; ++pass) {
            auto expanded = cloudy;
            auto expandedDarkCloudy = darkCloudy;
            bool changed = false;
            for (std::uint32_t row = 0; row < rows; ++row) {
                for (std::uint32_t column = 0; column < columns; ++column) {
                    const auto index = static_cast<std::size_t>(row) * columns + column;
                    const bool brightSupport = std::isfinite(residuals[index]) &&
                                               residuals[index] >= softThreshold &&
                                               tiles[index].contrast <= maxContrast &&
                                               tiles[index].starDensity <= brightStarLimit;
                    // A genuine veil can cover an isolated star.  Its tile is
                    // intentionally excluded from ordinary bright growth above,
                    // but may be filled when the low-frequency haze surrounds
                    // it on every side (see the four-neighbor bridge below).
                    const bool brightBridgeSupport = std::isfinite(residuals[index]) &&
                                                     residuals[index] >= softThreshold;
                    const bool darkEdgeSupport = darkResiduals[index] <= darkEdgeSoftThreshold &&
                                                 tiles[index].contrast <= maxContrast * 1.25F;
                    // Weak cloud edge is allowed only when it grows from an
                    // already-confirmed, very dark frame-edge core.  It is not
                    // an independent seed, so smooth sky falloff cannot start
                    // a dark-cloud component by itself.  Bright expansion keeps
                    // the same low-star gate as bright-haze seeding, preventing
                    // Milky Way structure from being absorbed into a veil mask.
                    if (cloudy[index]) {
                        continue;
                    }
                    std::size_t cloudyNeighbors = 0;
                    std::size_t darkNeighbors = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) {
                                continue;
                            }
                            const int neighborX = static_cast<int>(column) + dx;
                            const int neighborY = static_cast<int>(row) + dy;
                            if (neighborX < 0 || neighborY < 0 || neighborX >= static_cast<int>(columns) ||
                                neighborY >= static_cast<int>(rows)) {
                                continue;
                            }
                            cloudyNeighbors += cloudy[static_cast<std::size_t>(neighborY) * columns +
                                                      static_cast<std::size_t>(neighborX)] != 0;
                            darkNeighbors += darkCloudy[static_cast<std::size_t>(neighborY) * columns +
                                                         static_cast<std::size_t>(neighborX)] != 0;
                        }
                    }
                    if (darkEdgeSupport && darkNeighbors >= 1) {
                        expanded[index] = 1;
                        expandedDarkCloudy[index] = 1;
                        changed = true;
                    } else if (brightSupport && cloudyNeighbors >= 2) {
                        expanded[index] = 1;
                        changed = true;
                    } else if (brightBridgeSupport && cloudyNeighbors >= 4) {
                        // Four adjacent cloudy tiles indicate a star-sized
                        // hole inside a confirmed veil, unlike an edge-facing
                        // Milky Way field which has at most three neighbors.
                        expanded[index] = 1;
                        changed = true;
                    }
                }
            }
            cloudy.swap(expanded);
            darkCloudy.swap(expandedDarkCloudy);
            if (!changed) {
                break;
            }
        }
        if (darkMask != nullptr) {
            *darkMask = std::move(darkCloudy);
        }
        return cloudy;
    };

    std::vector<std::uint8_t> darkCloudy;
    auto cloudy = classify(backgroundSurface, false, &darkCloudy);
    // A dark cloud can pull the low-quantile bright-haze baseline down and
    // make otherwise clear tiles look like a broad, opposite-polarity veil.
    // Once dark cores are confirmed, estimate the bright background without
    // them and classify the bright branch again.  Keep the original dark mask
    // authoritative so this normalization cannot erase an edge cloud.
    if (std::any_of(darkCloudy.begin(), darkCloudy.end(), [](std::uint8_t value) { return value != 0; })) {
        std::vector<float> darkMask(darkCloudy.size(), 0.0F);
        for (std::size_t index = 0; index < darkCloudy.size(); ++index) {
            darkMask[index] = darkCloudy[index] ? 1.0F : 0.0F;
        }
        auto brightBackgroundSurface = estimateUnobscuredBackgroundSurface(
            tileLevels,
            darkMask,
            columns,
            rows,
            0.25F
        );
        std::vector<std::uint8_t> reclassifiedDark;
        auto reclassifiedCloudy = classify(brightBackgroundSurface, false, &reclassifiedDark);
        for (std::size_t index = 0; index < reclassifiedCloudy.size(); ++index) {
            if (reclassifiedDark[index] && !darkCloudy[index]) {
                reclassifiedCloudy[index] = 0;
            }
            if (darkCloudy[index]) {
                reclassifiedCloudy[index] = 1;
            }
        }
        backgroundSurface = std::move(brightBackgroundSurface);
        cloudy = std::move(reclassifiedCloudy);
    }
    const auto primaryDarkCloudy = darkCloudy;
    auto primaryBrightCloudy = cloudy;
    for (std::size_t index = 0; index < primaryBrightCloudy.size(); ++index) {
        if (primaryDarkCloudy[index]) {
            primaryBrightCloudy[index] = 0;
        }
    }
    const auto primaryLargest = largestConnectedComponentSize(primaryBrightCloudy, columns, rows);
    auto fallbackSurface = estimateBackgroundSurface(tileLevels, columns, rows, 0.10F);
    std::vector<std::uint8_t> fallbackDarkCloudy;
    auto fallbackCloudy = classify(fallbackSurface, true, &fallbackDarkCloudy);
    removeBoundaryCells(fallbackCloudy, columns, rows);
    removeBoundaryCells(fallbackDarkCloudy, columns, rows);
    auto fallbackBrightCloudy = fallbackCloudy;
    for (std::size_t index = 0; index < fallbackBrightCloudy.size(); ++index) {
        if (fallbackDarkCloudy[index]) {
            fallbackBrightCloudy[index] = 0;
        }
    }
    const auto fallbackLargest = largestConnectedComponentSize(fallbackBrightCloudy, columns, rows);
    const auto fallbackFraction = static_cast<float>(fallbackLargest) / static_cast<float>(fallbackBrightCloudy.size());
    bool usedMajorityFallback = false;
    if (fallbackLargest > primaryLargest && fallbackFraction >= 0.35F) {
        backgroundSurface = std::move(fallbackSurface);
        cloudy = std::move(fallbackBrightCloudy);
        darkCloudy.assign(cloudy.size(), 0);
        // The majority fallback intentionally trims boundary-connected bright
        // regions to reject horizon glow.  Keep independently confirmed dark
        // clouds, because real cloud banks commonly enter from an image edge.
        for (std::size_t index = 0; index < cloudy.size(); ++index) {
            if (primaryDarkCloudy[index]) {
                cloudy[index] = 1;
                darkCloudy[index] = 1;
            }
        }
        usedMajorityFallback = true;
    }

    // The seed classifier stays deliberately conservative.  Once it has found
    // a dark cloud touching an edge, permit two inward grid cells of a weaker
    // fringe to join it.  Keeping this as a post-pass on dark masks means a
    // bright Milky Way structure can neither initiate nor join the expansion.
    for (int pass = 0; pass < 2; ++pass) {
        auto expandedDark = darkCloudy;
        bool changed = false;
        for (std::uint32_t row = 0; row < rows; ++row) {
            for (std::uint32_t column = 0; column < columns; ++column) {
                const auto index = static_cast<std::size_t>(row) * columns + column;
                if (darkCloudy[index]) {
                    continue;
                }
                const auto boundaryDepth = std::min({
                    row,
                    rows - 1U - row,
                    column,
                    columns - 1U - column,
                });
                if (boundaryDepth > 2U ||
                    !std::isfinite(tileLevels[index]) ||
                    tileLevels[index] > darkCloudBackgroundSurface[index] -
                                            std::max(options.minBrightnessDelta * 0.01F, 0.0005F) ||
                    tiles[index].contrast > maxContrast * 1.35F ||
                    neighboringStarDensity[index] < 1.0e-4F ||
                    tiles[index].starDensity > neighboringStarDensity[index] * 0.95F) {
                    continue;
                }
                bool adjacentDark = false;
                constexpr int offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& offset : offsets) {
                    const int neighborX = static_cast<int>(column) + offset[0];
                    const int neighborY = static_cast<int>(row) + offset[1];
                    if (neighborX >= 0 && neighborY >= 0 && neighborX < static_cast<int>(columns) &&
                        neighborY < static_cast<int>(rows) &&
                        darkCloudy[static_cast<std::size_t>(neighborY) * columns +
                                   static_cast<std::size_t>(neighborX)]) {
                        adjacentDark = true;
                        break;
                    }
                }
                if (adjacentDark) {
                    expandedDark[index] = 1;
                    changed = true;
                }
            }
        }
        darkCloudy.swap(expandedDark);
        if (!changed) {
            break;
        }
    }
    for (std::size_t index = 0; index < cloudy.size(); ++index) {
        if (darkCloudy[index]) {
            cloudy[index] = 1;
        }
    }

    std::vector<std::uint8_t> visited(tiles.size(), 0);
    CloudRemovalResult result;
    result.ok = true;
    result.image = image;
    result.usedMajorityFallback = usedMajorityFallback;

    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto start = static_cast<std::size_t>(row) * columns + column;
            if (!cloudy[start] || visited[start]) {
                continue;
            }
            const bool darkComponent = darkCloudy[start] != 0;

            std::queue<std::pair<std::uint32_t, std::uint32_t>> queue;
            queue.emplace(column, row);
            visited[start] = 1;
            std::uint32_t minColumn = column;
            std::uint32_t maxColumn = column;
            std::uint32_t minRow = row;
            std::uint32_t maxRow = row;
            float meanSum = 0.0F;
            float backgroundSum = 0.0F;
            float darkBackgroundSum = 0.0F;
            float starDensitySum = 0.0F;
            std::size_t count = 0;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> componentCells;

            while (!queue.empty()) {
                const auto [x, y] = queue.front();
                queue.pop();
                const auto index = static_cast<std::size_t>(y) * columns + x;
                meanSum += tiles[index].lowerLuminance;
                backgroundSum += backgroundSurface[index];
                darkBackgroundSum += darkCloudBackgroundSurface[index];
                starDensitySum += tiles[index].starDensity;
                ++count;
                componentCells.emplace_back(x, y);
                minColumn = std::min(minColumn, x);
                maxColumn = std::max(maxColumn, x);
                minRow = std::min(minRow, y);
                maxRow = std::max(maxRow, y);

                constexpr int offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& offset : offsets) {
                    const int nx = static_cast<int>(x) + offset[0];
                    const int ny = static_cast<int>(y) + offset[1];
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(columns) || ny >= static_cast<int>(rows)) {
                        continue;
                    }
                    const auto neighbor = static_cast<std::size_t>(ny) * columns + static_cast<std::size_t>(nx);
                    if (cloudy[neighbor] && !visited[neighbor] && (darkCloudy[neighbor] != 0) == darkComponent) {
                        visited[neighbor] = 1;
                        queue.emplace(static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny));
                    }
                }
            }

            const float coverage = static_cast<float>(count) / static_cast<float>(tiles.size());
            if (coverage < options.minCoverage) {
                continue;
            }
            const float mean = meanSum / static_cast<float>(count);
            const float localBackground = (darkComponent ? darkBackgroundSum : backgroundSum) / static_cast<float>(count);
            const float x = static_cast<float>(minColumn) * static_cast<float>(image.width) / static_cast<float>(columns);
            const float y = static_cast<float>(minRow) * static_cast<float>(image.height) / static_cast<float>(rows);
            const float right =
                static_cast<float>(maxColumn + 1) * static_cast<float>(image.width) / static_cast<float>(columns);
            const float bottom =
                static_cast<float>(maxRow + 1) * static_cast<float>(image.height) / static_cast<float>(rows);
            const float delta = std::abs(mean - localBackground);
            const float meanStarDensity = starDensitySum / static_cast<float>(count);
            const float starPenalty = std::clamp(meanStarDensity / std::max(0.002F, brightStarLimit * 2.5F), 0.0F, 1.0F);
            float contextStarDensity = 0.0F;
            std::size_t contextCount = 0;
            const std::uint32_t contextMinColumn = minColumn == 0 ? 0 : minColumn - 1;
            const std::uint32_t contextMaxColumn = std::min(columns - 1, maxColumn + 1);
            const std::uint32_t contextMinRow = minRow == 0 ? 0 : minRow - 1;
            const std::uint32_t contextMaxRow = std::min(rows - 1, maxRow + 1);
            for (std::uint32_t contextRow = contextMinRow; contextRow <= contextMaxRow; ++contextRow) {
                for (std::uint32_t contextColumn = contextMinColumn; contextColumn <= contextMaxColumn; ++contextColumn) {
                    contextStarDensity +=
                        tiles[static_cast<std::size_t>(contextRow) * columns + contextColumn].starDensity;
                    ++contextCount;
                }
            }
            contextStarDensity /= static_cast<float>(std::max<std::size_t>(1, contextCount));
            const float contextStarPenalty =
                std::clamp(contextStarDensity / std::max(0.0025F, brightStarLimit * 1.8F), 0.0F, 1.0F);
            // Very broad dark regions are generally vignetting or sky falloff,
            // not a selectable cloud bank.  Wide bright veils remain handled by
            // the majority fallback above.
            if (darkComponent && coverage > 0.08F) {
                continue;
            }
            const std::uint32_t componentColumns = maxColumn - minColumn + 1;
            const std::uint32_t componentRows = maxRow - minRow + 1;
            const float oneTileBandPenalty = std::min(componentColumns, componentRows) == 1 ? 0.28F : 0.0F;
            const float confidence = darkComponent
                                         ? std::clamp(
                                               delta / std::max(0.04F, localBackground + 0.02F) + coverage * 0.75F,
                                               0.0F,
                                               1.0F
                                           )
                                         : std::clamp(
                                               delta / std::max(0.05F, localBackground + 0.02F) + coverage * 1.25F -
                                                   starPenalty * 0.45F - contextStarPenalty * 0.38F - oneTileBandPenalty,
                                               0.0F,
                                               1.0F
                                           );
            result.regions.push_back({
                .index = result.regions.size(),
                .confidence = confidence,
                .x = x,
                .y = y,
                .width = std::max(1.0F, right - x),
                .height = std::max(1.0F, bottom - y),
                .coverage = coverage,
                .meanLuminance = mean,
                .backgroundLuminance = localBackground,
                .maskRects = makeMaskRects(std::move(componentCells), image, columns, rows),
            });
        }
    }

    std::sort(result.regions.begin(), result.regions.end(), [](const CloudRegion& lhs, const CloudRegion& rhs) {
        if (lhs.confidence != rhs.confidence) {
            return lhs.confidence > rhs.confidence;
        }
        if (lhs.y != rhs.y) {
            return lhs.y < rhs.y;
        }
        if (lhs.x != rhs.x) {
            return lhs.x < rhs.x;
        }
        if (lhs.height != rhs.height) {
            return lhs.height > rhs.height;
        }
        return lhs.width > rhs.width;
    });
    for (std::size_t i = 0; i < result.regions.size(); ++i) {
        result.regions[i].index = i;
    }
    return result;
}

CloudRemovalResult CloudRemoval::detectTemporal(
    const ImageBuffer& image,
    const std::vector<ImageBuffer>& referenceImages,
    const CloudRemovalOptions& options
) const {
    if (referenceImages.size() < 2) {
        return cloudError("ArgumentInvalid", "Temporal cloud detection requires at least two reference images");
    }
    for (const auto& reference : referenceImages) {
        if (!validImage(reference) || reference.width != image.width || reference.height != image.height ||
            reference.channels != image.channels || reference.colorEncoding != image.colorEncoding) {
            return cloudError(
                "ImageDimensionMismatch",
                "Temporal cloud detection requires reference images with matching dimensions, channels, and color encoding"
            );
        }
    }

    auto temporalOptions = options;
    temporalOptions.minBrightnessDelta = std::min(temporalOptions.minBrightnessDelta, 0.015F);
    std::vector<CloudRemovalResult> referenceDetections;
    referenceDetections.reserve(referenceImages.size());
    for (const auto& reference : referenceImages) {
        auto detection = detect(reference, temporalOptions);
        if (!detection.ok) {
            return detection;
        }
        referenceDetections.push_back(std::move(detection));
    }
    return detectTemporalFromDetections(image, referenceDetections, options);
}

CloudRemovalResult CloudRemoval::detectTemporalFromDetections(
    const ImageBuffer& image,
    const std::vector<CloudRemovalResult>& referenceDetections,
    const CloudRemovalOptions& options
) const {
    if (referenceDetections.size() < 2) {
        return cloudError("ArgumentInvalid", "Temporal cloud detection requires at least two reference detections");
    }
    if (const auto invalid = std::find_if(referenceDetections.begin(), referenceDetections.end(), [](const auto& detection) {
            return !detection.ok;
        }); invalid != referenceDetections.end()) {
        return cloudError("ArgumentInvalid", "Temporal cloud detection received an invalid reference detection");
    }

    auto result = detect(image, options);
    if (!result.ok) {
        return result;
    }

    // A slightly more sensitive first pass proposes weak dark banks.  A proposal
    // only joins the result when independently observed in neighboring frames,
    // so a static bright Milky Way structure cannot be introduced by relaxing
    // the single-frame threshold.  Reference reports are supplied separately
    // to let callers release full-resolution RAW buffers one at a time.
    auto temporalOptions = options;
    temporalOptions.minBrightnessDelta = std::min(temporalOptions.minBrightnessDelta, 0.015F);
    const auto weakTarget = detect(image, temporalOptions);
    if (!weakTarget.ok) {
        return weakTarget;
    }

    const auto overlap = [](const CloudRegion& lhs, const CloudRegion& rhs) {
        const float left = std::max(lhs.x, rhs.x);
        const float top = std::max(lhs.y, rhs.y);
        const float right = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
        const float bottom = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
        const float intersection = std::max(0.0F, right - left) * std::max(0.0F, bottom - top);
        const float unionArea = lhs.width * lhs.height + rhs.width * rhs.height - intersection;
        return unionArea > 1.0e-6F ? intersection / unionArea : 0.0F;
    };
    const std::size_t requiredSupport = std::max<std::size_t>(2, (referenceDetections.size() + 2) / 3);
    for (const auto& candidate : weakTarget.regions) {
        // Temporal assistance is deliberately limited to dark, star-obscuring
        // proposals. Bright diffuse structure is too readily confused with the
        // Milky Way even across a short fixed-camera sequence.
        if (candidate.meanLuminance >= candidate.backgroundLuminance) {
            continue;
        }
        std::size_t support = 0;
        for (const auto& detection : referenceDetections) {
            const bool matched = std::any_of(detection.regions.begin(), detection.regions.end(), [&](const auto& other) {
                return other.meanLuminance < other.backgroundLuminance && overlap(candidate, other) >= 0.10F;
            });
            support += matched ? 1U : 0U;
        }
        if (support < requiredSupport) {
            continue;
        }
        // Weak temporal proposals must occupy a coherent part of their bounds.
        // Sparse diagonal cell chains are commonly residual sky gradients or
        // star-field texture; allowing them through makes a selectable cloud
        // region that has no visually continuous veil.
        const float boundsCoverage = candidate.width * candidate.height /
                                     static_cast<float>(image.width * image.height);
        const float spatialCoherence = candidate.coverage / std::max(boundsCoverage, 1.0e-6F);
        if (spatialCoherence < 0.50F) {
            continue;
        }
        const auto existing = std::find_if(result.regions.begin(), result.regions.end(), [&](const auto& region) {
            return overlap(candidate, region) >= 0.50F;
        });
        if (existing != result.regions.end()) {
            if (existing->temporalSupport == 0) {
                ++result.temporallyConfirmedRegions;
            }
            existing->temporalSupport = std::max(existing->temporalSupport, support);
            existing->confidence = std::min(1.0F, existing->confidence + 0.035F * static_cast<float>(support));
            continue;
        }
        auto confirmed = candidate;
        confirmed.temporalSupport = support;
        confirmed.confidence = std::min(1.0F, confirmed.confidence + 0.035F * static_cast<float>(support));
        result.regions.push_back(std::move(confirmed));
        ++result.temporallyConfirmedRegions;
    }
    std::sort(result.regions.begin(), result.regions.end(), [](const CloudRegion& lhs, const CloudRegion& rhs) {
        if (lhs.confidence != rhs.confidence) {
            return lhs.confidence > rhs.confidence;
        }
        if (lhs.y != rhs.y) {
            return lhs.y < rhs.y;
        }
        return lhs.x < rhs.x;
    });
    for (std::size_t index = 0; index < result.regions.size(); ++index) {
        result.regions[index].index = index;
    }
    result.temporalFramesUsed = referenceDetections.size() + 1;
    return result;
}

CloudRemovalResult removeDetectedCloudRegions(
    const ImageBuffer& image,
    CloudRemovalResult result,
    const CloudRemovalOptions& options
) {
    std::unordered_set<std::size_t> selected(options.selectedIndices.begin(), options.selectedIndices.end());
    const bool removeAll = selected.empty();
    result.image = image;
    result.removedRegions = 0;
    const float strength = std::clamp(options.strength, 0.0F, 1.0F);
    if (result.regions.empty() || strength <= 0.0F) {
        return result;
    }

    const std::uint32_t columns = std::min(options.columns, image.width);
    const std::uint32_t rows = std::min(options.rows, image.height);
    const auto tiles = computeTiles(image, columns, rows);
    std::vector<float> selectedMask(tiles.size(), 0.0F);
    std::vector<float> detectedMask(tiles.size(), 0.0F);
    std::array<std::vector<float>, 3> tileColorLevels;
    std::array<std::vector<float>, 3> backgroundColorSurfaces;
    std::array<std::vector<float>, 3> selectedColorExcess;
    const float backgroundQuantile = result.usedMajorityFallback ? 0.10F : 0.25F;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        tileColorLevels[channel].reserve(tiles.size());
        selectedColorExcess[channel].assign(tiles.size(), 0.0F);
        for (const auto& tile : tiles) {
            tileColorLevels[channel].push_back(tile.lowerColor[channel]);
        }
    }

    for (const auto& region : result.regions) {
        for (const auto& rect : region.maskRects) {
            const auto minColumn = static_cast<std::uint32_t>(std::clamp(
                static_cast<int>(std::lround(rect.x * static_cast<float>(columns) / static_cast<float>(image.width))),
                0,
                static_cast<int>(columns) - 1
            ));
            const auto maxColumnExclusive = static_cast<std::uint32_t>(std::clamp(
                static_cast<int>(std::lround(
                    (rect.x + rect.width) * static_cast<float>(columns) / static_cast<float>(image.width)
                )),
                static_cast<int>(minColumn + 1),
                static_cast<int>(columns)
            ));
            const auto minRow = static_cast<std::uint32_t>(std::clamp(
                static_cast<int>(std::lround(rect.y * static_cast<float>(rows) / static_cast<float>(image.height))),
                0,
                static_cast<int>(rows) - 1
            ));
            const auto maxRowExclusive = static_cast<std::uint32_t>(std::clamp(
                static_cast<int>(std::lround(
                    (rect.y + rect.height) * static_cast<float>(rows) / static_cast<float>(image.height)
                )),
                static_cast<int>(minRow + 1),
                static_cast<int>(rows)
            ));
            for (std::uint32_t row = minRow; row < maxRowExclusive; ++row) {
                for (std::uint32_t column = minColumn; column < maxColumnExclusive; ++column) {
                    const auto index = static_cast<std::size_t>(row) * columns + column;
                    if (tiles[index].sampleWeight <= 1.0e-8) {
                        continue;
                    }
                    detectedMask[index] = 1.0F;
                    if (removeAll || selected.count(region.index) != 0) {
                        selectedMask[index] = 1.0F;
                    }
                }
            }
        }
        if (removeAll || selected.count(region.index) != 0) {
            ++result.removedRegions;
        }
    }

    if (result.removedRegions == 0) {
        return result;
    }

    for (std::size_t channel = 0; channel < 3; ++channel) {
        backgroundColorSurfaces[channel] = estimateUnobscuredBackgroundSurface(
            tileColorLevels[channel], detectedMask, columns, rows, backgroundQuantile
        );
        for (std::size_t index = 0; index < tiles.size(); ++index) {
            if (selectedMask[index] > 0.5F) {
                selectedColorExcess[channel][index] =
                    tileColorLevels[channel][index] - backgroundColorSurfaces[channel][index];
            }
        }
    }

    const float tileWidth = static_cast<float>(image.width) / static_cast<float>(columns);
    const float tileHeight = static_cast<float>(image.height) / static_cast<float>(rows);
    const float featherFraction = std::clamp(
        std::max(0.0F, options.featherRadius) / std::max(1.0F, std::min(tileWidth, tileHeight)),
        0.0F,
        1.0F
    );
    const float featherHalfWidth = featherFraction * 0.5F;

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            const float py = static_cast<float>(y) + 0.5F;
            const float linearMask = sampleTileField(selectedMask, columns, rows, image, px, py);
            const float alpha = featherFraction <= 0.001F
                                    ? (linearMask >= 0.5F ? 1.0F : 0.0F)
                                    : smoothstep(0.5F - featherHalfWidth, 0.5F + featherHalfWidth, linearMask);
            if (alpha <= 0.0F || linearMask <= 1.0e-6F) {
                continue;
            }

            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            if (validPixelCoverage(image, offset) <= 1.0e-6F) {
                continue;
            }
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float weightedExcess =
                    sampleTileField(selectedColorExcess[channel], columns, rows, image, px, py);
                const float localExcess = weightedExcess / linearMask;
                const float localBackground =
                    sampleTileField(backgroundColorSurfaces[channel], columns, rows, image, px, py);
                if (localExcess >= 0.0F) {
                    const float availableHaze = std::max(0.0F, image.pixels[offset + channel] - localBackground);
                    const float correction = std::min(localExcess * strength * alpha, availableHaze * 0.98F);
                    result.image.pixels[offset + channel] =
                        std::clamp(image.pixels[offset + channel] - correction, 0.0F, 1.0F);
                } else {
                    // Opaque cloud cannot restore hidden stars, but lifting the
                    // smoothly estimated background removes its dark veil without
                    // overshooting the local clear-sky reference.
                    const float availableRecovery = std::max(0.0F, localBackground - image.pixels[offset + channel]);
                    const float correction = std::min(-localExcess * strength * alpha, availableRecovery * 0.98F);
                    result.image.pixels[offset + channel] =
                        std::clamp(image.pixels[offset + channel] + correction, 0.0F, 1.0F);
                }
            }
        }
    }
    return result;
}

CloudRemovalResult CloudRemoval::remove(const ImageBuffer& image, const CloudRemovalOptions& options) const {
    auto result = detect(image, options);
    if (!result.ok) {
        return result;
    }
    if (std::any_of(options.selectedIndices.begin(), options.selectedIndices.end(), [&](std::size_t index) {
            return index >= result.regions.size();
        })) {
        return cloudError("ArgumentInvalid", "Selected cloud region index is out of range");
    }
    return removeDetectedCloudRegions(image, std::move(result), options);
}

CloudRemovalResult CloudRemoval::removeTemporal(
    const ImageBuffer& image,
    const std::vector<ImageBuffer>& referenceImages,
    const CloudRemovalOptions& options
) const {
    auto result = detectTemporal(image, referenceImages, options);
    if (!result.ok) {
        return result;
    }
    if (std::any_of(options.selectedIndices.begin(), options.selectedIndices.end(), [&](std::size_t index) {
            return index >= result.regions.size();
        })) {
        return cloudError("ArgumentInvalid", "Selected cloud region index is out of range");
    }
    return removeDetectedCloudRegions(image, std::move(result), options);
}

CloudRemovalResult CloudRemoval::removeTemporalFromDetections(
    const ImageBuffer& image,
    const std::vector<CloudRemovalResult>& referenceDetections,
    const CloudRemovalOptions& options
) const {
    auto result = detectTemporalFromDetections(image, referenceDetections, options);
    if (!result.ok) {
        return result;
    }
    if (std::any_of(options.selectedIndices.begin(), options.selectedIndices.end(), [&](std::size_t index) {
            return index >= result.regions.size();
        })) {
        return cloudError("ArgumentInvalid", "Selected cloud region index is out of range");
    }
    return removeDetectedCloudRegions(image, std::move(result), options);
}

} // namespace photonstack
