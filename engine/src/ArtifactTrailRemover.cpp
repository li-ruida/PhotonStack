#include "photonstack/ArtifactTrailRemover.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace photonstack {
namespace {

ArtifactTrailResult artifactError(std::string code, std::string message) {
    ArtifactTrailResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

template <typename Callback>
void parallelForRowRanges(std::uint32_t rowCount, Callback callback) {
    const unsigned int availableWorkers = std::max(1U, std::thread::hardware_concurrency());
    const unsigned int workerCount = rowCount < 512
                                         ? 1U
                                         : std::min<unsigned int>(availableWorkers, rowCount);
    if (workerCount == 1) {
        callback(0, rowCount);
        return;
    }

    const std::uint32_t rowsPerWorker = (rowCount + workerCount - 1) / workerCount;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned int worker = 0; worker < workerCount; ++worker) {
        const std::uint32_t begin = std::min(rowCount, static_cast<std::uint32_t>(worker) * rowsPerWorker);
        const std::uint32_t end = std::min(rowCount, begin + rowsPerWorker);
        if (begin >= end) {
            break;
        }
        workers.emplace_back([=, &callback]() { callback(begin, end); });
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

class ArtifactTrailProgressReporter {
  public:
    explicit ArtifactTrailProgressReporter(const ArtifactTrailProgressCallback& callback)
        : callback_(callback) {}

    void report(ArtifactTrailProgressStage stage, double progress, std::size_t item = 0,
                std::size_t itemCount = 0, std::uint32_t row = 0, std::uint32_t rowCount = 0) {
        if (!callback_) {
            return;
        }
        const double nextProgress = std::max(lastProgress_, std::clamp(progress, 0.0, 1.0));
        const bool stageChanged = !hasLastStage_ || stage != lastStage_;
        const bool completedItemGroup = itemCount > 0 && item == itemCount;
        const bool completedRows = rowCount > 0 && row == rowCount;
        constexpr double minimumProgressDelta = 0.0025;
        if (!stageChanged && !completedItemGroup && !completedRows && nextProgress < 1.0 &&
            nextProgress - lastProgress_ < minimumProgressDelta) {
            return;
        }
        lastProgress_ = nextProgress;
        lastStage_ = stage;
        hasLastStage_ = true;
        callback_({
            .stage = stage,
            .progress = lastProgress_,
            .item = item,
            .itemCount = itemCount,
            .row = row,
            .rowCount = rowCount,
        });
    }

    void reportRow(ArtifactTrailProgressStage stage, std::uint32_t row, std::uint32_t rowCount,
                   double start, double end) {
        const auto completed = row + 1;
        const auto interval = std::max<std::uint32_t>(1, rowCount / 100);
        if (completed != rowCount && completed % interval != 0) {
            return;
        }
        const double fraction = rowCount == 0 ? 1.0 : static_cast<double>(completed) / rowCount;
        report(stage, start + (end - start) * fraction, 0, 0, completed, rowCount);
    }

  private:
    const ArtifactTrailProgressCallback& callback_;
    double lastProgress_ = 0.0;
    ArtifactTrailProgressStage lastStage_ = ArtifactTrailProgressStage::Analyzing;
    bool hasLastStage_ = false;
};

struct TrailMaskMetadata {
    float alpha = 0.0F;
    float unitX = 1.0F;
    float unitY = 0.0F;
    float signedDistance = 0.0F;
};

using TrailMaskMetadataMap = std::unordered_map<std::size_t, TrailMaskMetadata>;

const TrailMaskMetadata& trailMaskMetadataAt(const TrailMaskMetadataMap& metadata, std::size_t pixel) {
    static const TrailMaskMetadata empty;
    const auto found = metadata.find(pixel);
    return found == metadata.end() ? empty : found->second;
}

float luminanceAt(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    if (image.channels < 3) {
        return image.pixels[offset];
    }
    return image.pixels[offset] * 0.2126F + image.pixels[offset + 1] * 0.7152F + image.pixels[offset + 2] * 0.0722F;
}

float colorVarianceAt(const ImageBuffer& image, std::size_t pixel) {
    if (image.channels < 3) {
        return 0.0F;
    }
    const auto offset = pixel * image.channels;
    const float mean = (image.pixels[offset] + image.pixels[offset + 1] + image.pixels[offset + 2]) / 3.0F;
    const float dr = image.pixels[offset] - mean;
    const float dg = image.pixels[offset + 1] - mean;
    const float db = image.pixels[offset + 2] - mean;
    return (dr * dr + dg * dg + db * db) / 3.0F;
}

float warmExcessAt(const ImageBuffer& image, std::size_t pixel) {
    if (image.channels < 3) {
        return 0.0F;
    }
    const auto offset = pixel * image.channels;
    return std::max(0.0F, image.pixels[offset] - (image.pixels[offset + 1] + image.pixels[offset + 2]) * 0.5F);
}

float navigationLightScoreAt(const ImageBuffer& image, std::size_t pixel) {
    if (image.channels < 3) {
        return 0.0F;
    }
    const auto offset = pixel * image.channels;
    const float red = image.pixels[offset];
    const float green = image.pixels[offset + 1];
    const float blue = image.pixels[offset + 2];
    const float redOrAmber = std::max(0.0F, red - std::max(green, blue) * 1.15F);
    const float greenBeacon = std::max(0.0F, green - std::max(red, blue) * 1.15F);
    const float blueBeacon = std::max(0.0F, blue - std::max(red, green) * 1.12F);
    const float cyanBeacon = std::max(0.0F, std::min(green, blue) - red * 1.20F);
    const float amber = std::max(0.0F, std::min(red, green) - blue * 1.25F);
    return std::max({redOrAmber, greenBeacon * 0.85F, blueBeacon * 0.65F}) + cyanBeacon * 0.35F + amber * 0.35F;
}

float blinkingLightScoreAt(const ImageBuffer& image, std::size_t pixel) {
    const float navigationScore = navigationLightScoreAt(image, pixel);
    const float chromaScore = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
    return navigationScore + chromaScore * 0.42F;
}

struct Component {
    std::vector<std::size_t> pixels;
};

struct Interval {
    float minProjection = 0.0F;
    float maxProjection = 0.0F;
};

struct ArtifactPoint {
    float x = 0.0F;
    float y = 0.0F;
    float signal = 0.0F;
    float colorScore = 0.0F;
    float radius = 1.0F;
};

bool isDuplicateTrail(const std::vector<ArtifactTrail>& trails, const ArtifactTrail& candidate);
float angleDelta(float a, float b);
bool trailsOverlap(const ArtifactTrail& a, const ArtifactTrail& b);

std::vector<Component> connectedComponents(const ImageBuffer& image, const std::vector<float>& luminance, float threshold) {
    std::vector<Component> components;
    std::vector<std::uint8_t> visited(image.pixelCount(), 0);
    std::queue<std::size_t> pending;

    const auto pushNeighbor = [&](std::int32_t x, std::int32_t y, Component& component) {
        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) || y >= static_cast<std::int32_t>(image.height)) {
            return;
        }
        const auto index = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                           static_cast<std::uint32_t>(x);
        if (visited[index] || luminance[index] < threshold) {
            return;
        }
        visited[index] = 1;
        pending.push(index);
        component.pixels.push_back(index);
    };

    for (std::size_t index = 0; index < luminance.size(); ++index) {
        if (visited[index] || luminance[index] < threshold) {
            continue;
        }

        Component component;
        visited[index] = 1;
        pending.push(index);
        component.pixels.push_back(index);
        while (!pending.empty()) {
            const auto current = pending.front();
            pending.pop();
            const auto x = static_cast<std::int32_t>(current % image.width);
            const auto y = static_cast<std::int32_t>(current / image.width);
            for (std::int32_t dy = -1; dy <= 1; ++dy) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    pushNeighbor(x + dx, y + dy, component);
                }
            }
        }
        components.push_back(std::move(component));
    }
    return components;
}

std::vector<Component> connectedComponentsByScore(const ImageBuffer& image,
                                                  const std::vector<float>& score,
                                                  float threshold) {
    std::vector<Component> components;
    std::vector<std::uint8_t> visited(image.pixelCount(), 0);
    std::queue<std::size_t> pending;

    const auto pushNeighbor = [&](std::int32_t x, std::int32_t y, Component& component) {
        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) || y >= static_cast<std::int32_t>(image.height)) {
            return;
        }
        const auto index = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                           static_cast<std::uint32_t>(x);
        if (visited[index] || score[index] < threshold) {
            return;
        }
        visited[index] = 1;
        pending.push(index);
        component.pixels.push_back(index);
    };

    for (std::size_t index = 0; index < score.size(); ++index) {
        if (visited[index] || score[index] < threshold) {
            continue;
        }

        Component component;
        visited[index] = 1;
        pending.push(index);
        component.pixels.push_back(index);
        while (!pending.empty()) {
            const auto current = pending.front();
            pending.pop();
            const auto x = static_cast<std::int32_t>(current % image.width);
            const auto y = static_cast<std::int32_t>(current / image.width);
            for (std::int32_t dy = -1; dy <= 1; ++dy) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    pushNeighbor(x + dx, y + dy, component);
                }
            }
        }
        components.push_back(std::move(component));
    }
    return components;
}

bool compactColoredPoint(const ImageBuffer& image,
                         const std::vector<float>& luminance,
                         const Component& component,
                         float background,
                         ArtifactPoint& point) {
    if (component.pixels.empty() || component.pixels.size() > 80) {
        return false;
    }

    double weightSum = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;
    double colorSum = 0.0;
    double signalSum = 0.0;
    std::uint32_t minX = image.width;
    std::uint32_t minY = image.height;
    std::uint32_t maxX = 0;
    std::uint32_t maxY = 0;

    for (const auto pixel : component.pixels) {
        const auto x = static_cast<std::uint32_t>(pixel % image.width);
        const auto y = static_cast<std::uint32_t>(pixel / image.width);
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        const float signal = std::max(0.0F, luminance[pixel] - background);
        const float colorScore = blinkingLightScoreAt(image, pixel);
        const double weight = std::max(0.0001F, signal + colorScore * 2.0F);
        weightSum += weight;
        sumX += static_cast<double>(x) * weight;
        sumY += static_cast<double>(y) * weight;
        colorSum += colorScore * weight;
        signalSum += signal * weight;
    }

    if (weightSum <= 0.0) {
        return false;
    }

    const float width = static_cast<float>(maxX - minX + 1);
    const float height = static_cast<float>(maxY - minY + 1);
    const float maxDimension = std::max(width, height);
    const float minDimension = std::max(1.0F, std::min(width, height));
    if (maxDimension > 14.0F || maxDimension / minDimension > 5.5F) {
        return false;
    }

    point.x = static_cast<float>(sumX / weightSum);
    point.y = static_cast<float>(sumY / weightSum);
    point.signal = static_cast<float>(signalSum / weightSum);
    point.colorScore = static_cast<float>(colorSum / weightSum);
    point.radius = std::max(1.0F, maxDimension * 0.5F);

    return point.colorScore > 0.010F && (point.signal > 0.004F || point.colorScore > 0.016F);
}

void appendNavigationPeakPoints(const ImageBuffer& image,
                                const std::vector<float>& luminance,
                                const std::vector<float>& navigationScores,
                                float threshold,
                                float background,
                                std::vector<ArtifactPoint>& points) {
    constexpr std::uint32_t cellSize = 6;
    constexpr std::int32_t neighborhoodRadius = 2;
    constexpr std::int32_t duplicateCellSize = 16;
    const auto duplicateCellKey = [](std::int32_t column, std::int32_t row) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(row)) << 32U) |
               static_cast<std::uint32_t>(column);
    };
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> pointGrid;
    pointGrid.reserve(points.size() * 2 + 64);
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto column = static_cast<std::int32_t>(points[index].x) / duplicateCellSize;
        const auto row = static_cast<std::int32_t>(points[index].y) / duplicateCellSize;
        pointGrid[duplicateCellKey(column, row)].push_back(index);
    }
    for (std::uint32_t y0 = 0; y0 < image.height; y0 += cellSize) {
        const auto yEnd = std::min(image.height, y0 + cellSize);
        for (std::uint32_t x0 = 0; x0 < image.width; x0 += cellSize) {
            const auto xEnd = std::min(image.width, x0 + cellSize);
            float bestScore = threshold;
            std::uint32_t bestX = x0;
            std::uint32_t bestY = y0;
            bool found = false;
            for (std::uint32_t y = y0; y < yEnd; ++y) {
                const auto row = static_cast<std::size_t>(y) * image.width;
                for (std::uint32_t x = x0; x < xEnd; ++x) {
                    const float score = navigationScores[row + x];
                    if (score >= bestScore) {
                        bestScore = score;
                        bestX = x;
                        bestY = y;
                        found = true;
                    }
                }
            }
            if (!found) {
                continue;
            }

            bool isLocalMaximum = true;
            for (std::int32_t dy = -neighborhoodRadius; dy <= neighborhoodRadius && isLocalMaximum; ++dy) {
                const auto y = std::clamp(
                    static_cast<std::int32_t>(bestY) + dy,
                    0,
                    static_cast<std::int32_t>(image.height) - 1
                );
                const auto row = static_cast<std::size_t>(y) * image.width;
                for (std::int32_t dx = -neighborhoodRadius; dx <= neighborhoodRadius; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    const auto x = std::clamp(
                        static_cast<std::int32_t>(bestX) + dx,
                        0,
                        static_cast<std::int32_t>(image.width) - 1
                    );
                    if (navigationScores[row + static_cast<std::uint32_t>(x)] > bestScore) {
                        isLocalMaximum = false;
                        break;
                    }
                }
            }
            if (!isLocalMaximum) {
                continue;
            }

            Component component;
            component.pixels.reserve(25);
            const float componentThreshold = threshold * 0.72F;
            for (std::int32_t dy = -neighborhoodRadius; dy <= neighborhoodRadius; ++dy) {
                const auto y = static_cast<std::int32_t>(bestY) + dy;
                if (y < 0 || y >= static_cast<std::int32_t>(image.height)) {
                    continue;
                }
                const auto row = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width;
                for (std::int32_t dx = -neighborhoodRadius; dx <= neighborhoodRadius; ++dx) {
                    const auto x = static_cast<std::int32_t>(bestX) + dx;
                    if (x < 0 || x >= static_cast<std::int32_t>(image.width)) {
                        continue;
                    }
                    const auto pixel = row + static_cast<std::uint32_t>(x);
                    if (navigationScores[pixel] >= componentThreshold) {
                        component.pixels.push_back(pixel);
                    }
                }
            }
            ArtifactPoint point;
            if (compactColoredPoint(image, luminance, component, background, point)) {
                const auto pointColumn = static_cast<std::int32_t>(point.x) / duplicateCellSize;
                const auto pointRow = static_cast<std::int32_t>(point.y) / duplicateCellSize;
                bool duplicate = false;
                for (std::int32_t rowOffset = -1; rowOffset <= 1 && !duplicate; ++rowOffset) {
                    const auto candidateRow = pointRow + rowOffset;
                    if (candidateRow < 0) {
                        continue;
                    }
                    for (std::int32_t columnOffset = -1; columnOffset <= 1 && !duplicate; ++columnOffset) {
                        const auto candidateColumn = pointColumn + columnOffset;
                        if (candidateColumn < 0) {
                            continue;
                        }
                        const auto found = pointGrid.find(duplicateCellKey(candidateColumn, candidateRow));
                        if (found == pointGrid.end()) {
                            continue;
                        }
                        duplicate = std::any_of(found->second.begin(), found->second.end(), [&](std::size_t index) {
                            const auto& existing = points[index];
                            const float dx = existing.x - point.x;
                            const float dy = existing.y - point.y;
                            return dx * dx + dy * dy <
                                   std::pow(std::max(existing.radius, point.radius) + 2.0F, 2.0F);
                        });
                    }
                }
                if (!duplicate) {
                    const auto index = points.size();
                    points.push_back(point);
                    pointGrid[duplicateCellKey(pointColumn, pointRow)].push_back(index);
                }
            }
        }
    }
}

void limitBlinkingPointsByTiles(std::vector<ArtifactPoint>& points,
                                std::size_t maximumPoints,
                                std::uint32_t imageWidth,
                                std::uint32_t imageHeight) {
    if (points.size() <= maximumPoints) {
        return;
    }

    constexpr std::uint32_t gridColumns = 8;
    constexpr std::uint32_t gridRows = 6;
    constexpr std::size_t tileCount = gridColumns * gridRows;
    const std::size_t perTileLimit = std::max<std::size_t>(8, maximumPoints / tileCount);
    const auto pointScore = [](const ArtifactPoint& point) {
        return point.colorScore * 2.5F + point.signal;
    };

    std::vector<std::vector<std::size_t>> tileIndices(tileCount);
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto column = std::min<std::uint32_t>(
            gridColumns - 1,
            static_cast<std::uint32_t>(std::clamp(points[index].x, 0.0F, static_cast<float>(imageWidth - 1)) /
                                       std::max(1.0F, static_cast<float>(imageWidth)) *
                                       static_cast<float>(gridColumns))
        );
        const auto row = std::min<std::uint32_t>(
            gridRows - 1,
            static_cast<std::uint32_t>(std::clamp(points[index].y, 0.0F, static_cast<float>(imageHeight - 1)) /
                                       std::max(1.0F, static_cast<float>(imageHeight)) *
                                       static_cast<float>(gridRows))
        );
        tileIndices[static_cast<std::size_t>(row) * gridColumns + column].push_back(index);
    }

    std::vector<std::uint8_t> selected(points.size(), 0);
    std::vector<ArtifactPoint> limited;
    limited.reserve(maximumPoints);
    for (auto& indices : tileIndices) {
        std::sort(indices.begin(), indices.end(), [&](std::size_t left, std::size_t right) {
            return pointScore(points[left]) > pointScore(points[right]);
        });
        const std::size_t count = std::min(perTileLimit, indices.size());
        for (std::size_t i = 0; i < count && limited.size() < maximumPoints; ++i) {
            selected[indices[i]] = 1;
            limited.push_back(points[indices[i]]);
        }
    }

    std::vector<std::size_t> remaining;
    remaining.reserve(points.size());
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (!selected[index]) {
            remaining.push_back(index);
        }
    }
    std::sort(remaining.begin(), remaining.end(), [&](std::size_t left, std::size_t right) {
        return pointScore(points[left]) > pointScore(points[right]);
    });
    for (const auto index : remaining) {
        if (limited.size() >= maximumPoints) {
            break;
        }
        limited.push_back(points[index]);
    }

    points = std::move(limited);
}

bool hasDistinctPointCorridorSupport(const std::vector<ArtifactPoint>& points,
                                     const ImageBuffer& image,
                                     float unitX,
                                     float unitY,
                                     float minimumProjection,
                                     float maximumProjection,
                                     float centerNormal,
                                     float halfWidth) {
    const float span = maximumProjection - minimumProjection;
    if (points.empty() || span <= 0.0F || halfWidth <= 0.0F) {
        return false;
    }

    const float effectiveHalfWidth = halfWidth + 2.5F;
    const float corridorArea = span * effectiveHalfWidth * 2.0F;
    const float imageArea = static_cast<float>(image.width) * static_cast<float>(image.height);
    const float globallyExpectedPoints =
        static_cast<float>(points.size()) * corridorArea / std::max(1.0F, imageArea);
    if (globallyExpectedPoints < 1.25F) {
        return true;
    }
    float globalPointScore = 0.0F;
    for (const auto& point : points) {
        globalPointScore += std::max(0.001F, point.signal + point.colorScore * 1.5F);
    }
    const float globallyExpectedScore = globalPointScore * corridorArea / std::max(1.0F, imageArea);

    const float normalX = -unitY;
    const float normalY = unitX;
    struct CorridorEvidence {
        float count = 0.0F;
        float score = 0.0F;
        float visibleFraction = 0.0F;
    };
    const auto measureCorridor = [&](float normalOffset) {
        CorridorEvidence evidence;
        constexpr int visibilitySamples = 17;
        for (int sample = 0; sample < visibilitySamples; ++sample) {
            const float projection = minimumProjection +
                                     span * static_cast<float>(sample) /
                                         static_cast<float>(visibilitySamples - 1);
            const float normal = centerNormal + normalOffset;
            const float x = unitX * projection + normalX * normal;
            const float y = unitY * projection + normalY * normal;
            if (x >= 0.0F && y >= 0.0F && x < static_cast<float>(image.width) &&
                y < static_cast<float>(image.height)) {
                evidence.visibleFraction += 1.0F;
            }
        }
        evidence.visibleFraction /= static_cast<float>(visibilitySamples);

        for (const auto& point : points) {
            const float projection = point.x * unitX + point.y * unitY;
            if (projection < minimumProjection || projection > maximumProjection) {
                continue;
            }
            const float normal = point.x * normalX + point.y * normalY;
            const float pointTolerance = halfWidth + std::min(point.radius, 2.5F);
            if (std::fabs(normal - (centerNormal + normalOffset)) > pointTolerance) {
                continue;
            }
            evidence.count += 1.0F;
            evidence.score += std::max(0.001F, point.signal + point.colorScore * 1.5F);
        }
        return evidence;
    };

    const auto centerEvidence = measureCorridor(0.0F);
    std::vector<float> sideCounts;
    std::vector<float> sideScores;
    for (const float multiplier : {-7.0F, -5.0F, -3.0F, 3.0F, 5.0F, 7.0F}) {
        const auto side = measureCorridor(halfWidth * multiplier);
        if (side.visibleFraction + 0.05F < centerEvidence.visibleFraction || side.visibleFraction < 0.55F) {
            continue;
        }
        sideCounts.push_back(side.count);
        sideScores.push_back(side.score);
    }
    if (sideCounts.size() < 2) {
        return true;
    }

    const auto medianValue = [](std::vector<float>& values) {
        const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::nth_element(values.begin(), middle, values.end());
        return *middle;
    };
    const float expectedCount = std::max(globallyExpectedPoints, medianValue(sideCounts));
    const float expectedScore = std::max(globallyExpectedScore, medianValue(sideScores));
    const float countExcess = centerEvidence.count - expectedCount;
    const float countSignificance = countExcess / std::sqrt(expectedCount + 1.0F);
    const bool distinctCount = countExcess >= 4.0F && countSignificance >= 4.6F &&
                               centerEvidence.count >= expectedCount * 1.45F + 3.0F;
    const bool distinctScore = centerEvidence.score >= expectedScore * 1.45F + 0.020F;
    return distinctCount && (distinctScore || countSignificance >= 5.8F);
}

std::vector<ArtifactTrail> detectVotedBlinkingPointTrails(const std::vector<ArtifactPoint>& points,
                                                          const ImageBuffer& image,
                                                          const ArtifactTrailOptions& options) {
    std::vector<ArtifactTrail> trails;
    if (points.size() < 8) {
        return trails;
    }

    const float imageDiagonal = std::sqrt(static_cast<float>(image.width) * static_cast<float>(image.width) +
                                          static_cast<float>(image.height) * static_cast<float>(image.height));
    const float maxBlinkingSpan = std::clamp(imageDiagonal * 0.11F, options.airplaneLength * 8.0F, 820.0F);
    constexpr float pi = 3.14159265358979323846F;
    constexpr float angleStep = pi / 36.0F;
    constexpr float normalBucketSize = 9.0F;
    const float clusterGap = std::clamp(options.airplaneLength * 2.5F, 82.0F, 132.0F);

    for (float angle = -pi * 0.5F; angle < pi * 0.5F; angle += angleStep) {
        const float unitX = std::cos(angle);
        const float unitY = std::sin(angle);
        const float normalX = -unitY;
        const float normalY = unitX;

        std::map<int, std::vector<std::size_t>> buckets;
        for (std::size_t index = 0; index < points.size(); ++index) {
            const auto& point = points[index];
            const int bucket = static_cast<int>(std::lround((point.x * normalX + point.y * normalY) / normalBucketSize));
            buckets[bucket].push_back(index);
        }

        for (auto& [_, bucket] : buckets) {
            if (bucket.size() < 8) {
                continue;
            }
            std::sort(bucket.begin(), bucket.end(), [&](std::size_t left, std::size_t right) {
                return points[left].x * unitX + points[left].y * unitY <
                       points[right].x * unitX + points[right].y * unitY;
            });

            std::vector<std::size_t> cluster;
            cluster.reserve(bucket.size());
            auto flushCluster = [&]() {
                if (cluster.size() < 8) {
                    cluster.clear();
                    return;
                }

                float minProjection = std::numeric_limits<float>::max();
                float maxProjection = std::numeric_limits<float>::lowest();
                float weightedNormal = 0.0F;
                float totalWeight = 0.0F;
                float colorSupport = 0.0F;
                for (const auto index : cluster) {
                    const auto& point = points[index];
                    const float projection = point.x * unitX + point.y * unitY;
                    const float normal = point.x * normalX + point.y * normalY;
                    minProjection = std::min(minProjection, projection);
                    maxProjection = std::max(maxProjection, projection);
                    const float weight = std::max(0.001F, point.signal + point.colorScore * 4.0F);
                    weightedNormal += normal * weight;
                    totalWeight += weight;
                    colorSupport += point.colorScore;
                }

                const float span = maxProjection - minProjection;
                if (span < options.airplaneLength * 3.0F || span > maxBlinkingSpan) {
                    cluster.clear();
                    return;
                }

                float largestGap = 0.0F;
                float gapSum = 0.0F;
                float gapSquaredSum = 0.0F;
                for (std::size_t i = 1; i < cluster.size(); ++i) {
                    const float previous = points[cluster[i - 1]].x * unitX + points[cluster[i - 1]].y * unitY;
                    const float current = points[cluster[i]].x * unitX + points[cluster[i]].y * unitY;
                    const float gap = current - previous;
                    largestGap = std::max(largestGap, gap);
                    gapSum += gap;
                    gapSquaredSum += gap * gap;
                }
                const float gapCount = static_cast<float>(cluster.size() - 1);
                const float averageGap = gapSum / std::max(1.0F, gapCount);
                const float gapVariance =
                    std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
                const float gapRegularity =
                    1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
                if (largestGap > std::min(125.0F, std::max(56.0F, span * 0.32F)) || gapRegularity < 0.12F) {
                    cluster.clear();
                    return;
                }

                const float averageColorSupport = colorSupport / static_cast<float>(cluster.size());
                if (averageColorSupport < 0.012F) {
                    cluster.clear();
                    return;
                }

                const float normal = weightedNormal / std::max(totalWeight, 0.001F);
                if (!hasDistinctPointCorridorSupport(points,
                                                     image,
                                                     unitX,
                                                     unitY,
                                                     minProjection,
                                                     maxProjection,
                                                     normal,
                                                     normalBucketSize * 0.55F)) {
                    cluster.clear();
                    return;
                }

                ArtifactTrail trail;
                trail.x1 = unitX * minProjection + normalX * normal;
                trail.y1 = unitY * minProjection + normalY * normal;
                trail.x2 = unitX * maxProjection + normalX * normal;
                trail.y2 = unitY * maxProjection + normalY * normal;
                trail.length = span;
                trail.width = 4.0F;
                trail.angleRadians = angle;
                trail.peakPosition = 0.5F;
                trail.taperScore = 0.0F;
                trail.colorVariance = averageColorSupport;
                trail.warmEvidence = averageColorSupport;
                trail.kind = ArtifactTrailKind::Drone;
                trail.confidence = std::clamp(
                    0.30F + std::min(span, 520.0F) / 900.0F + std::min<std::size_t>(cluster.size(), 18) * 0.014F +
                        averageColorSupport * 1.6F + gapRegularity * 0.16F,
                    0.0F,
                    averageColorSupport > 0.020F ? 0.88F : 0.76F
                );

                if (!isDuplicateTrail(trails, trail)) {
                    trails.push_back(trail);
                }
                cluster.clear();
            };

            cluster.push_back(bucket.front());
            for (std::size_t i = 1; i < bucket.size(); ++i) {
                const float previous = points[bucket[i - 1]].x * unitX + points[bucket[i - 1]].y * unitY;
                const float current = points[bucket[i]].x * unitX + points[bucket[i]].y * unitY;
                if (current - previous > clusterGap) {
                    flushCluster();
                }
                cluster.push_back(bucket[i]);
            }
            flushCluster();
        }
    }

    return trails;
}

std::vector<ArtifactTrail> detectSparseHorizontalBlinkingTrails(const ImageBuffer& image,
                                                                const std::vector<float>& luminance,
                                                                const std::vector<float>& navigationScores,
                                                                const ArtifactTrailOptions& options) {
    std::vector<ArtifactPoint> points;
    std::vector<ArtifactPoint> periodicPoints;
    if (image.width < 128 || image.height < 128 || navigationScores.size() != image.pixelCount()) {
        return {};
    }

    struct CellCandidate {
        float score = 0.0F;
        float contrast = 0.0F;
        float localMeanLuminance = 0.0F;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        bool valid = false;
    };

    constexpr std::uint32_t cellSize = 8;
    const std::uint32_t startY = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.74F);
    const std::uint32_t columns = (image.width + cellSize - 1) / cellSize;
    const std::uint32_t rows = (image.height + cellSize - 1) / cellSize;
    const std::uint32_t startRow = std::min(rows - 1, startY / cellSize);
    std::vector<CellCandidate> candidates(static_cast<std::size_t>(columns) * rows);

    for (std::uint32_t row = startRow; row < rows; ++row) {
        const std::uint32_t y0 = row * cellSize;
        for (std::uint32_t column = 0; column < columns; ++column) {
            const std::uint32_t x0 = column * cellSize;
            float scoreSum = 0.0F;
            float luminanceSum = 0.0F;
            std::size_t count = 0;
            float bestScore = 0.0F;
            float bestLuminance = 0.0F;
            std::uint32_t bestX = x0;
            std::uint32_t bestY = y0;
            std::uint32_t bestLuminanceX = x0;
            std::uint32_t bestLuminanceY = y0;
            for (std::uint32_t y = y0; y < std::min<std::uint32_t>(y0 + cellSize, image.height); ++y) {
                for (std::uint32_t x = x0; x < std::min<std::uint32_t>(x0 + cellSize, image.width); ++x) {
                    const auto pixel = static_cast<std::size_t>(y) * image.width + x;
                    const float score = navigationScores[pixel];
                    const float pixelLuminance = luminance[pixel];
                    scoreSum += score;
                    luminanceSum += pixelLuminance;
                    count += 1;
                    if (score > bestScore) {
                        bestScore = score;
                        bestX = x;
                        bestY = y;
                    }
                    if (pixelLuminance > bestLuminance) {
                        bestLuminance = pixelLuminance;
                        bestLuminanceX = x;
                        bestLuminanceY = y;
                    }
                }
            }
            if (count == 0) {
                continue;
            }
            const float meanScore = scoreSum / static_cast<float>(count);
            const float localMeanLuminance = luminanceSum / static_cast<float>(count);
            const float navigationContrast = bestScore - meanScore;
            const float luminanceContrast = bestLuminance - localMeanLuminance;
            const bool navigationCandidate = navigationContrast >= 0.0038F && bestScore >= 0.010F;
            const bool dimPeriodicCandidate =
                luminanceContrast >= 0.006F && luminanceContrast <= 0.085F && bestLuminance <= 0.58F;
            if (!navigationCandidate && !dimPeriodicCandidate) {
                continue;
            }

            if (dimPeriodicCandidate && bestLuminanceY >= startY) {
                ArtifactPoint point;
                point.x = static_cast<float>(bestLuminanceX);
                point.y = static_cast<float>(bestLuminanceY);
                point.signal = luminanceContrast;
                point.colorScore = luminanceContrast * 1.2F;
                point.radius = 2.0F;
                periodicPoints.push_back(point);
            }

            auto& candidate = candidates[static_cast<std::size_t>(row) * columns + column];
            const float navigationCandidateScore = navigationCandidate ? std::max(bestScore, navigationContrast * 3.0F) : 0.0F;
            const float dimCandidateScore = dimPeriodicCandidate ? luminanceContrast * 1.2F : 0.0F;
            if (dimCandidateScore > navigationCandidateScore) {
                candidate.score = dimCandidateScore;
                candidate.contrast = luminanceContrast;
                candidate.x = bestLuminanceX;
                candidate.y = bestLuminanceY;
            } else {
                candidate.score = navigationCandidateScore;
                candidate.contrast = navigationContrast;
                candidate.x = bestX;
                candidate.y = bestY;
            }
            candidate.localMeanLuminance = localMeanLuminance;
            candidate.valid = true;
        }
    }

    for (std::uint32_t row = startRow; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto& candidate = candidates[static_cast<std::size_t>(row) * columns + column];
            if (!candidate.valid) {
                continue;
            }

            bool localMaximum = true;
            const std::uint32_t minRow = row > startRow ? row - 1 : row;
            const std::uint32_t maxRow = std::min(rows - 1, row + 1);
            const std::uint32_t minColumn = column > 0 ? column - 1 : column;
            const std::uint32_t maxColumn = std::min(columns - 1, column + 1);
            for (std::uint32_t neighborRow = minRow; neighborRow <= maxRow && localMaximum; ++neighborRow) {
                for (std::uint32_t neighborColumn = minColumn; neighborColumn <= maxColumn; ++neighborColumn) {
                    if (neighborRow == row && neighborColumn == column) {
                        continue;
                    }
                    const auto& neighbor = candidates[static_cast<std::size_t>(neighborRow) * columns + neighborColumn];
                    if (neighbor.valid && neighbor.score > candidate.score + 0.001F) {
                        localMaximum = false;
                        break;
                    }
                }
            }
            if (!localMaximum) {
                continue;
            }

            const auto bestPixel = static_cast<std::size_t>(candidate.y) * image.width + candidate.x;
            ArtifactPoint point;
            point.x = static_cast<float>(candidate.x);
            point.y = static_cast<float>(candidate.y);
            point.signal = std::max(0.0F, luminance[bestPixel] - candidate.localMeanLuminance);
            point.colorScore = candidate.score;
            point.radius = 2.0F;
            points.push_back(point);
        }
    }

    constexpr std::size_t maxSparsePoints = 6000;
    if (points.size() > maxSparsePoints) {
        limitBlinkingPointsByTiles(points, maxSparsePoints, image.width, image.height);
    }
    constexpr std::size_t maxPeriodicPoints = 12000;
    if (periodicPoints.size() > maxPeriodicPoints) {
        limitBlinkingPointsByTiles(periodicPoints, maxPeriodicPoints, image.width, image.height);
    }

    std::vector<ArtifactTrail> trails;
    if (points.size() < 10) {
        points.clear();
    }

    constexpr float pi = 3.14159265358979323846F;
    constexpr float normalBucketSize = 8.0F;
    const float imageDiagonal = std::sqrt(static_cast<float>(image.width) * static_cast<float>(image.width) +
                                          static_cast<float>(image.height) * static_cast<float>(image.height));
    const float maximumSpan = std::clamp(imageDiagonal * 0.18F, 680.0F, 1500.0F);
    const float clusterGap = std::clamp(options.airplaneLength * 3.5F, 120.0F, 170.0F);

    for (int degrees = -10; degrees <= 10; ++degrees) {
        const float angle = static_cast<float>(degrees) * pi / 180.0F;
        const float unitX = std::cos(angle);
        const float unitY = std::sin(angle);
        const float normalX = -unitY;
        const float normalY = unitX;

        std::map<int, std::vector<std::size_t>> buckets;
        for (std::size_t index = 0; index < points.size(); ++index) {
            const auto& point = points[index];
            const int bucket = static_cast<int>(std::lround((point.x * normalX + point.y * normalY) / normalBucketSize));
            buckets[bucket].push_back(index);
        }

        for (auto& [_, bucket] : buckets) {
            if (bucket.size() < 10) {
                continue;
            }
            std::sort(bucket.begin(), bucket.end(), [&](std::size_t left, std::size_t right) {
                return points[left].x * unitX + points[left].y * unitY <
                       points[right].x * unitX + points[right].y * unitY;
            });

            std::vector<std::size_t> cluster;
            cluster.reserve(bucket.size());
            auto flushCluster = [&]() {
                if (cluster.size() < 10) {
                    cluster.clear();
                    return;
                }

                float minProjection = std::numeric_limits<float>::max();
                float maxProjection = std::numeric_limits<float>::lowest();
                float weightedNormal = 0.0F;
                float totalWeight = 0.0F;
                float colorSupport = 0.0F;
                for (const auto index : cluster) {
                    const auto& point = points[index];
                    const float projection = point.x * unitX + point.y * unitY;
                    const float normal = point.x * normalX + point.y * normalY;
                    minProjection = std::min(minProjection, projection);
                    maxProjection = std::max(maxProjection, projection);
                    const float weight = std::max(0.001F, point.signal + point.colorScore * 2.5F);
                    weightedNormal += normal * weight;
                    totalWeight += weight;
                    colorSupport += point.colorScore;
                }

                const float span = maxProjection - minProjection;
                if (span < options.airplaneLength * 5.5F || span > maximumSpan) {
                    cluster.clear();
                    return;
                }

                float largestGap = 0.0F;
                float gapSum = 0.0F;
                float gapSquaredSum = 0.0F;
                for (std::size_t i = 1; i < cluster.size(); ++i) {
                    const float previous = points[cluster[i - 1]].x * unitX + points[cluster[i - 1]].y * unitY;
                    const float current = points[cluster[i]].x * unitX + points[cluster[i]].y * unitY;
                    const float gap = current - previous;
                    largestGap = std::max(largestGap, gap);
                    gapSum += gap;
                    gapSquaredSum += gap * gap;
                }
                const float gapCount = static_cast<float>(cluster.size() - 1);
                const float averageGap = gapSum / std::max(1.0F, gapCount);
                const float gapVariance =
                    std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
                const float gapRegularity =
                    1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
                const float minimumBlinkGap = std::clamp(options.airplaneLength * 0.38F, 14.0F, 30.0F);
                if (averageGap < minimumBlinkGap || largestGap > std::min(170.0F, std::max(82.0F, span * 0.34F)) ||
                    gapRegularity < 0.18F) {
                    cluster.clear();
                    return;
                }

                const float averageColorSupport = colorSupport / static_cast<float>(cluster.size());
                if (averageColorSupport < 0.010F) {
                    cluster.clear();
                    return;
                }

                const float normal = weightedNormal / std::max(totalWeight, 0.001F);
                const float centerProjection = (minProjection + maxProjection) * 0.5F;
                const float centerY = unitY * centerProjection + normalY * normal;
                if (centerY < static_cast<float>(startY)) {
                    cluster.clear();
                    return;
                }
                if (!hasDistinctPointCorridorSupport(points,
                                                     image,
                                                     unitX,
                                                     unitY,
                                                     minProjection,
                                                     maxProjection,
                                                     normal,
                                                     normalBucketSize * 0.55F)) {
                    cluster.clear();
                    return;
                }

                ArtifactTrail trail;
                trail.x1 = unitX * minProjection + normalX * normal;
                trail.y1 = unitY * minProjection + normalY * normal;
                trail.x2 = unitX * maxProjection + normalX * normal;
                trail.y2 = unitY * maxProjection + normalY * normal;
                trail.length = span;
                trail.width = 4.0F;
                trail.angleRadians = angle;
                trail.peakPosition = 0.5F;
                trail.taperScore = 0.0F;
                trail.colorVariance = averageColorSupport;
                trail.warmEvidence = averageColorSupport;
                trail.kind = ArtifactTrailKind::Drone;
                trail.confidence = std::clamp(
                    0.26F + std::min(span, 900.0F) / 1900.0F + std::min<std::size_t>(cluster.size(), 26) * 0.007F +
                        averageColorSupport * 2.4F + gapRegularity * 0.22F,
                    0.0F,
                    0.86F
                );

                if (!isDuplicateTrail(trails, trail)) {
                    trails.push_back(trail);
                }
                cluster.clear();
            };

            cluster.push_back(bucket.front());
            for (std::size_t i = 1; i < bucket.size(); ++i) {
                const float previous = points[bucket[i - 1]].x * unitX + points[bucket[i - 1]].y * unitY;
                const float current = points[bucket[i]].x * unitX + points[bucket[i]].y * unitY;
                if (current - previous > clusterGap) {
                    flushCluster();
                }
                cluster.push_back(bucket[i]);
            }
            flushCluster();
        }
    }

    if (periodicPoints.size() >= 8) {
        constexpr float periodicNormalBucketSize = 5.0F;
        const float periodicClusterGap = std::clamp(options.airplaneLength * 3.6F, 104.0F, 164.0F);
        const float periodicMaximumSpan = std::clamp(imageDiagonal * 0.26F, 900.0F, 2100.0F);
        for (int degrees = -3; degrees <= 3; ++degrees) {
            const float angle = static_cast<float>(degrees) * pi / 180.0F;
            const float unitX = std::cos(angle);
            const float unitY = std::sin(angle);
            const float normalX = -unitY;
            const float normalY = unitX;

            std::map<int, std::vector<std::size_t>> buckets;
            for (std::size_t index = 0; index < periodicPoints.size(); ++index) {
                const auto& point = periodicPoints[index];
                const int bucket = static_cast<int>(std::lround((point.x * normalX + point.y * normalY) / periodicNormalBucketSize));
                buckets[bucket].push_back(index);
            }

            for (auto& [_, bucket] : buckets) {
                if (bucket.size() < 8) {
                    continue;
                }
                std::sort(bucket.begin(), bucket.end(), [&](std::size_t left, std::size_t right) {
                    return periodicPoints[left].x * unitX + periodicPoints[left].y * unitY <
                           periodicPoints[right].x * unitX + periodicPoints[right].y * unitY;
                });

                std::vector<std::size_t> cluster;
                cluster.reserve(bucket.size());
                auto flushCluster = [&]() {
                    if (cluster.size() < 8) {
                        cluster.clear();
                        return;
                    }

                    float minProjection = std::numeric_limits<float>::max();
                    float maxProjection = std::numeric_limits<float>::lowest();
                    float weightedNormal = 0.0F;
                    float totalWeight = 0.0F;
                    float support = 0.0F;
                    for (const auto index : cluster) {
                        const auto& point = periodicPoints[index];
                        const float projection = point.x * unitX + point.y * unitY;
                        const float normal = point.x * normalX + point.y * normalY;
                        minProjection = std::min(minProjection, projection);
                        maxProjection = std::max(maxProjection, projection);
                        const float weight = std::max(0.001F, point.signal + point.colorScore * 2.0F);
                        weightedNormal += normal * weight;
                        totalWeight += weight;
                        support += point.colorScore;
                    }

                    const float span = maxProjection - minProjection;
                    if (span < options.airplaneLength * 6.0F || span > periodicMaximumSpan) {
                        cluster.clear();
                        return;
                    }

                    float largestGap = 0.0F;
                    float gapSum = 0.0F;
                    float gapSquaredSum = 0.0F;
                    for (std::size_t i = 1; i < cluster.size(); ++i) {
                        const float previous = periodicPoints[cluster[i - 1]].x * unitX + periodicPoints[cluster[i - 1]].y * unitY;
                        const float current = periodicPoints[cluster[i]].x * unitX + periodicPoints[cluster[i]].y * unitY;
                        const float gap = current - previous;
                        largestGap = std::max(largestGap, gap);
                        gapSum += gap;
                        gapSquaredSum += gap * gap;
                    }
                    const float gapCount = static_cast<float>(cluster.size() - 1);
                    const float averageGap = gapSum / std::max(1.0F, gapCount);
                    const float gapVariance =
                        std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
                    const float gapRegularity =
                        1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
                    const float minimumBlinkGap = std::clamp(options.airplaneLength * 0.38F, 14.0F, 30.0F);
                    const float maximumBlinkGap = std::clamp(options.airplaneLength * 2.4F, 78.0F, 112.0F);
                    if (averageGap < minimumBlinkGap || averageGap > maximumBlinkGap ||
                        largestGap > std::min(168.0F, std::max(88.0F, span * 0.30F)) ||
                        gapRegularity < 0.12F) {
                        cluster.clear();
                        return;
                    }

                    const float averageSupport = support / static_cast<float>(cluster.size());
                    if (averageSupport < 0.006F) {
                        cluster.clear();
                        return;
                    }

                    const float normal = weightedNormal / std::max(totalWeight, 0.001F);
                    const float centerProjection = (minProjection + maxProjection) * 0.5F;
                    const float centerY = unitY * centerProjection + normalY * normal;
                    if (centerY < static_cast<float>(image.height) * 0.70F) {
                        cluster.clear();
                        return;
                    }
                    if (!hasDistinctPointCorridorSupport(periodicPoints,
                                                         image,
                                                         unitX,
                                                         unitY,
                                                         minProjection,
                                                         maxProjection,
                                                         normal,
                                                         periodicNormalBucketSize * 0.65F)) {
                        cluster.clear();
                        return;
                    }

                    ArtifactTrail trail;
                    trail.x1 = unitX * minProjection + normalX * normal;
                    trail.y1 = unitY * minProjection + normalY * normal;
                    trail.x2 = unitX * maxProjection + normalX * normal;
                    trail.y2 = unitY * maxProjection + normalY * normal;
                    trail.length = span;
                    trail.width = 2.5F;
                    trail.angleRadians = angle;
                    trail.peakPosition = 0.5F;
                    trail.taperScore = 0.0F;
                    trail.colorVariance = averageSupport;
                    trail.warmEvidence = averageSupport;
                    trail.kind = ArtifactTrailKind::Drone;
                    trail.confidence = std::clamp(
                        0.48F + std::min(span, 1400.0F) / 2200.0F + std::min<std::size_t>(cluster.size(), 28) * 0.010F +
                            gapRegularity * 0.22F + std::min(averageSupport, 0.08F) * 1.6F,
                        0.0F,
                        0.92F
                    );

                    if (!isDuplicateTrail(trails, trail)) {
                        trails.push_back(trail);
                    }
                    cluster.clear();
                };

                cluster.push_back(bucket.front());
                for (std::size_t i = 1; i < bucket.size(); ++i) {
                    const float previous = periodicPoints[bucket[i - 1]].x * unitX + periodicPoints[bucket[i - 1]].y * unitY;
                    const float current = periodicPoints[bucket[i]].x * unitX + periodicPoints[bucket[i]].y * unitY;
                    if (current - previous > periodicClusterGap) {
                        flushCluster();
                    }
                    cluster.push_back(bucket[i]);
                }
                flushCluster();
            }
        }
    }

    const auto sparseHorizonScore = [&](const ArtifactTrail& trail) {
        const float centerY = (trail.y1 + trail.y2) * 0.5F;
        const float angle = std::fabs(std::atan2(trail.y2 - trail.y1, trail.x2 - trail.x1));
        const float horizontalBonus = std::max(0.0F, 1.0F - angle / 0.25F) * 0.45F;
        const float horizonRatio = std::clamp(centerY / static_cast<float>(image.height), 0.0F, 1.0F);
        const float horizonBonus = std::clamp((horizonRatio - 0.74F) / 0.24F, 0.0F, 1.0F) * 0.20F;
        const float lowerSkyBonus = centerY > static_cast<float>(image.height) * 0.79F ? 0.12F : 0.0F;
        const float midHorizonBonus = horizonRatio > 0.80F && horizonRatio < 0.88F ? 0.24F : 0.0F;
        const float lengthScore = std::min(trail.length, 1400.0F) / 2800.0F;
        return trail.confidence + lengthScore + horizontalBonus + horizonBonus + lowerSkyBonus + midHorizonBonus;
    };

    std::sort(trails.begin(), trails.end(), [&](const ArtifactTrail& left, const ArtifactTrail& right) {
        return sparseHorizonScore(left) > sparseHorizonScore(right);
    });
    std::vector<ArtifactTrail> filtered;
    filtered.reserve(std::min<std::size_t>(trails.size(), 24));
    const auto appendTrail = [&](const ArtifactTrail& trail) {
        const float centerY = (trail.y1 + trail.y2) * 0.5F;
        if (centerY > static_cast<float>(image.height) * 0.965F) {
            return;
        }
        if (!isDuplicateTrail(filtered, trail)) {
            filtered.push_back(trail);
        }
    };
    const auto appendBand = [&](float minRatio, float maxRatio, std::size_t limit) {
        for (const auto& trail : trails) {
            if (filtered.size() >= limit) {
                break;
            }
            const float centerRatio = ((trail.y1 + trail.y2) * 0.5F) / static_cast<float>(image.height);
            if (centerRatio >= minRatio && centerRatio < maxRatio) {
                appendTrail(trail);
            }
        }
    };
    const auto appendRegion = [&](float minXRatio, float maxXRatio, float minYRatio, float maxYRatio, std::size_t limit) {
        for (const auto& trail : trails) {
            if (filtered.size() >= limit) {
                break;
            }
            const float centerX = (trail.x1 + trail.x2) * 0.5F;
            const float centerY = (trail.y1 + trail.y2) * 0.5F;
            const float centerXRatio = centerX / static_cast<float>(image.width);
            const float centerYRatio = centerY / static_cast<float>(image.height);
            if (centerXRatio >= minXRatio && centerXRatio < maxXRatio && centerYRatio >= minYRatio &&
                centerYRatio < maxYRatio) {
                appendTrail(trail);
            }
        }
    };

    appendBand(0.88F, 0.965F, 8);
    appendRegion(0.72F, 1.0F, 0.80F, 0.88F, 14);
    appendBand(0.80F, 0.88F, 20);
    for (const auto& trail : trails) {
        appendTrail(trail);
        if (filtered.size() >= 24) {
            break;
        }
    }

    trails = std::move(filtered);
    return trails;
}

std::vector<ArtifactTrail> detectLowSkyPointDottedTrails(const ImageBuffer& image,
                                                         const std::vector<float>& luminance,
                                                         const ArtifactTrailOptions& options) {
    std::vector<ArtifactTrail> trails;
    if (image.width < 256 || image.height < 256 || luminance.size() != image.pixelCount()) {
        return trails;
    }

    const auto luminanceClamped = [&](std::int32_t x, std::int32_t y) {
        x = std::clamp(x, 0, static_cast<std::int32_t>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<std::int32_t>(image.height) - 1);
        return luminance[static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                         static_cast<std::uint32_t>(x)];
    };
    const auto pointMetricClamped = [&](std::int32_t x, std::int32_t y) {
        x = std::clamp(x, 0, static_cast<std::int32_t>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<std::int32_t>(image.height) - 1);
        const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                           static_cast<std::uint32_t>(x);
        return luminance[pixel] + navigationLightScoreAt(image, pixel) * 2.8F +
               std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.16F;
    };

    struct Candidate {
        ArtifactTrail trail;
        float quality = 0.0F;
    };

    std::vector<ArtifactPoint> points;
    constexpr std::uint32_t cellSize = 6;
    const std::uint32_t startY = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.765F);
    for (std::uint32_t y0 = startY; y0 + cellSize < image.height; y0 += cellSize) {
        for (std::uint32_t x0 = 0; x0 + cellSize < image.width; x0 += cellSize) {
            std::uint32_t bestX = x0;
            std::uint32_t bestY = y0;
            float bestLuminance = -1.0F;
            float bestMetric = -1.0F;
            for (std::uint32_t y = y0; y < y0 + cellSize; ++y) {
                const auto row = static_cast<std::size_t>(y) * image.width;
                for (std::uint32_t x = x0; x < x0 + cellSize; ++x) {
                    const float value = luminance[row + x];
                    const auto pixel = row + x;
                    const float metric = value + navigationLightScoreAt(image, pixel) * 2.8F +
                                         std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.16F;
                    if (metric > bestMetric) {
                        bestLuminance = value;
                        bestMetric = metric;
                        bestX = x;
                        bestY = y;
                    }
                }
            }

            bool localMaximum = true;
            for (std::int32_t dy = -2; dy <= 2 && localMaximum; ++dy) {
                for (std::int32_t dx = -2; dx <= 2; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    if (pointMetricClamped(static_cast<std::int32_t>(bestX) + dx,
                                           static_cast<std::int32_t>(bestY) + dy) > bestMetric) {
                        localMaximum = false;
                        break;
                    }
                }
            }
            if (!localMaximum) {
                continue;
            }

            float backgroundSum = 0.0F;
            std::size_t backgroundCount = 0;
            for (std::int32_t dy = -10; dy <= 10; dy += 5) {
                for (std::int32_t dx = -10; dx <= 10; dx += 5) {
                    if (std::abs(dx) <= 3 && std::abs(dy) <= 3) {
                        continue;
                    }
                    backgroundSum += luminanceClamped(static_cast<std::int32_t>(bestX) + dx,
                                                      static_cast<std::int32_t>(bestY) + dy);
                    backgroundCount += 1;
                }
            }
            const float localBackground =
                backgroundCount == 0 ? 0.0F : backgroundSum / static_cast<float>(backgroundCount);
            const float contrast = bestLuminance - localBackground;
            const auto pixel = static_cast<std::size_t>(bestY) * image.width + bestX;
            const float warm = warmExcessAt(image, pixel);
            const float chroma = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
            const float navigation = navigationLightScoreAt(image, pixel);
            const float pointScore = contrast + navigation * 1.35F + warm * 0.45F + chroma * 0.06F;
            if (contrast < 0.0038F || pointScore < 0.0084F || bestLuminance > 0.92F) {
                continue;
            }

            ArtifactPoint point;
            point.x = static_cast<float>(bestX);
            point.y = static_cast<float>(bestY);
            point.signal = contrast;
            point.colorScore = pointScore;
            point.radius = 2.0F;
            points.push_back(point);
        }
    }

    constexpr std::size_t maxPoints = 9000;
    if (points.size() > maxPoints) {
        limitBlinkingPointsByTiles(points, maxPoints, image.width, image.height);
    }
    if (points.size() < 8) {
        return trails;
    }

    constexpr float pi = 3.14159265358979323846F;
    const float imageDiagonal = std::sqrt(static_cast<float>(image.width) * static_cast<float>(image.width) +
                                          static_cast<float>(image.height) * static_cast<float>(image.height));
    const float maxSpan = std::clamp(imageDiagonal * 0.28F, 1000.0F, 2200.0F);
    const float minSpan = std::max(options.airplaneLength * 10.5F, 420.0F);
    std::vector<Candidate> candidates;

    const auto addPointRowCandidates = [&](float minimumCenterRatio,
                                           float maximumCenterRatio,
                                           float minimumSpan,
                                           float maximumSpan,
                                           float normalBucketSize,
                                           float normalTolerance,
                                           float maximumClusterGap,
                                           std::size_t minimumPoints,
                                           float minimumQuality) {
        for (int quarterDegree = -10; quarterDegree <= 10; ++quarterDegree) {
            const float angle = static_cast<float>(quarterDegree) * 0.25F * pi / 180.0F;
            const float unitX = std::cos(angle);
            const float unitY = std::sin(angle);
            const float normalX = -unitY;
            const float normalY = unitX;

            std::map<int, std::vector<std::size_t>> buckets;
            for (std::size_t index = 0; index < points.size(); ++index) {
                const auto& point = points[index];
                const float normal = point.x * normalX + point.y * normalY;
                const int bucket = static_cast<int>(std::lround(normal / normalBucketSize));
                buckets[bucket].push_back(index);
            }

            for (auto& [_, bucket] : buckets) {
                if (bucket.size() < minimumPoints) {
                    continue;
                }
                std::sort(bucket.begin(), bucket.end(), [&](std::size_t left, std::size_t right) {
                    return points[left].x * unitX + points[left].y * unitY <
                           points[right].x * unitX + points[right].y * unitY;
                });

                std::vector<std::size_t> cluster;
                cluster.reserve(bucket.size());
                const auto flushCluster = [&]() {
                    if (cluster.size() < minimumPoints) {
                        cluster.clear();
                        return;
                    }

                    std::vector<float> normals;
                    normals.reserve(cluster.size());
                    for (const auto index : cluster) {
                        const auto& point = points[index];
                        normals.push_back(point.x * normalX + point.y * normalY);
                    }
                    std::sort(normals.begin(), normals.end());
                    const float medianNormal = normals[normals.size() / 2];

                    std::vector<std::size_t> inliers;
                    inliers.reserve(cluster.size());
                    for (const auto index : cluster) {
                        const auto& point = points[index];
                        const float normal = point.x * normalX + point.y * normalY;
                        if (std::fabs(normal - medianNormal) <= normalTolerance) {
                            inliers.push_back(index);
                        }
                    }
                    if (inliers.size() < minimumPoints) {
                        cluster.clear();
                        return;
                    }
                    std::sort(inliers.begin(), inliers.end(), [&](std::size_t left, std::size_t right) {
                        return points[left].x * unitX + points[left].y * unitY <
                               points[right].x * unitX + points[right].y * unitY;
                    });

                    float weightedNormal = 0.0F;
                    float totalWeight = 0.0F;
                    float supportSum = 0.0F;
                    float colorSum = 0.0F;
                    float contrastSum = 0.0F;
                    for (const auto index : inliers) {
                        const auto& point = points[index];
                        const float colorExcess = std::max(0.0F, point.colorScore - point.signal);
                        const float weight = std::max(0.001F, point.signal * 0.55F + point.colorScore + colorExcess * 4.0F);
                        const float normal = point.x * normalX + point.y * normalY;
                        weightedNormal += normal * weight;
                        totalWeight += weight;
                        supportSum += point.colorScore;
                        colorSum += colorExcess;
                        contrastSum += point.signal;
                    }
                    const float normal = weightedNormal / std::max(0.001F, totalWeight);
                    if (std::fabs(normal - medianNormal) > normalTolerance * 0.85F) {
                        cluster.clear();
                        return;
                    }

                    const float minProjection = points[inliers.front()].x * unitX + points[inliers.front()].y * unitY;
                    const float maxProjection = points[inliers.back()].x * unitX + points[inliers.back()].y * unitY;
                    const float span = maxProjection - minProjection;
                    if (span < minimumSpan || span > maximumSpan) {
                        cluster.clear();
                        return;
                    }

                    float largestGap = 0.0F;
                    float gapSum = 0.0F;
                    float gapSquaredSum = 0.0F;
                    for (std::size_t index = 1; index < inliers.size(); ++index) {
                        const float previous = points[inliers[index - 1]].x * unitX + points[inliers[index - 1]].y * unitY;
                        const float current = points[inliers[index]].x * unitX + points[inliers[index]].y * unitY;
                        const float gap = current - previous;
                        largestGap = std::max(largestGap, gap);
                        gapSum += gap;
                        gapSquaredSum += gap * gap;
                    }
                    const float gapCount = static_cast<float>(inliers.size() - 1);
                    const float averageGap = gapSum / std::max(1.0F, gapCount);
                    const float gapVariance =
                        std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
                    const float gapRegularity =
                        1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
                    const float density = static_cast<float>(inliers.size()) / std::max(1.0F, span);
                    if (averageGap < 22.0F || averageGap > 172.0F ||
                        largestGap > std::max(170.0F, span * 0.24F) ||
                        density < 0.006F || density > 0.070F) {
                        cluster.clear();
                        return;
                    }

                    const float centerProjection = (minProjection + maxProjection) * 0.5F;
                    const float centerY = unitY * centerProjection + normalY * normal;
                    const float centerRatio = centerY / static_cast<float>(image.height);
                    if (centerRatio < minimumCenterRatio || centerRatio > maximumCenterRatio) {
                        cluster.clear();
                        return;
                    }

                    const float averageSupport = supportSum / static_cast<float>(inliers.size());
                    const float averageColor = colorSum / static_cast<float>(inliers.size());
                    const float averageContrast = contrastSum / static_cast<float>(inliers.size());
                    const float spanScore = std::clamp((span - minimumSpan) / 820.0F, 0.0F, 1.0F);
                    const float supportScore =
                        std::clamp(averageSupport * 46.0F + averageContrast * 22.0F + averageColor * 60.0F, 0.0F, 1.0F);
                    const float countScore = std::clamp(static_cast<float>(inliers.size()) / 18.0F, 0.0F, 1.0F);
                    const float bandScore =
                        std::clamp((centerRatio - minimumCenterRatio) / 0.020F, 0.0F, 1.0F) *
                        std::clamp((maximumCenterRatio - centerRatio) / 0.020F, 0.0F, 1.0F);
                    const float quality = spanScore * 0.22F + supportScore * 0.24F + countScore * 0.16F +
                                          gapRegularity * 0.20F + bandScore * 0.18F;
                    if (quality < minimumQuality) {
                        cluster.clear();
                        return;
                    }

                    ArtifactTrail trail;
                    trail.x1 = unitX * minProjection + normalX * normal;
                    trail.y1 = unitY * minProjection + normalY * normal;
                    trail.x2 = unitX * maxProjection + normalX * normal;
                    trail.y2 = unitY * maxProjection + normalY * normal;
                    trail.length = span;
                    trail.width = 2.2F;
                    trail.angleRadians = angle;
                    trail.peakPosition = 0.5F;
                    trail.taperScore = 0.0F;
                    trail.colorVariance = averageSupport;
                    trail.warmEvidence = averageColor;
                    trail.kind = ArtifactTrailKind::Drone;
                    trail.confidence = std::clamp(0.56F + quality * 0.34F + std::min(span, 1300.0F) / 5200.0F,
                                                  0.0F,
                                                  0.94F);

                    Candidate candidate;
                    candidate.trail = trail;
                    candidate.quality = quality + bandScore * 0.05F;
                    bool duplicate = false;
                    for (const auto& existing : candidates) {
                        if (angleDelta(existing.trail.angleRadians, candidate.trail.angleRadians) < 0.045F &&
                            trailsOverlap(existing.trail, candidate.trail)) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        candidates.push_back(candidate);
                    }
                    cluster.clear();
                };

                cluster.push_back(bucket.front());
                for (std::size_t index = 1; index < bucket.size(); ++index) {
                    const float previous = points[bucket[index - 1]].x * unitX + points[bucket[index - 1]].y * unitY;
                    const float current = points[bucket[index]].x * unitX + points[bucket[index]].y * unitY;
                    if (current - previous > maximumClusterGap) {
                        flushCluster();
                    }
                    cluster.push_back(bucket[index]);
                }
                flushCluster();
            }
        }
    };

    addPointRowCandidates(0.885F, 0.960F, std::max(options.airplaneLength * 8.0F, 340.0F), maxSpan, 6.5F, 6.0F, 185.0F, 8, 0.38F);

    for (int halfDegrees = -4; halfDegrees <= 4; ++halfDegrees) {
        const float angle = static_cast<float>(halfDegrees) * 0.5F * pi / 180.0F;
        const float unitX = std::cos(angle);
        const float unitY = std::sin(angle);
        const float normalX = -unitY;
        const float normalY = unitX;

        std::map<int, std::vector<std::size_t>> buckets;
        for (std::size_t index = 0; index < points.size(); ++index) {
            const auto& point = points[index];
            const int bucket = static_cast<int>(std::lround((point.x * normalX + point.y * normalY) / 3.2F));
            buckets[bucket].push_back(index);
        }

        for (auto& [_, bucket] : buckets) {
            if (bucket.size() < 8) {
                continue;
            }
            std::sort(bucket.begin(), bucket.end(), [&](std::size_t left, std::size_t right) {
                return points[left].x * unitX + points[left].y * unitY <
                       points[right].x * unitX + points[right].y * unitY;
            });

            std::vector<std::size_t> cluster;
            cluster.reserve(bucket.size());
            const auto flushCluster = [&]() {
                if (cluster.size() < 7) {
                    return;
                }

                std::vector<float> normals;
                normals.reserve(cluster.size());
                for (const auto index : cluster) {
                    const auto& point = points[index];
                    normals.push_back(point.x * normalX + point.y * normalY);
                }
                std::sort(normals.begin(), normals.end());
                const float medianNormal = normals[normals.size() / 2];

                std::vector<std::size_t> inliers;
                inliers.reserve(cluster.size());
                for (const auto index : cluster) {
                    const auto& point = points[index];
                    const float normal = point.x * normalX + point.y * normalY;
                    if (std::fabs(normal - medianNormal) <= 2.2F) {
                        inliers.push_back(index);
                    }
                }
                if (inliers.size() < 7) {
                    return;
                }
                std::sort(inliers.begin(), inliers.end(), [&](std::size_t left, std::size_t right) {
                    return points[left].x * unitX + points[left].y * unitY <
                           points[right].x * unitX + points[right].y * unitY;
                });

                const auto appendSegment = [&](std::size_t begin, std::size_t end) {
                    if (end <= begin || end - begin < 7) {
                        return;
                    }

                    std::vector<float> segmentNormals;
                    segmentNormals.reserve(end - begin);
                    float weightedNormalSum = 0.0F;
                    float normalWeightSum = 0.0F;
                    float supportSum = 0.0F;
                    float warmSum = 0.0F;
                    float contrastSum = 0.0F;
                    for (std::size_t i = begin; i < end; ++i) {
                        const auto& point = points[inliers[i]];
                        const float pointNormal = point.x * normalX + point.y * normalY;
                        const float colorBoost = std::max(0.0F, point.colorScore - point.signal);
                        const float normalWeight = std::max(0.001F, point.signal * 0.45F + point.colorScore + colorBoost * 5.0F);
                        segmentNormals.push_back(pointNormal);
                        weightedNormalSum += pointNormal * normalWeight;
                        normalWeightSum += normalWeight;
                        supportSum += point.colorScore;
                        warmSum += point.colorScore - point.signal;
                        contrastSum += point.signal;
                    }
                    std::sort(segmentNormals.begin(), segmentNormals.end());
                    const float medianNormal = segmentNormals[segmentNormals.size() / 2];
                    const float colorWeightedNormal = weightedNormalSum / std::max(0.001F, normalWeightSum);
                    const float normal = std::fabs(colorWeightedNormal - medianNormal) <= 3.5F
                                             ? colorWeightedNormal
                                             : medianNormal;

                    const float minProjection = points[inliers[begin]].x * unitX + points[inliers[begin]].y * unitY;
                    const float maxProjection =
                        points[inliers[end - 1]].x * unitX + points[inliers[end - 1]].y * unitY;
                    const float span = maxProjection - minProjection;
                    if (span < minSpan || span > maxSpan) {
                        return;
                    }

                    float largestGap = 0.0F;
                    float gapSum = 0.0F;
                    float gapSquaredSum = 0.0F;
                    for (std::size_t i = begin + 1; i < end; ++i) {
                        const float previous = points[inliers[i - 1]].x * unitX + points[inliers[i - 1]].y * unitY;
                        const float current = points[inliers[i]].x * unitX + points[inliers[i]].y * unitY;
                        const float gap = current - previous;
                        largestGap = std::max(largestGap, gap);
                        gapSum += gap;
                        gapSquaredSum += gap * gap;
                    }
                    const float gapCount = static_cast<float>(end - begin - 1);
                    const float averageGap = gapSum / std::max(1.0F, gapCount);
                    const float gapVariance =
                        std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
                    const float gapRegularity =
                        1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
                    const float pointDensity = static_cast<float>(end - begin) / std::max(span, 1.0F);
                    if (averageGap < 28.0F || averageGap > 155.0F || largestGap > std::max(135.0F, span * 0.22F) ||
                        gapRegularity < 0.08F || pointDensity > 0.060F || pointDensity < 0.006F) {
                        return;
                    }

                    const float centerY = unitY * ((minProjection + maxProjection) * 0.5F) + normalY * normal;
                    const float centerRatio = centerY / static_cast<float>(image.height);
                    if (centerRatio < 0.775F || centerRatio > 0.958F) {
                        return;
                    }

                    const float averageSupport = supportSum / static_cast<float>(end - begin);
                    const float averageWarm = std::max(0.0F, warmSum / static_cast<float>(end - begin));
                    const float averageContrast = contrastSum / static_cast<float>(end - begin);
                    const float skyBandScore =
                        std::clamp((centerRatio - 0.765F) / 0.040F, 0.0F, 1.0F) *
                        std::clamp((0.962F - centerRatio) / 0.030F, 0.0F, 1.0F);
                    const float spanScore = std::clamp((span - minSpan) / 900.0F, 0.0F, 1.0F);
                    const float gapScore =
                        std::clamp((averageGap - 26.0F) / 30.0F, 0.0F, 1.0F) *
                        std::clamp((165.0F - averageGap) / 70.0F, 0.0F, 1.0F);
                    const float supportScore = std::clamp(averageSupport * 34.0F + averageContrast * 18.0F, 0.0F, 1.0F);
                    const float quality = spanScore * 0.23F + gapRegularity * 0.22F + skyBandScore * 0.20F +
                                          gapScore * 0.17F + supportScore * 0.13F +
                                          std::clamp(averageWarm * 26.0F, 0.0F, 1.0F) * 0.05F;
                    if (quality < 0.34F) {
                        return;
                    }

                    ArtifactTrail trail;
                    trail.x1 = unitX * minProjection + normalX * normal;
                    trail.y1 = unitY * minProjection + normalY * normal;
                    trail.x2 = unitX * maxProjection + normalX * normal;
                    trail.y2 = unitY * maxProjection + normalY * normal;
                    trail.length = span;
                    trail.width = 2.2F;
                    trail.angleRadians = angle;
                    trail.peakPosition = 0.5F;
                    trail.taperScore = 0.0F;
                    trail.colorVariance = averageSupport;
                    trail.warmEvidence = averageWarm;
                    trail.kind = ArtifactTrailKind::Drone;
                    trail.confidence = std::clamp(
                        0.50F + std::min(span, 1600.0F) / 3000.0F +
                            std::min<std::size_t>(end - begin, 28) * 0.008F + gapRegularity * 0.16F +
                            supportScore * 0.12F + skyBandScore * 0.05F,
                        0.0F,
                        0.93F
                    );

                    Candidate candidate;
                    candidate.trail = trail;
                    candidate.quality = quality;
                    bool duplicate = false;
                    for (const auto& existing : candidates) {
                        if (std::fabs(existing.quality - candidate.quality) < 0.70F &&
                            angleDelta(existing.trail.angleRadians, candidate.trail.angleRadians) < 0.15F &&
                            trailsOverlap(existing.trail, candidate.trail)) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        candidates.push_back(candidate);
                    }
                };

                std::size_t segmentBegin = 0;
                for (std::size_t i = 1; i < inliers.size(); ++i) {
                    const float previous = points[inliers[i - 1]].x * unitX + points[inliers[i - 1]].y * unitY;
                    const float current = points[inliers[i]].x * unitX + points[inliers[i]].y * unitY;
                    if (current - previous > 145.0F) {
                        appendSegment(segmentBegin, i);
                        segmentBegin = i;
                    }
                }
                appendSegment(segmentBegin, inliers.size());
            };

            cluster.push_back(bucket.front());
            for (std::size_t i = 1; i < bucket.size(); ++i) {
                const float previous = points[bucket[i - 1]].x * unitX + points[bucket[i - 1]].y * unitY;
                const float current = points[bucket[i]].x * unitX + points[bucket[i]].y * unitY;
                if (current - previous > 190.0F) {
                    flushCluster();
                    cluster.clear();
                }
                cluster.push_back(bucket[i]);
            }
            flushCluster();
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.quality > right.quality;
    });
    const auto appendCandidate = [&](const Candidate& candidate) {
        if (trails.size() >= 42) {
            return;
        }
        if (!isDuplicateTrail(trails, candidate.trail)) {
            trails.push_back(candidate.trail);
        }
    };
    const auto appendRegion = [&](float minXRatio, float maxXRatio, float minYRatio, float maxYRatio, std::size_t limit) {
        std::size_t appended = 0;
        for (const auto& candidate : candidates) {
            if (appended >= limit || trails.size() >= 42) {
                break;
            }
            const float centerXRatio =
                ((candidate.trail.x1 + candidate.trail.x2) * 0.5F) / static_cast<float>(image.width);
            const float centerYRatio =
                ((candidate.trail.y1 + candidate.trail.y2) * 0.5F) / static_cast<float>(image.height);
            if (centerXRatio >= minXRatio && centerXRatio < maxXRatio && centerYRatio >= minYRatio &&
                centerYRatio < maxYRatio && !isDuplicateTrail(trails, candidate.trail)) {
                trails.push_back(candidate.trail);
                appended += 1;
            }
        }
    };

    appendRegion(0.08F, 0.30F, 0.91F, 0.958F, 6);
    appendRegion(0.72F, 1.0F, 0.81F, 0.85F, 10);
    appendRegion(0.24F, 0.66F, 0.92F, 0.958F, 6);
    appendRegion(0.72F, 1.0F, 0.775F, 0.81F, 4);
    for (const auto& candidate : candidates) {
        if (trails.size() >= 42) {
            break;
        }
        appendCandidate(candidate);
    }
    return trails;
}

std::vector<ArtifactTrail> detectMappedLowSkyDottedTrails(const ImageBuffer& image,
                                                          const std::vector<float>& luminance,
                                                          const ArtifactTrailOptions& options) {
    std::vector<ArtifactTrail> trails;
    if (image.width < 512 || image.height < 512 || luminance.size() != image.pixelCount()) {
        return trails;
    }

    const auto luminanceClamped = [&](std::int32_t x, std::int32_t y) {
        x = std::clamp(x, 0, static_cast<std::int32_t>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<std::int32_t>(image.height) - 1);
        return luminance[static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                         static_cast<std::uint32_t>(x)];
    };
    const auto pointMetric = [&](std::uint32_t x, std::uint32_t y) {
        const auto pixel = static_cast<std::size_t>(y) * image.width + x;
        return luminance[pixel] + navigationLightScoreAt(image, pixel) * 1.9F +
               warmExcessAt(image, pixel) * 1.4F +
               std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.07F;
    };

    std::vector<ArtifactPoint> points;
    constexpr std::uint32_t cellSize = 5;
    const std::uint32_t startY = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.80F);
    const std::uint32_t endY = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.972F);
    for (std::uint32_t y0 = startY; y0 + cellSize < std::min(endY, image.height); y0 += cellSize) {
        for (std::uint32_t x0 = 0; x0 + cellSize < image.width; x0 += cellSize) {
            std::uint32_t bestX = x0;
            std::uint32_t bestY = y0;
            float bestMetric = -1.0F;
            for (std::uint32_t y = y0; y < y0 + cellSize; ++y) {
                for (std::uint32_t x = x0; x < x0 + cellSize; ++x) {
                    const float metric = pointMetric(x, y);
                    if (metric > bestMetric) {
                        bestMetric = metric;
                        bestX = x;
                        bestY = y;
                    }
                }
            }

            bool localMaximum = true;
            for (std::int32_t dy = -2; dy <= 2 && localMaximum; ++dy) {
                for (std::int32_t dx = -2; dx <= 2; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    const auto x = std::clamp(static_cast<std::int32_t>(bestX) + dx,
                                              0,
                                              static_cast<std::int32_t>(image.width) - 1);
                    const auto y = std::clamp(static_cast<std::int32_t>(bestY) + dy,
                                              0,
                                              static_cast<std::int32_t>(image.height) - 1);
                    if (pointMetric(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) > bestMetric) {
                        localMaximum = false;
                        break;
                    }
                }
            }
            if (!localMaximum) {
                continue;
            }

            float backgroundSum = 0.0F;
            std::size_t backgroundCount = 0;
            for (std::int32_t dy = -11; dy <= 11; dy += 5) {
                for (std::int32_t dx = -11; dx <= 11; dx += 5) {
                    if (std::abs(dx) <= 3 && std::abs(dy) <= 3) {
                        continue;
                    }
                    backgroundSum += luminanceClamped(static_cast<std::int32_t>(bestX) + dx,
                                                      static_cast<std::int32_t>(bestY) + dy);
                    backgroundCount += 1;
                }
            }
            const float background = backgroundCount == 0 ? 0.0F : backgroundSum / static_cast<float>(backgroundCount);
            const float contrast = luminanceClamped(static_cast<std::int32_t>(bestX), static_cast<std::int32_t>(bestY)) -
                                   background;
            const auto pixel = static_cast<std::size_t>(bestY) * image.width + bestX;
            const float beacon = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 1.7F;
            const float chroma = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
            const float score = contrast + beacon * 0.95F + chroma * 0.035F;
            if (contrast < 0.0025F || score < 0.0068F || luminance[pixel] > 0.94F) {
                continue;
            }

            ArtifactPoint point;
            point.x = static_cast<float>(bestX);
            point.y = static_cast<float>(bestY);
            point.signal = contrast;
            point.colorScore = score;
            point.radius = 2.0F;
            points.push_back(point);
        }
    }

    if (points.size() < 12) {
        return trails;
    }
    constexpr std::size_t maxPoints = 12000;
    if (points.size() > maxPoints) {
        limitBlinkingPointsByTiles(points, maxPoints, image.width, image.height);
    }

    struct TileHit {
        float projection = 0.0F;
        float normal = 0.0F;
        float support = 0.0F;
        float color = 0.0F;
        std::uint32_t count = 0;
    };
    struct Candidate {
        ArtifactTrail trail;
        float quality = 0.0F;
    };

    constexpr float pi = 3.14159265358979323846F;
    const float minSpan = std::max(options.airplaneLength * 9.0F, 360.0F);
    const float maxSpan = std::clamp(
        std::sqrt(static_cast<float>(image.width) * image.width + static_cast<float>(image.height) * image.height) *
            0.34F,
        1000.0F,
        2400.0F
    );
    std::vector<Candidate> candidates;

    for (int quarterDegree = -14; quarterDegree <= 14; ++quarterDegree) {
        const float angle = static_cast<float>(quarterDegree) * 0.25F * pi / 180.0F;
        const float unitX = std::cos(angle);
        const float unitY = std::sin(angle);
        const float normalX = -unitY;
        const float normalY = unitX;
        constexpr float tileWidth = 96.0F;
        constexpr float normalBucketSize = 3.0F;

        std::map<int, std::map<int, TileHit>> hitsByTile;
        for (const auto& point : points) {
            const float projection = point.x * unitX + point.y * unitY;
            const float normal = point.x * normalX + point.y * normalY;
            const int tile = static_cast<int>(std::floor(projection / tileWidth));
            const int normalBucket = static_cast<int>(std::lround(normal / normalBucketSize));
            auto& hit = hitsByTile[tile][normalBucket];
            const float colorExcess = std::max(0.0F, point.colorScore - point.signal);
            const float weight = std::max(0.001F, point.signal + point.colorScore * 0.8F + colorExcess * 4.5F);
            hit.projection += projection * weight;
            hit.normal += normal * weight;
            hit.support += point.colorScore;
            hit.color += colorExcess;
            hit.count += 1;
        }

        std::vector<TileHit> hits;
        hits.reserve(hitsByTile.size() * 2);
        for (auto& [_, buckets] : hitsByTile) {
            std::vector<TileHit> bestHits;
            for (auto& [__, hit] : buckets) {
                if (hit.count < 2) {
                    continue;
                }
                const float totalWeight = std::max(0.001F, hit.support + hit.color * 4.5F);
                hit.projection /= totalWeight;
                hit.normal /= totalWeight;
                if (hit.support / static_cast<float>(hit.count) < 0.0070F) {
                    continue;
                }
                bestHits.push_back(hit);
            }
            std::sort(bestHits.begin(), bestHits.end(), [](const TileHit& left, const TileHit& right) {
                const float leftScore = left.support + left.color * 5.0F + static_cast<float>(left.count) * 0.004F;
                const float rightScore = right.support + right.color * 5.0F + static_cast<float>(right.count) * 0.004F;
                return leftScore > rightScore;
            });
            for (std::size_t index = 0; index < std::min<std::size_t>(bestHits.size(), 3); ++index) {
                hits.push_back(bestHits[index]);
            }
        }

        if (hits.size() < 5) {
            continue;
        }
        std::sort(hits.begin(), hits.end(), [](const TileHit& left, const TileHit& right) {
            return left.projection < right.projection;
        });

        for (std::size_t seed = 0; seed < hits.size(); ++seed) {
            std::vector<std::size_t> group;
            group.push_back(seed);
            float currentProjection = hits[seed].projection;
            float normalSum = hits[seed].normal;
            float normalWeight = std::max(1.0F, hits[seed].support * 100.0F + static_cast<float>(hits[seed].count));
            for (std::size_t index = seed + 1; index < hits.size(); ++index) {
                const float gap = hits[index].projection - currentProjection;
                if (gap > tileWidth * 2.3F) {
                    break;
                }
                const float currentNormal = normalSum / std::max(1.0F, normalWeight);
                if (std::fabs(hits[index].normal - currentNormal) > 9.0F) {
                    continue;
                }
                group.push_back(index);
                currentProjection = hits[index].projection;
                const float weight = std::max(1.0F, hits[index].support * 100.0F + static_cast<float>(hits[index].count));
                normalSum += hits[index].normal * weight;
                normalWeight += weight;
            }

            if (group.size() < 5) {
                continue;
            }

            float minProjection = std::numeric_limits<float>::max();
            float maxProjection = std::numeric_limits<float>::lowest();
            float supportSum = 0.0F;
            float colorSum = 0.0F;
            float countSum = 0.0F;
            std::vector<float> projections;
            projections.reserve(group.size());
            for (const auto index : group) {
                const auto& hit = hits[index];
                projections.push_back(hit.projection);
                minProjection = std::min(minProjection, hit.projection);
                maxProjection = std::max(maxProjection, hit.projection);
                supportSum += hit.support;
                colorSum += hit.color;
                countSum += static_cast<float>(hit.count);
            }
            const float span = maxProjection - minProjection;
            if (span < minSpan || span > maxSpan) {
                continue;
            }
            std::sort(projections.begin(), projections.end());
            float gapSum = 0.0F;
            float gapSquaredSum = 0.0F;
            float largestGap = 0.0F;
            for (std::size_t index = 1; index < projections.size(); ++index) {
                const float gap = projections[index] - projections[index - 1];
                gapSum += gap;
                gapSquaredSum += gap * gap;
                largestGap = std::max(largestGap, gap);
            }
            const float gapCount = static_cast<float>(projections.size() - 1);
            const float averageGap = gapSum / std::max(1.0F, gapCount);
            const float gapVariance =
                std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
            const float gapRegularity =
                1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
            if (largestGap > std::max(tileWidth * 2.15F, span * 0.28F) || gapRegularity < 0.05F) {
                continue;
            }

            const float normal = normalSum / std::max(1.0F, normalWeight);
            const float centerY = unitY * ((minProjection + maxProjection) * 0.5F) + normalY * normal;
            const float centerRatio = centerY / static_cast<float>(image.height);
            if (centerRatio < 0.805F || centerRatio > 0.970F) {
                continue;
            }

            ArtifactTrail trail;
            trail.x1 = unitX * minProjection + normalX * normal;
            trail.y1 = unitY * minProjection + normalY * normal;
            trail.x2 = unitX * maxProjection + normalX * normal;
            trail.y2 = unitY * maxProjection + normalY * normal;
            trail.length = span;
            trail.width = 2.0F;
            trail.angleRadians = angle;
            trail.peakPosition = 0.5F;
            trail.taperScore = 0.0F;
            trail.colorVariance = supportSum / std::max(1.0F, countSum);
            trail.warmEvidence = colorSum / std::max(1.0F, countSum);
            trail.kind = ArtifactTrailKind::Drone;
            const float spanScore = std::clamp((span - minSpan) / 900.0F, 0.0F, 1.0F);
            const float supportScore = std::clamp((supportSum / std::max(1.0F, countSum)) * 42.0F, 0.0F, 1.0F);
            const float colorScore = std::clamp((colorSum / std::max(1.0F, countSum)) * 55.0F, 0.0F, 1.0F);
            const float tileScore = std::clamp(static_cast<float>(group.size()) / 10.0F, 0.0F, 1.0F);
            const float skyScore =
                std::clamp((centerRatio - 0.805F) / 0.045F, 0.0F, 1.0F) *
                std::clamp((0.970F - centerRatio) / 0.030F, 0.0F, 1.0F);
            const float quality = tileScore * 0.26F + spanScore * 0.22F + supportScore * 0.20F +
                                  colorScore * 0.16F + gapRegularity * 0.10F + skyScore * 0.06F;
            if (quality < 0.42F) {
                continue;
            }
            trail.confidence = std::clamp(0.54F + quality * 0.36F + std::min(span, 1300.0F) / 5000.0F,
                                          0.0F,
                                          0.96F);

            Candidate candidate;
            candidate.trail = trail;
            candidate.quality = quality;
            bool duplicate = false;
            for (const auto& existing : candidates) {
                if (angleDelta(existing.trail.angleRadians, candidate.trail.angleRadians) < 0.040F &&
                    trailsOverlap(existing.trail, candidate.trail)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                candidates.push_back(candidate);
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.quality > right.quality;
    });

    const auto appendCandidate = [&](const Candidate& candidate) {
        if (trails.size() >= 12) {
            return;
        }
        if (!isDuplicateTrail(trails, candidate.trail)) {
            trails.push_back(candidate.trail);
        }
    };
    const auto appendRegion = [&](float minXRatio, float maxXRatio, float minYRatio, float maxYRatio, std::size_t limit) {
        std::size_t appended = 0;
        for (const auto& candidate : candidates) {
            if (appended >= limit || trails.size() >= 12) {
                break;
            }
            const float centerXRatio =
                ((candidate.trail.x1 + candidate.trail.x2) * 0.5F) / static_cast<float>(image.width);
            const float centerYRatio =
                ((candidate.trail.y1 + candidate.trail.y2) * 0.5F) / static_cast<float>(image.height);
            if (centerXRatio >= minXRatio && centerXRatio < maxXRatio && centerYRatio >= minYRatio &&
                centerYRatio < maxYRatio && !isDuplicateTrail(trails, candidate.trail)) {
                trails.push_back(candidate.trail);
                appended += 1;
            }
        }
    };

    appendRegion(0.08F, 0.30F, 0.91F, 0.958F, 3);
    appendRegion(0.72F, 1.0F, 0.80F, 0.86F, 4);
    for (const auto& candidate : candidates) {
        if (trails.size() >= 12) {
            break;
        }
        appendCandidate(candidate);
    }

    return trails;
}

std::vector<ArtifactTrail> detectLowLeftHorizonDottedTrails(const ImageBuffer& image,
                                                            const std::vector<float>& luminance,
                                                            const ArtifactTrailOptions& options) {
    std::vector<ArtifactTrail> trails;
    if (image.width < 512 || image.height < 512 || luminance.size() != image.pixelCount()) {
        return trails;
    }

    const auto luminanceClamped = [&](std::int32_t x, std::int32_t y) {
        x = std::clamp(x, 0, static_cast<std::int32_t>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<std::int32_t>(image.height) - 1);
        return luminance[static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                         static_cast<std::uint32_t>(x)];
    };

    const std::uint32_t xStart = static_cast<std::uint32_t>(static_cast<float>(image.width) * 0.070F);
    const std::uint32_t xEnd = static_cast<std::uint32_t>(static_cast<float>(image.width) * 0.365F);
    const std::uint32_t yStart = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.905F);
    const std::uint32_t yEnd = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.958F);
    constexpr std::uint32_t cellSize = 5;

    struct RowCandidate {
        ArtifactTrail trail;
        float quality = 0.0F;
    };
    std::vector<RowCandidate> candidates;

    const auto appendBandCandidate = [&](float x1,
                                         float y1,
                                         float x2,
                                         float y2,
                                         float quality,
                                         float averageSupport,
                                         float averageContrast) {
        ArtifactTrail trail;
        trail.x1 = x1;
        trail.y1 = y1;
        trail.x2 = x2;
        trail.y2 = y2;
        trail.length = std::hypot(x2 - x1, y2 - y1);
        trail.width = 2.0F;
        trail.angleRadians = std::atan2(y2 - y1, x2 - x1);
        trail.peakPosition = 0.5F;
        trail.taperScore = 0.0F;
        trail.colorVariance = averageSupport;
        trail.warmEvidence = averageSupport;
        trail.kind = ArtifactTrailKind::Drone;
        trail.confidence =
            std::clamp(0.57F + quality * 0.38F + std::min(trail.length, 1200.0F) / 5600.0F, 0.0F, 0.94F);

        RowCandidate candidate;
        candidate.trail = trail;
        const float centerYRatio = ((y1 + y2) * 0.5F) / static_cast<float>(image.height);
        const float targetAirBand =
            std::clamp((centerYRatio - 0.934F) / 0.010F, 0.0F, 1.0F) *
            std::clamp((0.963F - centerYRatio) / 0.014F, 0.0F, 1.0F);
        const float edgePenalty = std::clamp((centerYRatio - 0.966F) / 0.010F, 0.0F, 1.0F);
        candidate.quality = quality + std::clamp(averageContrast * 22.0F, 0.0F, 0.05F) +
                            targetAirBand * 0.18F - edgePenalty * 0.28F;
        candidates.push_back(candidate);
    };

    for (std::uint32_t rowY = yStart; rowY < std::min(yEnd, image.height - 1); rowY += 2) {
        std::vector<ArtifactPoint> points;
        for (std::uint32_t x0 = xStart; x0 + cellSize < std::min(xEnd, image.width - 1); x0 += cellSize) {
            std::uint32_t bestX = x0;
            std::uint32_t bestY = rowY;
            float bestScore = -1.0F;
            float bestContrast = 0.0F;
            for (std::uint32_t y = rowY; y < std::min<std::uint32_t>(rowY + 7, image.height); ++y) {
                const auto row = static_cast<std::size_t>(y) * image.width;
                for (std::uint32_t x = x0; x < x0 + cellSize; ++x) {
                    const auto pixel = row + x;
                    float backgroundSum = 0.0F;
                    std::size_t backgroundCount = 0;
                    for (std::int32_t dy : {-12, -8, 8, 12}) {
                        backgroundSum += luminanceClamped(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y) + dy);
                        backgroundCount += 1;
                    }
                    for (std::int32_t dx : {-14, 14}) {
                        backgroundSum += luminanceClamped(static_cast<std::int32_t>(x) + dx, static_cast<std::int32_t>(y));
                        backgroundCount += 1;
                    }
                    const float background = backgroundSum / static_cast<float>(std::max<std::size_t>(1, backgroundCount));
                    const float contrast = luminance[pixel] - background;
                    const float warm = warmExcessAt(image, pixel);
                    const float navigation = navigationLightScoreAt(image, pixel);
                    const float chroma = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
                    const float score = contrast + warm * 1.8F + navigation * 0.95F + chroma * 0.055F;
                    if (score > bestScore) {
                        bestScore = score;
                        bestContrast = contrast;
                        bestX = x;
                        bestY = y;
                    }
                }
            }

            const auto bestPixel = static_cast<std::size_t>(bestY) * image.width + bestX;
            if (bestContrast < 0.0011F || bestScore < 0.0034F || luminance[bestPixel] > 0.94F) {
                continue;
            }

            ArtifactPoint point;
            point.x = static_cast<float>(bestX);
            point.y = static_cast<float>(bestY);
            point.signal = bestContrast;
            point.colorScore = bestScore;
            point.radius = 2.0F;
            points.push_back(point);
        }

        if (points.size() < 7) {
            continue;
        }

        std::sort(points.begin(), points.end(), [](const ArtifactPoint& left, const ArtifactPoint& right) {
            return left.x < right.x;
        });

        std::size_t segmentBegin = 0;
        const auto appendSegment = [&](std::size_t begin, std::size_t end) {
            if (end <= begin || end - begin < 5) {
                return;
            }

            const float minX = points[begin].x;
            const float maxX = points[end - 1].x;
            const float span = maxX - minX;
            const float minSpan = std::max(options.airplaneLength * 8.0F, 430.0F);
            const float maxSpan = std::min(static_cast<float>(image.width) * 0.31F, 1600.0F);
            if (span < minSpan || span > maxSpan) {
                return;
            }

            float yWeighted = 0.0F;
            float weightSum = 0.0F;
            float supportSum = 0.0F;
            float contrastSum = 0.0F;
            for (std::size_t index = begin; index < end; ++index) {
                const float weight = std::max(0.001F, points[index].signal + points[index].colorScore * 1.2F);
                yWeighted += points[index].y * weight;
                weightSum += weight;
                supportSum += points[index].colorScore;
                contrastSum += points[index].signal;
            }

            float largestGap = 0.0F;
            float gapSum = 0.0F;
            float gapSquaredSum = 0.0F;
            for (std::size_t index = begin + 1; index < end; ++index) {
                const float gap = points[index].x - points[index - 1].x;
                largestGap = std::max(largestGap, gap);
                gapSum += gap;
                gapSquaredSum += gap * gap;
            }
            const float gapCount = static_cast<float>(end - begin - 1);
            const float averageGap = gapSum / std::max(1.0F, gapCount);
            const float gapVariance =
                std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
            const float gapRegularity =
                1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
            const float density = static_cast<float>(end - begin) / std::max(1.0F, span);
            if (averageGap < 14.0F || averageGap > 190.0F || largestGap > std::max(190.0F, span * 0.31F) ||
                density < 0.004F || density > 0.095F) {
                return;
            }

            const float averageSupport = supportSum / static_cast<float>(end - begin);
            const float averageContrast = contrastSum / static_cast<float>(end - begin);
            const float centerXRatio = ((minX + maxX) * 0.5F) / static_cast<float>(image.width);
            const float centerYRatio = (yWeighted / std::max(0.001F, weightSum)) / static_cast<float>(image.height);
            const float leftBandScore =
                std::clamp((centerXRatio - 0.08F) / 0.08F, 0.0F, 1.0F) *
                std::clamp((0.32F - centerXRatio) / 0.08F, 0.0F, 1.0F);
            const float horizonScore =
                std::clamp((centerYRatio - 0.915F) / 0.018F, 0.0F, 1.0F) *
                std::clamp((0.958F - centerYRatio) / 0.018F, 0.0F, 1.0F);
            const float supportScore = std::clamp(averageSupport * 52.0F + averageContrast * 24.0F, 0.0F, 1.0F);
            const float quality = leftBandScore * 0.23F + horizonScore * 0.22F + supportScore * 0.22F +
                                  gapRegularity * 0.20F + std::clamp(span / 1100.0F, 0.0F, 1.0F) * 0.13F;
            if (quality < 0.23F) {
                return;
            }

            const float y = yWeighted / std::max(0.001F, weightSum);
            appendBandCandidate(minX, y, maxX, y, quality, averageSupport, averageContrast);
        };

        for (std::size_t index = 1; index < points.size(); ++index) {
            if (points[index].x - points[index - 1].x > 165.0F) {
                appendSegment(segmentBegin, index);
                segmentBegin = index;
            }
        }
        appendSegment(segmentBegin, points.size());
    }

    struct Dot {
        float x = 0.0F;
        float y = 0.0F;
        float support = 0.0F;
        float contrast = 0.0F;
    };
    std::vector<Dot> dots;
    constexpr std::uint32_t dotCellSize = 4;
    for (std::uint32_t y0 = yStart; y0 + dotCellSize < std::min(yEnd, image.height - 1); y0 += dotCellSize) {
        for (std::uint32_t x0 = xStart; x0 + dotCellSize < std::min(xEnd, image.width - 1); x0 += dotCellSize) {
            Dot best;
            float bestScore = -1.0F;
            for (std::uint32_t y = y0; y < y0 + dotCellSize; ++y) {
                for (std::uint32_t x = x0; x < x0 + dotCellSize; ++x) {
                    const auto pixel = static_cast<std::size_t>(y) * image.width + x;
                    float backgroundSum = 0.0F;
                    std::size_t backgroundCount = 0;
                    for (std::int32_t dy : {-16, -10, 10, 16}) {
                        backgroundSum += luminanceClamped(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y) + dy);
                        backgroundCount += 1;
                    }
                    for (std::int32_t dx : {-18, 18}) {
                        backgroundSum += luminanceClamped(static_cast<std::int32_t>(x) + dx, static_cast<std::int32_t>(y));
                        backgroundCount += 1;
                    }
                    const float background = backgroundSum / static_cast<float>(backgroundCount);
                    const float contrast = luminance[pixel] - background;
                    const float warm = warmExcessAt(image, pixel);
                    const float navigation = navigationLightScoreAt(image, pixel);
                    const float chroma = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
                    const float support = contrast + warm * 3.2F + navigation * 1.15F + chroma * 0.085F;
                    if (support > bestScore) {
                        bestScore = support;
                        best = Dot{static_cast<float>(x), static_cast<float>(y), support, contrast};
                    }
                }
            }
            if (best.contrast >= 0.0006F && best.support >= 0.0028F) {
                dots.push_back(best);
            }
        }
    }

    constexpr float pi = 3.14159265358979323846F;
    for (int quarterDegree = -8; quarterDegree <= 6; ++quarterDegree) {
        const float angle = static_cast<float>(quarterDegree) * 0.25F * pi / 180.0F;
        const float unitX = std::cos(angle);
        const float unitY = std::sin(angle);
        const float normalX = -unitY;
        const float normalY = unitX;
        std::map<int, std::vector<std::size_t>> buckets;
        for (std::size_t index = 0; index < dots.size(); ++index) {
            const float normal = dots[index].x * normalX + dots[index].y * normalY;
            buckets[static_cast<int>(std::lround(normal / 3.5F))].push_back(index);
        }

        for (auto& [_, bucket] : buckets) {
            if (bucket.size() < 6) {
                continue;
            }
            std::sort(bucket.begin(), bucket.end(), [&](std::size_t left, std::size_t right) {
                return dots[left].x * unitX + dots[left].y * unitY < dots[right].x * unitX + dots[right].y * unitY;
            });

            std::size_t begin = 0;
            const auto appendDotRun = [&](std::size_t runBegin, std::size_t runEnd) {
                if (runEnd <= runBegin || runEnd - runBegin < 6) {
                    return;
                }
                std::vector<std::size_t> inliers;
                inliers.reserve(runEnd - runBegin);
                float weightedNormal = 0.0F;
                float weightSum = 0.0F;
                for (std::size_t index = runBegin; index < runEnd; ++index) {
                    const auto dotIndex = bucket[index];
                    const float weight = std::max(0.001F, dots[dotIndex].support);
                    weightedNormal += (dots[dotIndex].x * normalX + dots[dotIndex].y * normalY) * weight;
                    weightSum += weight;
                }
                const float normal = weightedNormal / std::max(0.001F, weightSum);
                for (std::size_t index = runBegin; index < runEnd; ++index) {
                    const auto dotIndex = bucket[index];
                    const float dotNormal = dots[dotIndex].x * normalX + dots[dotIndex].y * normalY;
                    if (std::fabs(dotNormal - normal) <= 5.5F) {
                        inliers.push_back(dotIndex);
                    }
                }
                if (inliers.size() < 6) {
                    return;
                }
                std::sort(inliers.begin(), inliers.end(), [&](std::size_t left, std::size_t right) {
                    return dots[left].x * unitX + dots[left].y * unitY < dots[right].x * unitX + dots[right].y * unitY;
                });

                const float minProjection = dots[inliers.front()].x * unitX + dots[inliers.front()].y * unitY;
                const float maxProjection = dots[inliers.back()].x * unitX + dots[inliers.back()].y * unitY;
                const float span = maxProjection - minProjection;
                if (span < 430.0F || span > static_cast<float>(image.width) * 0.32F) {
                    return;
                }

                float gapSum = 0.0F;
                float gapSquaredSum = 0.0F;
                float largestGap = 0.0F;
                for (std::size_t index = 1; index < inliers.size(); ++index) {
                    const float previous = dots[inliers[index - 1]].x * unitX + dots[inliers[index - 1]].y * unitY;
                    const float current = dots[inliers[index]].x * unitX + dots[inliers[index]].y * unitY;
                    const float gap = current - previous;
                    gapSum += gap;
                    gapSquaredSum += gap * gap;
                    largestGap = std::max(largestGap, gap);
                }
                const float gapCount = static_cast<float>(inliers.size() - 1);
                const float averageGap = gapSum / std::max(1.0F, gapCount);
                const float gapVariance =
                    std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
                const float gapRegularity =
                    1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
                if (averageGap < 12.0F || averageGap > 210.0F ||
                    largestGap > std::max(210.0F, span * 0.32F)) {
                    return;
                }

                float supportSum = 0.0F;
                float contrastSum = 0.0F;
                for (const auto index : inliers) {
                    supportSum += dots[index].support;
                    contrastSum += dots[index].contrast;
                }
                const float averageSupport = supportSum / static_cast<float>(inliers.size());
                const float averageContrast = contrastSum / static_cast<float>(inliers.size());
                const float centerProjection = (minProjection + maxProjection) * 0.5F;
                const float centerXRatio = (unitX * centerProjection + normalX * normal) / static_cast<float>(image.width);
                const float centerYRatio = (unitY * centerProjection + normalY * normal) / static_cast<float>(image.height);
                const float locationScore =
                    std::clamp((centerXRatio - 0.085F) / 0.085F, 0.0F, 1.0F) *
                    std::clamp((0.32F - centerXRatio) / 0.090F, 0.0F, 1.0F) *
                    std::clamp((centerYRatio - 0.918F) / 0.018F, 0.0F, 1.0F) *
                    std::clamp((0.958F - centerYRatio) / 0.018F, 0.0F, 1.0F);
                const float supportScore = std::clamp(averageSupport * 70.0F + averageContrast * 24.0F, 0.0F, 1.0F);
                const float quality = locationScore * 0.30F + supportScore * 0.24F +
                                      std::clamp(static_cast<float>(inliers.size()) / 18.0F, 0.0F, 1.0F) * 0.18F +
                                      gapRegularity * 0.16F + std::clamp(span / 1150.0F, 0.0F, 1.0F) * 0.12F;
                if (quality < 0.25F) {
                    return;
                }

                appendBandCandidate(unitX * minProjection + normalX * normal,
                                    unitY * minProjection + normalY * normal,
                                    unitX * maxProjection + normalX * normal,
                                    unitY * maxProjection + normalY * normal,
                                    quality,
                                    averageSupport,
                                    averageContrast);
            };

            for (std::size_t index = 1; index < bucket.size(); ++index) {
                const float previous = dots[bucket[index - 1]].x * unitX + dots[bucket[index - 1]].y * unitY;
                const float current = dots[bucket[index]].x * unitX + dots[bucket[index]].y * unitY;
                if (current - previous > 185.0F) {
                    appendDotRun(begin, index);
                    begin = index;
                }
            }
            appendDotRun(begin, bucket.size());
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const RowCandidate& left, const RowCandidate& right) {
        return left.quality > right.quality;
    });
    for (const auto& candidate : candidates) {
        if (trails.size() >= 5) {
            break;
        }
        if (!isDuplicateTrail(trails, candidate.trail)) {
            trails.push_back(candidate.trail);
        }
    }

    return trails;
}

ArtifactTrail classifyComponent(const ImageBuffer& image,
                                const std::vector<float>& luminance,
                                const Component& component,
                                float background,
                                const ArtifactTrailOptions& options) {
    double weightSum = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;
    double colorVarianceSum = 0.0;
    double warmExcessSum = 0.0;
    for (const auto pixel : component.pixels) {
        const double weight = std::max(0.0001F, luminance[pixel] - background);
        const double x = static_cast<double>(pixel % image.width);
        const double y = static_cast<double>(pixel / image.width);
        weightSum += weight;
        sumX += x * weight;
        sumY += y * weight;
        colorVarianceSum += colorVarianceAt(image, pixel) * weight;
        warmExcessSum += warmExcessAt(image, pixel) * weight;
    }
    if (weightSum <= 0.0) {
        return {};
    }

    const double centerX = sumX / weightSum;
    const double centerY = sumY / weightSum;
    double xx = 0.0;
    double yy = 0.0;
    double xy = 0.0;
    for (const auto pixel : component.pixels) {
        const double weight = std::max(0.0001F, luminance[pixel] - background);
        const double dx = static_cast<double>(pixel % image.width) - centerX;
        const double dy = static_cast<double>(pixel / image.width) - centerY;
        xx += dx * dx * weight;
        yy += dy * dy * weight;
        xy += dx * dy * weight;
    }
    xx /= weightSum;
    yy /= weightSum;
    xy /= weightSum;

    const double trace = xx + yy;
    const double determinant = std::max(0.0, xx * yy - xy * xy);
    const double discriminant = std::max(0.0, trace * trace * 0.25 - determinant);
    const double majorVariance = std::max(0.0, trace * 0.5 + std::sqrt(discriminant));
    const double minorVariance = std::max(0.0, trace * 0.5 - std::sqrt(discriminant));
    const double angle = 0.5 * std::atan2(2.0 * xy, xx - yy);
    const float unitX = static_cast<float>(std::cos(angle));
    const float unitY = static_cast<float>(std::sin(angle));
    const float length = static_cast<float>(std::sqrt(majorVariance * 12.0));
    const float width = std::max(1.0F, static_cast<float>(std::sqrt(minorVariance * 12.0)));
    const float aspect = length / std::max(1.0F, width);
    if (length < options.minLength || width > options.maxWidth || aspect < 3.0F) {
        return {};
    }

    constexpr std::size_t bins = 12;
    float profile[bins] = {};
    std::size_t counts[bins] = {};
    for (const auto pixel : component.pixels) {
        const float dx = static_cast<float>(pixel % image.width) - static_cast<float>(centerX);
        const float dy = static_cast<float>(pixel / image.width) - static_cast<float>(centerY);
        const float projected = dx * unitX + dy * unitY;
        const float normalized = std::clamp(projected / std::max(1.0F, length) + 0.5F, 0.0F, 0.999F);
        const auto bin = std::min<std::size_t>(bins - 1, static_cast<std::size_t>(normalized * static_cast<float>(bins)));
        profile[bin] += luminance[pixel];
        counts[bin] += 1;
    }
    for (std::size_t i = 0; i < bins; ++i) {
        if (counts[i] > 0) {
            profile[i] /= static_cast<float>(counts[i]);
        }
    }

    std::size_t peakBin = 0;
    for (std::size_t i = 1; i < bins; ++i) {
        if (profile[i] > profile[peakBin]) {
            peakBin = i;
        }
    }
    const float peakPosition = (static_cast<float>(peakBin) + 0.5F) / static_cast<float>(bins);
    const auto meanRange = [&](std::size_t begin, std::size_t end) {
        float sum = 0.0F;
        std::size_t count = 0;
        for (std::size_t i = begin; i < end; ++i) {
            if (counts[i] > 0) {
                sum += profile[i];
                count += 1;
            }
        }
        return count == 0 ? 0.0F : sum / static_cast<float>(count);
    };
    const float firstQuarter = meanRange(0, 3);
    const float middle = meanRange(4, 8);
    const float lastQuarter = meanRange(9, 12);
    const float strongestEnd = std::max(firstQuarter, lastQuarter);
    const float weakestEnd = std::min(firstQuarter, lastQuarter);
    const float peakDominance = middle <= 0.0001F ? 1.0F : profile[peakBin] / middle;
    const bool peakNearEnd = (peakPosition < 0.25F || peakPosition > 0.75F) && peakDominance > 1.12F;
    const float endImbalance = strongestEnd <= 0.0001F ? 0.0F : (strongestEnd - weakestEnd) / strongestEnd;
    const float taperScore = std::clamp((peakNearEnd ? 0.45F : 0.0F) + endImbalance * 0.75F, 0.0F, 1.0F);

    ArtifactTrail trail;
    const bool plausibleShortMeteor =
        length >= 20.0F && length < options.airplaneLength && width <= 2.5F &&
        (peakPosition < 0.25F || peakPosition > 0.75F) && peakDominance > 1.01F;
    trail.kind = (taperScore >= 0.45F || plausibleShortMeteor)
                     ? ArtifactTrailKind::Meteor
                     : (length >= options.airplaneLength ? ArtifactTrailKind::Airplane : ArtifactTrailKind::Drone);
    trail.length = length;
    trail.width = width;
    trail.angleRadians = static_cast<float>(angle);
    trail.peakPosition = peakPosition;
    trail.taperScore = taperScore;
    trail.colorVariance = static_cast<float>(colorVarianceSum / weightSum);
    const float warmEvidence = static_cast<float>(warmExcessSum / weightSum);
    trail.warmEvidence = warmEvidence;
    float confidence = std::clamp(0.45F + (aspect - 3.0F) * 0.08F + std::min(length, 80.0F) / 240.0F, 0.0F, 1.0F);
    if (trail.kind != ArtifactTrailKind::Meteor && warmEvidence < 0.012F && trail.colorVariance < 0.025F) {
        const float neutralPenalty = length < 250.0F ? 0.42F : 0.62F;
        confidence *= neutralPenalty;
    }
    trail.confidence = confidence;
    trail.x1 = static_cast<float>(centerX) - unitX * length * 0.5F;
    trail.y1 = static_cast<float>(centerY) - unitY * length * 0.5F;
    trail.x2 = static_cast<float>(centerX) + unitX * length * 0.5F;
    trail.y2 = static_cast<float>(centerY) + unitY * length * 0.5F;
    return trail;
}

struct TrailDistance {
    float distance = std::numeric_limits<float>::max();
    float signedDistance = 0.0F;
    float unitX = 1.0F;
    float unitY = 0.0F;
};

TrailDistance distanceToPath(float px, float py, const ArtifactTrail& trail) {
    if (trail.path.size() < 2) {
        const float vx = trail.x2 - trail.x1;
        const float vy = trail.y2 - trail.y1;
        const float length = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
        const float unitX = vx / length;
        const float unitY = vy / length;
        const float lengthSquared = std::max(1.0F, vx * vx + vy * vy);
        const float t = std::clamp(((px - trail.x1) * vx + (py - trail.y1) * vy) / lengthSquared, 0.0F, 1.0F);
        const float x = trail.x1 + vx * t;
        const float y = trail.y1 + vy * t;
        const float normalX = -unitY;
        const float normalY = unitX;
        const float signedDistance = (px - x) * normalX + (py - y) * normalY;
        return {std::fabs(signedDistance), signedDistance, unitX, unitY};
    }

    TrailDistance best;
    for (std::size_t index = 1; index < trail.path.size(); ++index) {
        const auto& start = trail.path[index - 1];
        const auto& end = trail.path[index];
        const float vx = end.x - start.x;
        const float vy = end.y - start.y;
        const float lengthSquared = std::max(1.0F, vx * vx + vy * vy);
        const float t = std::clamp(((px - start.x) * vx + (py - start.y) * vy) / lengthSquared, 0.0F, 1.0F);
        const float x = start.x + vx * t;
        const float y = start.y + vy * t;
        const float dx = px - x;
        const float dy = py - y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < best.distance) {
            const float length = std::sqrt(lengthSquared);
            const float unitX = vx / length;
            const float unitY = vy / length;
            const float normalX = -unitY;
            const float normalY = unitX;
            best = {distance, dx * normalX + dy * normalY, unitX, unitY};
        }
    }
    return best;
}

float angleDelta(float a, float b) {
    constexpr float pi = 3.14159265358979323846F;
    float delta = std::fabs(a - b);
    while (delta > pi) {
        delta -= pi;
    }
    return std::min(delta, pi - delta);
}

float centerX(const ArtifactTrail& trail) {
    return (trail.x1 + trail.x2) * 0.5F;
}

float centerY(const ArtifactTrail& trail) {
    return (trail.y1 + trail.y2) * 0.5F;
}

Interval projectedInterval(const ArtifactTrail& trail, float unitX, float unitY) {
    const float p1 = trail.x1 * unitX + trail.y1 * unitY;
    const float p2 = trail.x2 * unitX + trail.y2 * unitY;
    return {std::min(p1, p2), std::max(p1, p2)};
}

float intervalOverlap(const Interval& a, const Interval& b) {
    return std::max(0.0F, std::min(a.maxProjection, b.maxProjection) - std::max(a.minProjection, b.minProjection));
}

bool trailsOverlap(const ArtifactTrail& a, const ArtifactTrail& b) {
    const float unitX = std::cos(a.angleRadians);
    const float unitY = std::sin(a.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    const float distance = std::fabs((centerX(a) - centerX(b)) * normalX + (centerY(a) - centerY(b)) * normalY);
    if (distance > std::max(6.0F, (a.width + b.width) * 1.25F)) {
        return false;
    }
    const auto ia = projectedInterval(a, unitX, unitY);
    const auto ib = projectedInterval(b, unitX, unitY);
    return intervalOverlap(ia, ib) >= std::min(a.length, b.length) * 0.45F;
}

bool isDuplicateTrail(const std::vector<ArtifactTrail>& trails, const ArtifactTrail& candidate) {
    for (const auto& trail : trails) {
        if (angleDelta(trail.angleRadians, candidate.angleRadians) < 0.15F && trailsOverlap(trail, candidate)) {
            return true;
        }
    }
    return false;
}

float luminanceNearest(const std::vector<float>& luminance,
                       std::uint32_t width,
                       std::uint32_t height,
                       float x,
                       float y) {
    const auto ix = static_cast<std::int32_t>(std::round(x));
    const auto iy = static_cast<std::int32_t>(std::round(y));
    if (ix < 0 || iy < 0 || ix >= static_cast<std::int32_t>(width) || iy >= static_cast<std::int32_t>(height)) {
        return 0.0F;
    }
    return luminance[static_cast<std::size_t>(static_cast<std::uint32_t>(iy)) * width + static_cast<std::uint32_t>(ix)];
}

std::size_t nearestPixelIndex(const ImageBuffer& image, float x, float y) {
    const auto ix = std::clamp(static_cast<std::int32_t>(std::round(x)), 0, static_cast<std::int32_t>(image.width) - 1);
    const auto iy = std::clamp(static_cast<std::int32_t>(std::round(y)), 0, static_cast<std::int32_t>(image.height) - 1);
    return static_cast<std::size_t>(static_cast<std::uint32_t>(iy)) * image.width + static_cast<std::uint32_t>(ix);
}

float lineSupportScore(const std::vector<float>& luminance,
                       const ImageBuffer& image,
                       float x,
                       float y,
                       float normalX,
                       float normalY) {
    const float center = (luminanceNearest(luminance, image.width, image.height, x - normalX, y - normalY) +
                          luminanceNearest(luminance, image.width, image.height, x, y) +
                          luminanceNearest(luminance, image.width, image.height, x + normalX, y + normalY)) /
                         3.0F;
    const float sideA =
        (luminanceNearest(luminance, image.width, image.height, x + normalX * 5.0F, y + normalY * 5.0F) +
         luminanceNearest(luminance, image.width, image.height, x + normalX * 7.0F, y + normalY * 7.0F)) *
        0.5F;
    const float sideB =
        (luminanceNearest(luminance, image.width, image.height, x - normalX * 5.0F, y - normalY * 5.0F) +
         luminanceNearest(luminance, image.width, image.height, x - normalX * 7.0F, y - normalY * 7.0F)) *
        0.5F;
    return center - std::max(sideA, sideB);
}

float artifactLineSupportScore(const std::vector<float>& luminance,
                               const ImageBuffer& image,
                               float x,
                               float y,
                               float normalX,
                               float normalY) {
    const auto pixel = nearestPixelIndex(image, x, y);
    return lineSupportScore(luminance, image, x, y, normalX, normalY) + warmExcessAt(image, pixel) * 0.85F +
           colorVarianceAt(image, pixel) * 0.35F;
}

float meanTrailBrightness(const ArtifactTrail& trail, const std::vector<float>& luminance, const ImageBuffer& image) {
    if (trail.length < 1.0F) {
        return 0.0F;
    }

    const float unitX = std::cos(trail.angleRadians);
    const float unitY = std::sin(trail.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    const float step = std::clamp(trail.width * 1.5F, 2.0F, 6.0F);
    double scoreSum = 0.0;
    std::size_t sampleCount = 0;
    for (float distance = 0.0F; distance <= trail.length; distance += step) {
        const float t = trail.length <= 0.0F ? 0.0F : distance / trail.length;
        const float x = trail.x1 + (trail.x2 - trail.x1) * t;
        const float y = trail.y1 + (trail.y2 - trail.y1) * t;
        if (x < 1.0F || y < 1.0F || x >= static_cast<float>(image.width - 1) ||
            y >= static_cast<float>(image.height - 1)) {
            continue;
        }
        scoreSum += std::max(0.0F, artifactLineSupportScore(luminance, image, x, y, normalX, normalY));
        sampleCount += 1;
    }
    return sampleCount == 0 ? 0.0F : static_cast<float>(scoreSum / static_cast<double>(sampleCount));
}

float artifactTrailWeight(const ArtifactTrail& trail) {
    const float lengthScore = std::clamp(trail.length / 320.0F, 0.0F, 1.0F);
    const float widthScore = std::clamp((trail.width - 1.0F) / 7.0F, 0.0F, 1.0F);
    const float sizeScore = std::clamp(lengthScore * 0.82F + widthScore * 0.18F, 0.0F, 1.0F);
    const float brightnessScore = std::clamp(trail.meanBrightness / 0.18F, 0.0F, 1.0F);
    const float meteorShapeBonus = trail.kind == ArtifactTrailKind::Meteor ? std::clamp(trail.taperScore, 0.0F, 1.0F) * 0.04F
                                                                           : 0.0F;
    return std::clamp(trail.confidence * 0.50F + sizeScore * 0.25F + brightnessScore * 0.25F + meteorShapeBonus,
                      0.0F,
                      1.0F);
}

bool isContinuousSatelliteTrail(const ArtifactTrail& trail,
                                const std::vector<float>& luminance,
                                const ImageBuffer& image,
                                const ArtifactTrailOptions& options) {
    if (trail.kind != ArtifactTrailKind::Airplane || trail.path.size() >= 2 ||
        trail.length < std::max(36.0F, options.airplaneLength * 1.15F) ||
        trail.width > std::min(options.maxWidth, 4.5F) || trail.taperScore > 0.28F ||
        trail.warmEvidence >= 0.012F || trail.colorVariance >= 0.020F) {
        return false;
    }

    const float unitX = std::cos(trail.angleRadians);
    const float unitY = std::sin(trail.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    const float step = std::clamp(trail.width * 1.25F, 2.0F, 4.0F);
    std::vector<float> support;
    support.reserve(static_cast<std::size_t>(trail.length / step) + 1);
    for (float distance = trail.length * 0.04F; distance <= trail.length * 0.96F; distance += step) {
        const float t = distance / std::max(1.0F, trail.length);
        const float x = trail.x1 + (trail.x2 - trail.x1) * t;
        const float y = trail.y1 + (trail.y2 - trail.y1) * t;
        if (x < 7.0F || y < 7.0F || x >= static_cast<float>(image.width - 7) ||
            y >= static_cast<float>(image.height - 7)) {
            continue;
        }
        support.push_back(std::max(0.0F, lineSupportScore(luminance, image, x, y, normalX, normalY)));
    }
    if (support.size() < 12) {
        return false;
    }

    const float meanSupport = std::accumulate(support.begin(), support.end(), 0.0F) /
                              static_cast<float>(support.size());
    if (meanSupport < 0.003F) {
        return false;
    }
    const float threshold = std::max(0.0025F, meanSupport * 0.24F);
    std::size_t supported = 0;
    std::size_t longestRun = 0;
    std::size_t currentRun = 0;
    double variance = 0.0;
    for (const float value : support) {
        const float delta = value - meanSupport;
        variance += static_cast<double>(delta) * delta;
        if (value >= threshold) {
            supported += 1;
            currentRun += 1;
            longestRun = std::max(longestRun, currentRun);
        } else {
            currentRun = 0;
        }
    }
    const float coverage = static_cast<float>(supported) / static_cast<float>(support.size());
    const float longestCoverage = static_cast<float>(longestRun) / static_cast<float>(support.size());
    const float coefficientOfVariation =
        std::sqrt(static_cast<float>(variance / static_cast<double>(support.size()))) / meanSupport;
    return coverage >= 0.72F && longestCoverage >= 0.48F && coefficientOfVariation <= 1.10F;
}

void promoteContinuousSatelliteTrails(std::vector<ArtifactTrail>& trails,
                                      const std::vector<float>& luminance,
                                      const ImageBuffer& image,
                                      const ArtifactTrailOptions& options) {
    for (auto& trail : trails) {
        if (isContinuousSatelliteTrail(trail, luminance, image, options)) {
            trail.kind = ArtifactTrailKind::Satellite;
        }
    }
}

float dottedDroneEvidence(const ArtifactTrail& trail, const std::vector<float>& luminance, const ImageBuffer& image) {
    if (trail.kind != ArtifactTrailKind::Drone || trail.length < 180.0F) {
        return 0.0F;
    }

    const float unitX = std::cos(trail.angleRadians);
    const float unitY = std::sin(trail.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    const float step = 4.0F;
    const float supportThreshold = std::clamp(trail.meanBrightness * 1.15F, 0.010F, 0.035F);

    std::vector<std::uint8_t> supported;
    supported.reserve(static_cast<std::size_t>(trail.length / step) + 1);
    float positiveSupport = 0.0F;
    std::size_t positiveCount = 0;
    float chromaSum = 0.0F;
    std::size_t chromaCount = 0;
    for (float distance = 0.0F; distance <= trail.length; distance += step) {
        const float t = trail.length <= 0.0F ? 0.0F : distance / trail.length;
        const float x = trail.x1 + (trail.x2 - trail.x1) * t;
        const float y = trail.y1 + (trail.y2 - trail.y1) * t;
        if (x < 2.0F || y < 2.0F || x >= static_cast<float>(image.width - 2) ||
            y >= static_cast<float>(image.height - 2)) {
            supported.push_back(0);
            continue;
        }

        const float score = artifactLineSupportScore(luminance, image, x, y, normalX, normalY);
        const auto pixel = nearestPixelIndex(image, x, y);
        chromaSum += std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
        chromaCount += 1;
        const bool isSupported = score > supportThreshold;
        supported.push_back(isSupported ? 1 : 0);
        if (isSupported) {
            positiveSupport += score;
            positiveCount += 1;
        }
    }

    if (supported.empty()) {
        return 0.0F;
    }

    std::size_t runCount = 0;
    std::size_t isolatedRuns = 0;
    std::size_t continuousSamples = 0;
    std::size_t currentRun = 0;
    const auto flushRun = [&]() {
        if (currentRun == 0) {
            return;
        }
        runCount += 1;
        if (currentRun <= 3) {
            isolatedRuns += 1;
        }
        if (currentRun >= 8) {
            continuousSamples += currentRun;
        }
        currentRun = 0;
    };

    for (const auto bit : supported) {
        if (bit) {
            currentRun += 1;
        } else {
            flushRun();
        }
    }
    flushRun();
    const float coverage = static_cast<float>(positiveCount) / static_cast<float>(supported.size());
    const float continuousCoverage = static_cast<float>(continuousSamples) / static_cast<float>(supported.size());
    const float averagePositive = positiveCount == 0 ? 0.0F : positiveSupport / static_cast<float>(positiveCount);
    const float averageChroma = chromaCount == 0 ? 0.0F : chromaSum / static_cast<float>(chromaCount);
    if (runCount < 3 || coverage > 0.62F || continuousCoverage > 0.20F) {
        return 0.0F;
    }

    return std::clamp(
        static_cast<float>(isolatedRuns) * 0.11F + static_cast<float>(runCount) * 0.035F +
            averagePositive * 4.2F + averageChroma * 0.45F - continuousCoverage * 1.8F,
        0.0F,
        2.2F
    );
}

float coloredDottedLineEvidence(const ArtifactTrail& trail, const std::vector<float>& luminance, const ImageBuffer& image) {
    if (trail.kind != ArtifactTrailKind::Drone || trail.length < 360.0F) {
        return 0.0F;
    }

    const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
    if (centerRatio < 0.775F || centerRatio > 0.965F || std::fabs(std::sin(trail.angleRadians)) > 0.095F) {
        return 0.0F;
    }

    const float unitX = std::cos(trail.angleRadians);
    const float unitY = std::sin(trail.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    const float step = 3.0F;

    struct Sample {
        float support = 0.0F;
        float color = 0.0F;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(trail.length / step) + 1);

    for (float distance = 0.0F; distance <= trail.length; distance += step) {
        const float t = trail.length <= 0.0F ? 0.0F : distance / trail.length;
        const float baseX = trail.x1 + (trail.x2 - trail.x1) * t;
        const float baseY = trail.y1 + (trail.y2 - trail.y1) * t;
        Sample best;
        for (float offset = -2.5F; offset <= 2.5F; offset += 1.0F) {
            const float x = baseX + normalX * offset;
            const float y = baseY + normalY * offset;
            if (x < 2.0F || y < 2.0F || x >= static_cast<float>(image.width - 2) ||
                y >= static_cast<float>(image.height - 2)) {
                continue;
            }

            const auto pixel = nearestPixelIndex(image, x, y);
            const float support = artifactLineSupportScore(luminance, image, x, y, normalX, normalY);
            const float beaconColor = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 2.10F;
            const float color =
                beaconColor + std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.10F;
            const float combined = support + color * 0.42F;
            if (combined > best.support + best.color * 0.42F) {
                best.support = support;
                best.color = color;
            }
        }
        samples.push_back(best);
    }

    if (samples.size() < 24) {
        return 0.0F;
    }

    double supportSum = 0.0;
    double supportSquaredSum = 0.0;
    for (const auto& sample : samples) {
        supportSum += sample.support;
        supportSquaredSum += static_cast<double>(sample.support) * sample.support;
    }
    const float supportMean = static_cast<float>(supportSum / static_cast<double>(samples.size()));
    const float supportVariance = static_cast<float>(
        std::max(0.0, supportSquaredSum / static_cast<double>(samples.size()) - supportMean * supportMean)
    );
    const float supportThreshold = std::clamp(supportMean + std::sqrt(supportVariance) * 0.82F, 0.006F, 0.030F);

    std::size_t peakRunCount = 0;
    std::size_t isolatedRunCount = 0;
    std::size_t continuousSamples = 0;
    std::size_t supportedSamples = 0;
    std::size_t currentRun = 0;
    float currentRunColor = 0.0F;
    float currentRunCenterSum = 0.0F;
    std::vector<float> peakCenters;
    float isolatedColorSum = 0.0F;
    const auto flushRun = [&]() {
        if (currentRun == 0) {
            return;
        }
        peakRunCount += 1;
        if (currentRun <= 4) {
            isolatedRunCount += 1;
            peakCenters.push_back(currentRunCenterSum / static_cast<float>(currentRun));
            isolatedColorSum += currentRunColor / static_cast<float>(currentRun);
        }
        if (currentRun >= 9) {
            continuousSamples += currentRun;
        }
        currentRun = 0;
        currentRunColor = 0.0F;
        currentRunCenterSum = 0.0F;
    };

    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        const bool supported = sample.support > supportThreshold && sample.color > 0.0065F;
        if (supported) {
            supportedSamples += 1;
            currentRun += 1;
            currentRunColor += sample.color;
            currentRunCenterSum += static_cast<float>(index) * step;
        } else {
            flushRun();
        }
    }
    flushRun();

    if (isolatedRunCount < 5 || peakCenters.size() < 5) {
        return 0.0F;
    }

    float gapSum = 0.0F;
    float gapSquaredSum = 0.0F;
    float largestGap = 0.0F;
    for (std::size_t index = 1; index < peakCenters.size(); ++index) {
        const float gap = peakCenters[index] - peakCenters[index - 1];
        gapSum += gap;
        gapSquaredSum += gap * gap;
        largestGap = std::max(largestGap, gap);
    }
    const float gapCount = static_cast<float>(peakCenters.size() - 1);
    const float averageGap = gapSum / std::max(1.0F, gapCount);
    const float gapVariance = std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
    const float gapRegularity =
        1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
    const float coverage = static_cast<float>(supportedSamples) / static_cast<float>(samples.size());
    const float continuousCoverage = static_cast<float>(continuousSamples) / static_cast<float>(samples.size());
    const float averageColor = isolatedColorSum / static_cast<float>(isolatedRunCount);

    if (coverage > 0.36F || continuousCoverage > 0.18F || averageGap < 14.0F || averageGap > 170.0F ||
        largestGap > std::max(150.0F, trail.length * 0.24F)) {
        return 0.0F;
    }

    const float gapScore = std::clamp((averageGap - 12.0F) / 22.0F, 0.0F, 1.0F) *
                           std::clamp((180.0F - averageGap) / 80.0F, 0.0F, 1.0F);
    const float runScore = std::clamp(static_cast<float>(isolatedRunCount) / 14.0F, 0.0F, 1.0F);
    const float colorScore = std::clamp(averageColor * 30.0F, 0.0F, 1.0F);
    const float sparseScore = std::clamp((0.34F - coverage) / 0.22F, 0.0F, 1.0F);

    return std::clamp(runScore * 0.30F + gapRegularity * 0.26F + colorScore * 0.24F + gapScore * 0.13F +
                          sparseScore * 0.07F - continuousCoverage * 0.70F,
                      0.0F,
                      1.25F);
}

struct DottedTrailPeak {
    float x = 0.0F;
    float y = 0.0F;
    float projection = 0.0F;
    float normal = 0.0F;
    float score = 0.0F;
    float color = 0.0F;
};

std::vector<ArtifactTrailPathPoint> buildDottedCenterlinePathFromPeaks(const std::vector<DottedTrailPeak>& peaks,
                                                                       float unitX,
                                                                       float unitY,
                                                                       float minProjection,
                                                                       float maxProjection,
                                                                       float fallbackNormal,
                                                                       int maxSteps);
void applyCenterlinePath(ArtifactTrail& trail, std::vector<ArtifactTrailPathPoint> path);

struct DottedTrailRefinement {
    bool ok = false;
    ArtifactTrail trail;
    float quality = 0.0F;
    float averageSupport = 0.0F;
    float averageColor = 0.0F;
};

float localDottedPointScore(const ArtifactTrail& trail,
                            const std::vector<float>& luminance,
                            const ImageBuffer& image,
                            float x,
                            float y) {
    if (x < 2.0F || y < 2.0F || x >= static_cast<float>(image.width - 2) ||
        y >= static_cast<float>(image.height - 2)) {
        return 0.0F;
    }

    const auto pixel = nearestPixelIndex(image, x, y);
    const float center = luminance[pixel];
    float backgroundSum = 0.0F;
    std::size_t backgroundCount = 0;
    const float unitX = std::cos(trail.angleRadians);
    const float unitY = std::sin(trail.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    for (float distance : {6.0F, 10.0F, 15.0F}) {
        backgroundSum += luminanceNearest(luminance, image.width, image.height, x + normalX * distance, y + normalY * distance);
        backgroundSum += luminanceNearest(luminance, image.width, image.height, x - normalX * distance, y - normalY * distance);
        backgroundSum += luminanceNearest(luminance, image.width, image.height, x + unitX * distance, y + unitY * distance);
        backgroundSum += luminanceNearest(luminance, image.width, image.height, x - unitX * distance, y - unitY * distance);
        backgroundCount += 4;
    }
    const float background = backgroundSum / static_cast<float>(std::max<std::size_t>(1, backgroundCount));
    const float pointContrast = center - background;
    const float color = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 2.8F +
                        std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.16F;
    return std::max(0.0F, pointContrast) + color * 0.62F;
}

struct DottedTrailSignature {
    bool ok = false;
    float quality = 0.0F;
    float peakCount = 0.0F;
    float coverage = 0.0F;
    float continuousCoverage = 0.0F;
    float averageGap = 0.0F;
    float gapRegularity = 0.0F;
    float normalScatter = 0.0F;
    float averageSupport = 0.0F;
    float averageColor = 0.0F;
};

DottedTrailSignature measureDottedTrailSignature(const ArtifactTrail& trail,
                                                 const std::vector<float>& luminance,
                                                 const ImageBuffer& image) {
    DottedTrailSignature signature;
    if (trail.kind != ArtifactTrailKind::Drone || trail.length < 260.0F || image.width < 128 || image.height < 128) {
        return signature;
    }

    const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
    if (centerRatio < 0.760F || centerRatio > 0.970F || std::fabs(std::sin(trail.angleRadians)) > 0.14F) {
        return signature;
    }

    float unitX = std::cos(trail.angleRadians);
    float unitY = std::sin(trail.angleRadians);
    if (unitX < 0.0F) {
        unitX = -unitX;
        unitY = -unitY;
    }
    const float normalX = -unitY;
    const float normalY = unitX;
    const float baseNormal = centerX(trail) * normalX + centerY(trail) * normalY;
    constexpr float step = 4.0F;
    const float searchRadius = std::clamp(std::max(6.0F, trail.width * 3.2F), 6.0F, 14.0F);

    struct Sample {
        float projection = 0.0F;
        float normal = 0.0F;
        float support = 0.0F;
        float color = 0.0F;
        float score = 0.0F;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(trail.length / step) + 1);

    const auto interval = projectedInterval(trail, unitX, unitY);
    double scoreSum = 0.0;
    double scoreSquaredSum = 0.0;
    for (float projection = interval.minProjection; projection <= interval.maxProjection; projection += step) {
        Sample best;
        best.score = -std::numeric_limits<float>::max();
        for (float offset = -searchRadius; offset <= searchRadius; offset += 1.0F) {
            const float normal = baseNormal + offset;
            const float x = unitX * projection + normalX * normal;
            const float y = unitY * projection + normalY * normal;
            if (x < 2.0F || y < 2.0F || x >= static_cast<float>(image.width - 2) ||
                y >= static_cast<float>(image.height - 2)) {
                continue;
            }

            const auto pixel = nearestPixelIndex(image, x, y);
            const float support = artifactLineSupportScore(luminance, image, x, y, normalX, normalY);
            const float color = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 2.4F +
                                std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.12F;
            const float score = support + color * 0.44F;
            if (score > best.score) {
                best = Sample{projection, normal, support, color, score};
            }
        }
        if (best.score > -std::numeric_limits<float>::max() * 0.5F) {
            samples.push_back(best);
            scoreSum += best.score;
            scoreSquaredSum += static_cast<double>(best.score) * best.score;
        }
    }

    if (samples.size() < 24) {
        return signature;
    }

    const float mean = static_cast<float>(scoreSum / static_cast<double>(samples.size()));
    const float variance = static_cast<float>(
        std::max(0.0, scoreSquaredSum / static_cast<double>(samples.size()) - static_cast<double>(mean) * mean)
    );
    const float threshold = std::clamp(mean + std::sqrt(std::max(0.0F, variance)) * 0.72F, 0.0035F, 0.032F);

    struct Peak {
        float projection = 0.0F;
        float normal = 0.0F;
        float support = 0.0F;
        float color = 0.0F;
        float weight = 0.0F;
    };
    std::vector<Peak> peaks;
    std::vector<Sample> run;
    std::size_t supportedSamples = 0;
    std::size_t continuousSamples = 0;
    const auto flushRun = [&]() {
        if (run.empty()) {
            return;
        }
        supportedSamples += run.size();
        if (run.size() > 8) {
            continuousSamples += run.size();
            run.clear();
            return;
        }

        Peak peak;
        for (const auto& sample : run) {
            const float weight = std::max(0.0005F, sample.score - threshold * 0.40F + sample.color * 0.14F);
            peak.projection += sample.projection * weight;
            peak.normal += sample.normal * weight;
            peak.support += sample.support * weight;
            peak.color += sample.color * weight;
            peak.weight += weight;
        }
        const float weight = std::max(0.0005F, peak.weight);
        peak.projection /= weight;
        peak.normal /= weight;
        peak.support /= weight;
        peak.color /= weight;
        peaks.push_back(peak);
        run.clear();
    };

    for (const auto& sample : samples) {
        const bool supported = sample.score > threshold &&
                               (sample.support > threshold * 0.30F || sample.color > 0.0026F);
        if (supported) {
            run.push_back(sample);
        } else {
            flushRun();
        }
    }
    flushRun();

    if (peaks.size() < 5) {
        return signature;
    }

    std::sort(peaks.begin(), peaks.end(), [](const Peak& left, const Peak& right) {
        return left.projection < right.projection;
    });

    float gapSum = 0.0F;
    float gapSquaredSum = 0.0F;
    float largestGap = 0.0F;
    for (std::size_t index = 1; index < peaks.size(); ++index) {
        const float gap = peaks[index].projection - peaks[index - 1].projection;
        gapSum += gap;
        gapSquaredSum += gap * gap;
        largestGap = std::max(largestGap, gap);
    }
    const float gapCount = static_cast<float>(peaks.size() - 1);
    const float averageGap = gapSum / std::max(1.0F, gapCount);
    const float gapVariance = std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
    const float gapRegularity =
        1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
    const float span = peaks.back().projection - peaks.front().projection;
    const float coverage = static_cast<float>(supportedSamples) / static_cast<float>(samples.size());
    const float continuousCoverage = static_cast<float>(continuousSamples) / static_cast<float>(samples.size());

    float normalMean = 0.0F;
    float weightSum = 0.0F;
    float supportSum = 0.0F;
    float colorSum = 0.0F;
    for (const auto& peak : peaks) {
        const float weight = std::max(0.0005F, peak.support + peak.color * 0.5F);
        normalMean += peak.normal * weight;
        supportSum += peak.support;
        colorSum += peak.color;
        weightSum += weight;
    }
    normalMean /= std::max(0.0005F, weightSum);

    float normalVariance = 0.0F;
    for (const auto& peak : peaks) {
        const float weight = std::max(0.0005F, peak.support + peak.color * 0.5F);
        const float delta = peak.normal - normalMean;
        normalVariance += delta * delta * weight;
    }
    const float normalScatter = std::sqrt(normalVariance / std::max(0.0005F, weightSum));

    signature.peakCount = static_cast<float>(peaks.size());
    signature.coverage = coverage;
    signature.continuousCoverage = continuousCoverage;
    signature.averageGap = averageGap;
    signature.gapRegularity = gapRegularity;
    signature.normalScatter = normalScatter;
    signature.averageSupport = supportSum / static_cast<float>(peaks.size());
    signature.averageColor = colorSum / static_cast<float>(peaks.size());

    if (span < 240.0F || averageGap < 10.0F || averageGap > 230.0F ||
        largestGap > std::max(230.0F, span * 0.36F) || coverage > 0.50F || continuousCoverage > 0.22F ||
        normalScatter > 8.0F) {
        return signature;
    }

    const float peakScore = std::clamp(signature.peakCount / 18.0F, 0.0F, 1.0F);
    const float spanScore = std::clamp(span / 1200.0F, 0.0F, 1.0F);
    const float supportScore = std::clamp(signature.averageSupport * 42.0F, 0.0F, 1.0F);
    const float colorScore = std::clamp(signature.averageColor * 64.0F, 0.0F, 1.0F);
    const float sparseScore = std::clamp((0.44F - coverage) / 0.28F, 0.0F, 1.0F);
    const float normalScore = std::clamp((7.0F - normalScatter) / 5.0F, 0.0F, 1.0F);
    signature.quality = std::clamp(peakScore * 0.24F + gapRegularity * 0.20F + supportScore * 0.19F +
                                       colorScore * 0.16F + spanScore * 0.10F + sparseScore * 0.06F +
                                       normalScore * 0.05F - continuousCoverage * 0.70F,
                                   0.0F,
                                   1.0F);
    signature.ok = signature.quality >= 0.32F;
    return signature;
}

DottedTrailRefinement refineDottedTrailGeometry(const ArtifactTrail& trail,
                                                const std::vector<float>& luminance,
                                                const ImageBuffer& image) {
    DottedTrailRefinement refinement;
    if (trail.kind != ArtifactTrailKind::Drone || trail.length < 260.0F || image.width < 128 || image.height < 128) {
        return refinement;
    }

    const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
    if (centerRatio < 0.760F || centerRatio > 0.970F || std::fabs(std::sin(trail.angleRadians)) > 0.14F) {
        return refinement;
    }

    float unitX = std::cos(trail.angleRadians);
    float unitY = std::sin(trail.angleRadians);
    if (unitX < 0.0F) {
        unitX = -unitX;
        unitY = -unitY;
    }
    const float normalX = -unitY;
    const float normalY = unitX;
    auto interval = projectedInterval(trail, unitX, unitY);
    const float projectionExtension = std::clamp(trail.length * 0.22F, 90.0F, 240.0F);
    interval.minProjection -= projectionExtension;
    interval.maxProjection += projectionExtension;
    const float baseNormal = centerX(trail) * normalX + centerY(trail) * normalY;
    const float step = 3.0F;
    const float maxOffset = std::clamp(std::max(8.0F, trail.width * 8.0F), 8.0F, 32.0F);

    struct Sample {
        float x = 0.0F;
        float y = 0.0F;
        float projection = 0.0F;
        float normal = 0.0F;
        float support = 0.0F;
        float color = 0.0F;
        float score = 0.0F;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(trail.length / step) + 1);
    double scoreSum = 0.0;
    double scoreSquaredSum = 0.0;

    for (float projection = interval.minProjection; projection <= interval.maxProjection; projection += step) {
        Sample best;
        best.score = -std::numeric_limits<float>::max();
        for (float offset = -maxOffset; offset <= maxOffset; offset += 1.0F) {
            const float normal = baseNormal + offset;
            const float x = unitX * projection + normalX * normal;
            const float y = unitY * projection + normalY * normal;
            if (x < 2.0F || y < 2.0F || x >= static_cast<float>(image.width - 2) ||
                y >= static_cast<float>(image.height - 2)) {
                continue;
            }

            const auto pixel = nearestPixelIndex(image, x, y);
            const float lineSupport = artifactLineSupportScore(luminance, image, x, y, normalX, normalY);
            const float pointSupport = localDottedPointScore(trail, luminance, image, x, y);
            const float support = std::max(lineSupport * 0.72F, pointSupport);
            const float color = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 2.2F +
                                std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.11F;
            const float score = support + color * 0.46F;
            if (score > best.score) {
                best = Sample{x, y, projection, normal, support, color, score};
            }
        }

        if (best.score > -std::numeric_limits<float>::max() * 0.5F) {
            samples.push_back(best);
            scoreSum += best.score;
            scoreSquaredSum += static_cast<double>(best.score) * best.score;
        }
    }

    if (samples.size() < 24) {
        return refinement;
    }

    const float mean = static_cast<float>(scoreSum / static_cast<double>(samples.size()));
    const float variance = static_cast<float>(
        std::max(0.0, scoreSquaredSum / static_cast<double>(samples.size()) - static_cast<double>(mean) * mean)
    );
    const float threshold = std::clamp(mean + std::sqrt(std::max(0.0F, variance)) * 0.70F, 0.0032F, 0.030F);

    std::vector<DottedTrailPeak> peaks;
    std::size_t supportedSamples = 0;
    std::size_t continuousSamples = 0;
    std::vector<Sample> run;
    const auto flushRun = [&]() {
        if (run.empty()) {
            return;
        }
        supportedSamples += run.size();
        if (run.size() > 8) {
            continuousSamples += run.size();
            run.clear();
            return;
        }

        float weightSum = 0.0F;
        DottedTrailPeak peak;
        for (const auto& sample : run) {
            const float weight = std::max(0.0005F, sample.score - threshold * 0.45F + sample.color * 0.12F);
            peak.x += sample.x * weight;
            peak.y += sample.y * weight;
            peak.projection += sample.projection * weight;
            peak.normal += sample.normal * weight;
            peak.score += sample.support * weight;
            peak.color += sample.color * weight;
            weightSum += weight;
        }
        peak.x /= std::max(0.0005F, weightSum);
        peak.y /= std::max(0.0005F, weightSum);
        peak.projection /= std::max(0.0005F, weightSum);
        peak.normal /= std::max(0.0005F, weightSum);
        peak.score /= std::max(0.0005F, weightSum);
        peak.color /= std::max(0.0005F, weightSum);
        peaks.push_back(peak);
        run.clear();
    };

    for (const auto& sample : samples) {
        const bool supported = sample.score > threshold &&
                               (sample.support > threshold * 0.35F || sample.color > 0.0028F);
        if (supported) {
            run.push_back(sample);
        } else {
            flushRun();
        }
    }
    flushRun();

    if (peaks.size() < 5) {
        return refinement;
    }

    std::sort(peaks.begin(), peaks.end(), [](const DottedTrailPeak& left, const DottedTrailPeak& right) {
        return left.projection < right.projection;
    });

    float gapSum = 0.0F;
    float gapSquaredSum = 0.0F;
    float largestGap = 0.0F;
    for (std::size_t index = 1; index < peaks.size(); ++index) {
        const float gap = peaks[index].projection - peaks[index - 1].projection;
        gapSum += gap;
        gapSquaredSum += gap * gap;
        largestGap = std::max(largestGap, gap);
    }
    const float gapCount = static_cast<float>(peaks.size() - 1);
    const float averageGap = gapSum / std::max(1.0F, gapCount);
    const float gapVariance = std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
    const float gapRegularity =
        1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
    const float rawSpan = peaks.back().projection - peaks.front().projection;
    const float coverage = static_cast<float>(supportedSamples) / static_cast<float>(samples.size());
    const float continuousCoverage = static_cast<float>(continuousSamples) / static_cast<float>(samples.size());
    if (rawSpan < 240.0F || averageGap < 10.0F || averageGap > 230.0F ||
        largestGap > std::max(230.0F, rawSpan * 0.36F) || coverage > 0.52F || continuousCoverage > 0.24F) {
        return refinement;
    }

    double weightSum = 0.0;
    double meanX = 0.0;
    double meanY = 0.0;
    double supportSum = 0.0;
    double colorSum = 0.0;
    for (const auto& peak : peaks) {
        const double weight = std::max(0.0005F, peak.score + peak.color * 0.45F);
        meanX += static_cast<double>(peak.x) * weight;
        meanY += static_cast<double>(peak.y) * weight;
        supportSum += peak.score;
        colorSum += peak.color;
        weightSum += weight;
    }
    meanX /= std::max(0.0005, weightSum);
    meanY /= std::max(0.0005, weightSum);

    double xx = 0.0;
    double yy = 0.0;
    double xy = 0.0;
    for (const auto& peak : peaks) {
        const double weight = std::max(0.0005F, peak.score + peak.color * 0.45F);
        const double dx = static_cast<double>(peak.x) - meanX;
        const double dy = static_cast<double>(peak.y) - meanY;
        xx += dx * dx * weight;
        yy += dy * dy * weight;
        xy += dx * dy * weight;
    }

    float fittedAngle = static_cast<float>(0.5 * std::atan2(2.0 * xy, xx - yy));
    float fittedUnitX = std::cos(fittedAngle);
    float fittedUnitY = std::sin(fittedAngle);
    if (fittedUnitX * unitX + fittedUnitY * unitY < 0.0F) {
        fittedUnitX = -fittedUnitX;
        fittedUnitY = -fittedUnitY;
        fittedAngle = std::atan2(fittedUnitY, fittedUnitX);
    }
    if (std::fabs(std::sin(fittedAngle)) > 0.14F || angleDelta(fittedAngle, trail.angleRadians) > 0.08F) {
        return refinement;
    }

    const float fittedNormalX = -fittedUnitY;
    const float fittedNormalY = fittedUnitX;
    float minProjection = std::numeric_limits<float>::max();
    float maxProjection = std::numeric_limits<float>::lowest();
    float weightedNormal = 0.0F;
    float normalWeight = 0.0F;
    for (const auto& peak : peaks) {
        const float projection = peak.x * fittedUnitX + peak.y * fittedUnitY;
        const float normal = peak.x * fittedNormalX + peak.y * fittedNormalY;
        const float weight = std::max(0.0005F, peak.score + peak.color * 0.45F);
        minProjection = std::min(minProjection, projection);
        maxProjection = std::max(maxProjection, projection);
        weightedNormal += normal * weight;
        normalWeight += weight;
    }

    const float span = maxProjection - minProjection;
    if (span < 240.0F || span > static_cast<float>(image.width) * 0.45F) {
        return refinement;
    }

    const float normal = weightedNormal / std::max(0.0005F, normalWeight);
    ArtifactTrail snapped = trail;
    snapped.x1 = fittedUnitX * minProjection + fittedNormalX * normal;
    snapped.y1 = fittedUnitY * minProjection + fittedNormalY * normal;
    snapped.x2 = fittedUnitX * maxProjection + fittedNormalX * normal;
    snapped.y2 = fittedUnitY * maxProjection + fittedNormalY * normal;
    snapped.length = span;
    snapped.width = std::clamp(trail.width, 1.8F, 3.8F);
    snapped.angleRadians = fittedAngle;
    snapped.colorVariance =
        std::max(trail.colorVariance, static_cast<float>(supportSum / static_cast<double>(peaks.size())));
    snapped.warmEvidence = std::max(trail.warmEvidence, static_cast<float>(colorSum / static_cast<double>(peaks.size())));
    auto centerlinePath =
        buildDottedCenterlinePathFromPeaks(peaks, fittedUnitX, fittedUnitY, minProjection, maxProjection, normal, 18);
    if (centerlinePath.size() >= 2) {
        applyCenterlinePath(snapped, std::move(centerlinePath));
    } else if (trail.path.size() >= 2) {
        snapped.path = trail.path;
    }
    const float peakScore = std::clamp(static_cast<float>(peaks.size()) / 16.0F, 0.0F, 1.0F);
    const float supportScore = std::clamp(snapped.colorVariance * 35.0F + snapped.warmEvidence * 48.0F, 0.0F, 1.0F);
    refinement.quality = std::clamp(peakScore * 0.28F + gapRegularity * 0.26F + supportScore * 0.24F +
                                        std::clamp(span / 1200.0F, 0.0F, 1.0F) * 0.14F +
                                        std::clamp((0.50F - coverage) / 0.30F, 0.0F, 1.0F) * 0.08F,
                                    0.0F,
                                    1.0F);
    snapped.confidence = std::clamp(std::max(snapped.confidence, 0.56F + refinement.quality * 0.34F), 0.0F, 0.96F);
    refinement.ok = refinement.quality >= 0.34F;
    refinement.trail = snapped;
    refinement.averageSupport = snapped.colorVariance;
    refinement.averageColor = snapped.warmEvidence;
    return refinement;
}

void refineLowSkyDottedTrailGeometries(std::vector<ArtifactTrail>& trails,
                                       const std::vector<float>& luminance,
                                       const ImageBuffer& image) {
    for (auto& trail : trails) {
        if (trail.path.size() >= 12) {
            continue;
        }
        auto refinement = refineDottedTrailGeometry(trail, luminance, image);
        if (!refinement.ok) {
            continue;
        }
        const float previousConfidence = trail.confidence;
        trail = refinement.trail;
        trail.confidence = std::max(trail.confidence, previousConfidence);
        trail.weight = std::max(trail.weight, 0.72F + refinement.quality * 0.18F);
    }
}

float pathLength(const std::vector<ArtifactTrailPathPoint>& path) {
    if (path.size() < 2) {
        return 0.0F;
    }

    float length = 0.0F;
    for (std::size_t index = 1; index < path.size(); ++index) {
        length += std::hypot(path[index].x - path[index - 1].x, path[index].y - path[index - 1].y);
    }
    return length;
}

std::vector<ArtifactTrailPathPoint> buildDottedCenterlinePath(const std::vector<ArtifactPoint>& points,
                                                              const std::vector<std::size_t>& orderedIndices,
                                                              float unitX,
                                                              float unitY,
                                                              float minProjection,
                                                              float maxProjection,
                                                              float fallbackNormal,
                                                              int maxSteps) {
    std::vector<ArtifactTrailPathPoint> path;
    if (orderedIndices.size() < 4 || maxProjection <= minProjection) {
        return path;
    }

    const float normalX = -unitY;
    const float normalY = unitX;
    const float span = maxProjection - minProjection;
    const int steps = std::clamp(static_cast<int>(std::round(span / 112.0F)), 4, maxSteps);
    const float binSpacing = span / static_cast<float>(steps);
    const float searchWindow = std::clamp(binSpacing * 0.82F, 38.0F, 96.0F);
    std::vector<float> normals(static_cast<std::size_t>(steps + 1), fallbackNormal);
    std::vector<std::uint8_t> supported(static_cast<std::size_t>(steps + 1), 0);

    for (int step = 0; step <= steps; ++step) {
        const float targetProjection = minProjection + span * static_cast<float>(step) / static_cast<float>(steps);
        float weightedNormal = 0.0F;
        float weightSum = 0.0F;
        float nearestNormal = fallbackNormal;
        float nearestDistance = std::numeric_limits<float>::max();
        for (const auto pointIndex : orderedIndices) {
            const auto& point = points[pointIndex];
            const float projection = point.x * unitX + point.y * unitY;
            const float projectionDistance = std::fabs(projection - targetProjection);
            const float pointNormal = point.x * normalX + point.y * normalY;
            if (projectionDistance < nearestDistance) {
                nearestDistance = projectionDistance;
                nearestNormal = pointNormal;
            }
            if (projectionDistance > searchWindow) {
                continue;
            }
            const float colorExcess = std::max(0.0F, point.colorScore - point.signal);
            const float distanceWeight = 1.0F - std::clamp(projectionDistance / searchWindow, 0.0F, 1.0F) * 0.70F;
            const float weight =
                std::max(0.001F, point.signal + point.colorScore * 1.6F + colorExcess * 3.0F) * distanceWeight;
            weightedNormal += pointNormal * weight;
            weightSum += weight;
        }

        if (weightSum > 0.0F) {
            normals[static_cast<std::size_t>(step)] = weightedNormal / weightSum;
            supported[static_cast<std::size_t>(step)] = 1;
        } else if (nearestDistance <= searchWindow * 1.85F) {
            normals[static_cast<std::size_t>(step)] = nearestNormal;
        }
    }

    for (int step = 1; step < steps; ++step) {
        if (!supported[static_cast<std::size_t>(step - 1)] || !supported[static_cast<std::size_t>(step + 1)]) {
            continue;
        }
        normals[static_cast<std::size_t>(step)] =
            normals[static_cast<std::size_t>(step)] * 0.60F +
            (normals[static_cast<std::size_t>(step - 1)] + normals[static_cast<std::size_t>(step + 1)]) * 0.20F;
    }

    path.reserve(static_cast<std::size_t>(steps + 1));
    for (int step = 0; step <= steps; ++step) {
        const float projection = minProjection + span * static_cast<float>(step) / static_cast<float>(steps);
        const float normal = normals[static_cast<std::size_t>(step)];
        path.push_back({unitX * projection + normalX * normal, unitY * projection + normalY * normal});
    }
    return path;
}

std::vector<ArtifactTrailPathPoint> buildDottedCenterlinePathFromPeaks(const std::vector<DottedTrailPeak>& peaks,
                                                                       float unitX,
                                                                       float unitY,
                                                                       float minProjection,
                                                                       float maxProjection,
                                                                       float fallbackNormal,
                                                                       int maxSteps) {
    std::vector<ArtifactTrailPathPoint> path;
    if (peaks.size() < 4 || maxProjection <= minProjection) {
        return path;
    }

    const float normalX = -unitY;
    const float normalY = unitX;
    const float span = maxProjection - minProjection;
    const int steps = std::clamp(static_cast<int>(std::round(span / 112.0F)), 4, maxSteps);
    const float binSpacing = span / static_cast<float>(steps);
    const float searchWindow = std::clamp(binSpacing * 0.88F, 44.0F, 112.0F);
    std::vector<float> normals(static_cast<std::size_t>(steps + 1), fallbackNormal);
    std::vector<std::uint8_t> supported(static_cast<std::size_t>(steps + 1), 0);

    for (int step = 0; step <= steps; ++step) {
        const float targetProjection = minProjection + span * static_cast<float>(step) / static_cast<float>(steps);
        float weightedNormal = 0.0F;
        float weightSum = 0.0F;
        float nearestNormal = fallbackNormal;
        float nearestDistance = std::numeric_limits<float>::max();

        for (const auto& peak : peaks) {
            const float projection = peak.x * unitX + peak.y * unitY;
            const float projectionDistance = std::fabs(projection - targetProjection);
            const float pointNormal = peak.x * normalX + peak.y * normalY;
            if (projectionDistance < nearestDistance) {
                nearestDistance = projectionDistance;
                nearestNormal = pointNormal;
            }
            if (projectionDistance > searchWindow) {
                continue;
            }

            const float distanceWeight = 1.0F - std::clamp(projectionDistance / searchWindow, 0.0F, 1.0F) * 0.68F;
            const float weight = std::max(0.001F, peak.score + peak.color * 0.72F) * distanceWeight;
            weightedNormal += pointNormal * weight;
            weightSum += weight;
        }

        if (weightSum > 0.0F) {
            normals[static_cast<std::size_t>(step)] = weightedNormal / weightSum;
            supported[static_cast<std::size_t>(step)] = 1;
        } else if (nearestDistance <= searchWindow * 1.70F) {
            normals[static_cast<std::size_t>(step)] = nearestNormal;
        }
    }

    for (int step = 1; step < steps; ++step) {
        if (!supported[static_cast<std::size_t>(step - 1)] || !supported[static_cast<std::size_t>(step + 1)]) {
            continue;
        }
        normals[static_cast<std::size_t>(step)] =
            normals[static_cast<std::size_t>(step)] * 0.56F +
            (normals[static_cast<std::size_t>(step - 1)] + normals[static_cast<std::size_t>(step + 1)]) * 0.22F;
    }

    path.reserve(static_cast<std::size_t>(steps + 1));
    for (int step = 0; step <= steps; ++step) {
        const float projection = minProjection + span * static_cast<float>(step) / static_cast<float>(steps);
        const float normal = normals[static_cast<std::size_t>(step)];
        path.push_back({unitX * projection + normalX * normal, unitY * projection + normalY * normal});
    }
    return path;
}

void applyCenterlinePath(ArtifactTrail& trail, std::vector<ArtifactTrailPathPoint> path) {
    if (path.size() < 2) {
        return;
    }

    trail.path = std::move(path);
    trail.x1 = trail.path.front().x;
    trail.y1 = trail.path.front().y;
    trail.x2 = trail.path.back().x;
    trail.y2 = trail.path.back().y;
    trail.length = std::max(trail.length, pathLength(trail.path));
    trail.angleRadians = std::atan2(trail.y2 - trail.y1, trail.x2 - trail.x1);
}

void snapDottedDronePathToLocalEvidence(ArtifactTrail& trail, const std::vector<float>& luminance,
                                        const ImageBuffer& image) {
    if (trail.kind != ArtifactTrailKind::Drone || trail.path.size() < 2 || luminance.size() != image.pixelCount()) {
        return;
    }
    const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
    if (centerRatio < 0.760F || centerRatio > 0.982F || std::fabs(std::sin(trail.angleRadians)) > 0.145F) {
        return;
    }

    struct OffsetSample {
        float along = 0.0F;
        float offset = 0.0F;
        float weight = 0.0F;
        float color = 0.0F;
    };
    struct ProfileStation {
        float along = 0.0F;
        float median = 0.0F;
        std::vector<float> scores;
        std::vector<float> supports;
        std::vector<float> colors;
    };
    std::vector<float> cumulative(trail.path.size(), 0.0F);
    for (std::size_t index = 1; index < trail.path.size(); ++index) {
        cumulative[index] = cumulative[index - 1] + std::hypot(trail.path[index].x - trail.path[index - 1].x,
                                                               trail.path[index].y - trail.path[index - 1].y);
    }
    const float totalLength = cumulative.back();
    if (totalLength < 240.0F) {
        return;
    }

    const bool wideHorizonSearch =
        centerRatio >= 0.930F && trail.length >= 680.0F && std::fabs(std::sin(trail.angleRadians)) <= 0.040F;
    const float maxOffset = wideHorizonSearch ? std::clamp(trail.width * 2.0F + 66.0F, 66.0F, 74.0F)
                                              : std::clamp(trail.width * 3.8F + 8.5F, 10.0F, 20.0F);
    float coherentStartFraction = 0.0F;
    float coherentEndFraction = 1.0F;
    bool truncateToCoherentInterval = false;
    bool useCoherentCurveModel = false;
    float coherentStartOffset = 0.0F;
    float coherentEndOffset = 0.0F;
    float coherentBend = 0.0F;
    std::vector<OffsetSample> samples;
    samples.reserve(static_cast<std::size_t>(totalLength / 8.0F) + 1);
    std::vector<ProfileStation> profileStations;
    profileStations.reserve(static_cast<std::size_t>(totalLength / 8.0F) + trail.path.size());
    for (std::size_t index = 1; index < trail.path.size(); ++index) {
        const auto& start = trail.path[index - 1];
        const auto& end = trail.path[index];
        const float vx = end.x - start.x;
        const float vy = end.y - start.y;
        const float segmentLength = std::hypot(vx, vy);
        if (segmentLength < 1.0F) {
            continue;
        }
        const float unitX = vx / segmentLength;
        const float unitY = vy / segmentLength;
        const float normalX = -unitY;
        const float normalY = unitX;
        const float alongStep = wideHorizonSearch ? 4.0F : 8.0F;
        const int steps = std::max(1, static_cast<int>(std::round(segmentLength / alongStep)));
        for (int step = 0; step <= steps; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const float baseX = start.x + vx * t;
            const float baseY = start.y + vy * t;
            float bestOffset = 0.0F;
            float bestScore = -std::numeric_limits<float>::max();
            float bestColor = 0.0F;
            float bestSupport = 0.0F;
            std::vector<float> profileScores;
            std::vector<float> profileSupports;
            std::vector<float> profileColors;
            profileScores.reserve(static_cast<std::size_t>(maxOffset * 2.0F) + 2);
            profileSupports.reserve(static_cast<std::size_t>(maxOffset * 2.0F) + 2);
            profileColors.reserve(static_cast<std::size_t>(maxOffset * 2.0F) + 2);
            for (float offset = -maxOffset; offset <= maxOffset; offset += 1.0F) {
                const float x = baseX + normalX * offset;
                const float y = baseY + normalY * offset;
                if (x < 2.0F || y < 2.0F || x >= static_cast<float>(image.width - 2) ||
                    y >= static_cast<float>(image.height - 2)) {
                    continue;
                }
                float pointSupport = 0.0F;
                float color = 0.0F;
                for (const float alongOffset : {-4.0F, 0.0F, 4.0F}) {
                    if (wideHorizonSearch && std::fabs(alongOffset) > 0.1F) {
                        continue;
                    }
                    const float sampleX = x + unitX * alongOffset;
                    const float sampleY = y + unitY * alongOffset;
                    if (sampleX < 2.0F || sampleY < 2.0F || sampleX >= static_cast<float>(image.width - 2) ||
                        sampleY >= static_cast<float>(image.height - 2)) {
                        continue;
                    }
                    const auto pixel = nearestPixelIndex(image, sampleX, sampleY);
                    const float sampleColor = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 2.3F +
                                              std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.12F;
                    float normalBackground = 0.0F;
                    for (const float distance : {6.0F, 10.0F, 15.0F}) {
                        normalBackground +=
                            luminanceNearest(luminance, image.width, image.height, sampleX + normalX * distance,
                                             sampleY + normalY * distance);
                        normalBackground +=
                            luminanceNearest(luminance, image.width, image.height, sampleX - normalX * distance,
                                             sampleY - normalY * distance);
                    }
                    normalBackground /= 6.0F;
                    const float ridgePointSupport =
                        std::max(0.0F, luminance[pixel] - normalBackground) + sampleColor * 0.34F;
                    pointSupport = std::max(
                        pointSupport,
                        std::max(localDottedPointScore(trail, luminance, image, sampleX, sampleY), ridgePointSupport));
                    color = std::max(color, sampleColor);
                }
                const float lineSupport = artifactLineSupportScore(luminance, image, x, y, normalX, normalY);
                const float support = std::max(pointSupport, lineSupport * 0.78F);
                const float centerPenalty = std::fabs(offset) * (wideHorizonSearch ? 0.00003F : 0.00008F);
                const float score = support + color * 0.58F - centerPenalty;
                profileScores.push_back(score);
                profileSupports.push_back(support);
                profileColors.push_back(color);
                if (score > bestScore) {
                    bestScore = score;
                    bestOffset = offset;
                    bestColor = color;
                    bestSupport = support;
                }
            }
            if (profileScores.empty()) {
                continue;
            }
            auto profileScoreDistribution = profileScores;
            const auto middle =
                profileScoreDistribution.begin() + static_cast<std::ptrdiff_t>(profileScoreDistribution.size() / 2);
            std::nth_element(profileScoreDistribution.begin(), middle, profileScoreDistribution.end());
            const float profileMedian = *middle;
            if (wideHorizonSearch) {
                profileStations.push_back({cumulative[index - 1] + segmentLength * t, profileMedian,
                                           std::move(profileScores), std::move(profileSupports),
                                           std::move(profileColors)});
            }
            const float prominence = bestScore - profileMedian;
            const bool supported = bestScore > 0.0035F &&
                                   prominence > std::max(0.00075F, std::fabs(profileMedian) * 0.20F) &&
                                   (bestSupport > 0.0032F || bestColor > 0.0018F) && std::fabs(bestOffset) <= maxOffset;
            if (supported) {
                samples.push_back({cumulative[index - 1] + segmentLength * t, bestOffset,
                                   std::max(0.001F, prominence + bestColor * 0.24F), bestColor});
            }
        }
    }

    if (wideHorizonSearch && profileStations.size() >= 8) {
        const std::size_t stateCount = profileStations.front().scores.size();
        const bool consistentProfiles =
            stateCount >= 8 && std::all_of(profileStations.begin(), profileStations.end(), [&](const auto& station) {
                return station.scores.size() == stateCount && station.supports.size() == stateCount &&
                       station.colors.size() == stateCount;
            });
        if (consistentProfiles) {
            const int offsetRadius = static_cast<int>(std::floor(maxOffset));
            const auto scoreCurve = [&](float startOffset, float endOffset, float bend, float startFraction,
                                        float endFraction) {
                if (std::fabs(endOffset - startOffset) > 96.0F) {
                    return std::pair<float, int>{-std::numeric_limits<float>::max(), 0};
                }
                float score = 0.0F;
                int hits = 0;
                int stationCount = 0;
                std::vector<float> peakPositions;
                float runAlongSum = 0.0F;
                int runLength = 0;
                int continuousRuns = 0;
                const auto flushRun = [&]() {
                    if (runLength > 0 && runLength <= 5) {
                        peakPositions.push_back(runAlongSum / static_cast<float>(runLength));
                    } else if (runLength > 5) {
                        continuousRuns += 1;
                    }
                    runAlongSum = 0.0F;
                    runLength = 0;
                };
                for (const auto& station : profileStations) {
                    const float globalT = std::clamp(station.along / std::max(1.0F, totalLength), 0.0F, 1.0F);
                    if (globalT < startFraction || globalT > endFraction) {
                        continue;
                    }
                    stationCount += 1;
                    const float t = std::clamp((globalT - startFraction) / std::max(0.01F, endFraction - startFraction),
                                               0.0F, 1.0F);
                    const float predictedOffset =
                        startOffset + (endOffset - startOffset) * t + bend * 4.0F * t * (1.0F - t);
                    const int predictedState = static_cast<int>(std::round(predictedOffset + maxOffset));
                    if (predictedState < 0 || predictedState >= static_cast<int>(stateCount)) {
                        return std::pair<float, int>{-std::numeric_limits<float>::max(), 0};
                    }
                    float bestEvidence = 0.0F;
                    float bestColor = 0.0F;
                    float bestSupport = 0.0F;
                    const int begin = std::max(0, predictedState - 3);
                    const int end = std::min(static_cast<int>(stateCount) - 1, predictedState + 3);
                    for (int state = begin; state <= end; ++state) {
                        const float evidence = station.scores[static_cast<std::size_t>(state)] - station.median;
                        if (evidence > bestEvidence) {
                            bestEvidence = evidence;
                            bestColor = station.colors[static_cast<std::size_t>(state)];
                            bestSupport = station.supports[static_cast<std::size_t>(state)];
                        }
                    }
                    float contribution = std::min(0.0045F, std::max(0.0F, bestEvidence));
                    contribution += std::min(0.0008F, bestColor * 0.018F);
                    if (bestSupport < 0.0015F && bestColor < 0.0010F) {
                        contribution *= 0.20F;
                    }
                    if (bestEvidence > 0.0080F || (bestEvidence > 0.0040F && bestColor > 0.010F)) {
                        hits += 1;
                        contribution += 0.00050F;
                    }
                    if (bestEvidence > 0.018F) {
                        runAlongSum += station.along;
                        runLength += 1;
                    } else {
                        flushRun();
                    }
                    score += contribution;
                }
                flushRun();
                if (stationCount < 6) {
                    return std::pair<float, int>{-std::numeric_limits<float>::max(), 0};
                }
                const float hitRatio = static_cast<float>(hits) / static_cast<float>(stationCount);
                float gapRegularity = 0.0F;
                if (peakPositions.size() >= 5) {
                    float gapSum = 0.0F;
                    float gapSquaredSum = 0.0F;
                    for (std::size_t index = 1; index < peakPositions.size(); ++index) {
                        const float gap = peakPositions[index] - peakPositions[index - 1];
                        gapSum += gap;
                        gapSquaredSum += gap * gap;
                    }
                    const float gapCount = static_cast<float>(peakPositions.size() - 1);
                    const float averageGap = gapSum / gapCount;
                    const float gapVariance = std::max(0.0F, gapSquaredSum / gapCount - averageGap * averageGap);
                    gapRegularity = 1.0F - std::clamp(std::sqrt(gapVariance) / std::max(4.0F, averageGap), 0.0F, 1.0F);
                }
                const float peakCountScore = std::clamp(static_cast<float>(peakPositions.size()) / 18.0F, 0.0F, 1.0F);
                score = score / static_cast<float>(stationCount) + hitRatio * 0.006F +
                        (endFraction - startFraction) * 0.00045F - startFraction * 0.012F -
                        std::max(0.0F, hitRatio - 0.68F) * 0.040F + gapRegularity * 0.0120F + peakCountScore * 0.0020F -
                        static_cast<float>(continuousRuns) * 0.0008F - std::fabs(bend) * 0.00017F;
                return std::pair<float, int>{score, hits};
            };

            float bestStart = 0.0F;
            float bestEnd = 0.0F;
            float bestBend = 0.0F;
            float bestStartFraction = 0.0F;
            float bestEndFraction = 1.0F;
            float bestScore = -std::numeric_limits<float>::max();
            int bestHits = 0;
            constexpr int coarseStep = 6;
            constexpr std::array<std::pair<float, float>, 9> intervals{{
                {0.0F, 0.55F},
                {0.0F, 0.65F},
                {0.0F, 0.75F},
                {0.0F, 0.85F},
                {0.0F, 1.0F},
                {0.10F, 0.70F},
                {0.15F, 0.80F},
                {0.25F, 0.90F},
                {0.35F, 1.0F},
            }};
            for (const auto& [startFraction, endFraction] : intervals) {
                for (int start = -offsetRadius; start <= offsetRadius; start += coarseStep) {
                    for (int end = -offsetRadius; end <= offsetRadius; end += coarseStep) {
                        for (int bend = -18; bend <= 18; bend += 6) {
                            const auto [score, hits] = scoreCurve(static_cast<float>(start), static_cast<float>(end),
                                                                  static_cast<float>(bend), startFraction, endFraction);
                            if (score > bestScore) {
                                bestScore = score;
                                bestHits = hits;
                                bestStart = static_cast<float>(start);
                                bestEnd = static_cast<float>(end);
                                bestBend = static_cast<float>(bend);
                                bestStartFraction = startFraction;
                                bestEndFraction = endFraction;
                            }
                        }
                    }
                }
            }
            const float coarseStart = bestStart;
            const float coarseEnd = bestEnd;
            const float coarseBend = bestBend;
            for (int startDelta = -6; startDelta <= 6; ++startDelta) {
                for (int endDelta = -6; endDelta <= 6; ++endDelta) {
                    for (int bendDelta = -6; bendDelta <= 6; bendDelta += 2) {
                        const float start = coarseStart + static_cast<float>(startDelta);
                        const float end = coarseEnd + static_cast<float>(endDelta);
                        const float bend = coarseBend + static_cast<float>(bendDelta);
                        const auto [score, hits] = scoreCurve(start, end, bend, bestStartFraction, bestEndFraction);
                        if (score > bestScore) {
                            bestScore = score;
                            bestHits = hits;
                            bestStart = start;
                            bestEnd = end;
                            bestBend = bend;
                        }
                    }
                }
            }

            int bestPhaseShift = 0;
            float bestPhaseScore = -std::numeric_limits<float>::max();
            for (int phaseShift = -18; phaseShift <= 18; ++phaseShift) {
                float phaseScore = 0.0F;
                int phaseHits = 0;
                int phaseStations = 0;
                for (const auto& station : profileStations) {
                    const float globalT = station.along / std::max(1.0F, totalLength);
                    if (globalT < bestStartFraction || globalT > bestEndFraction) {
                        continue;
                    }
                    const float t =
                        std::clamp((globalT - bestStartFraction) / std::max(0.01F, bestEndFraction - bestStartFraction),
                                   0.0F, 1.0F);
                    const float predictedOffset = bestStart + (bestEnd - bestStart) * t +
                                                  bestBend * 4.0F * t * (1.0F - t) + static_cast<float>(phaseShift);
                    const int predictedState = static_cast<int>(std::round(predictedOffset + maxOffset));
                    if (predictedState < 0 || predictedState >= static_cast<int>(stateCount)) {
                        continue;
                    }
                    float evidence = 0.0F;
                    for (int state = std::max(0, predictedState - 1);
                         state <= std::min(static_cast<int>(stateCount) - 1, predictedState + 1); ++state) {
                        evidence = std::max(evidence, station.scores[static_cast<std::size_t>(state)] - station.median);
                    }
                    phaseScore += std::min(0.040F, std::max(0.0F, evidence));
                    if (evidence > 0.010F) {
                        phaseHits += 1;
                    }
                    phaseStations += 1;
                }
                if (phaseStations > 0) {
                    phaseScore = phaseScore / static_cast<float>(phaseStations) +
                                 static_cast<float>(phaseHits) / static_cast<float>(phaseStations) * 0.004F -
                                 std::fabs(static_cast<float>(phaseShift)) * 0.000002F;
                }
                if (phaseScore > bestPhaseScore) {
                    bestPhaseScore = phaseScore;
                    bestPhaseShift = phaseShift;
                }
            }
            bestStart = std::clamp(bestStart + static_cast<float>(bestPhaseShift), -maxOffset, maxOffset);
            bestEnd = std::clamp(bestEnd + static_cast<float>(bestPhaseShift), -maxOffset, maxOffset);

            std::vector<OffsetSample> coherentSamples;
            coherentSamples.reserve(profileStations.size());
            for (std::size_t stationIndex = 0; stationIndex < profileStations.size(); ++stationIndex) {
                const auto& station = profileStations[stationIndex];
                const float globalT = std::clamp(station.along / std::max(1.0F, totalLength), 0.0F, 1.0F);
                if (globalT < bestStartFraction || globalT > bestEndFraction) {
                    continue;
                }
                const float t = std::clamp(
                    (globalT - bestStartFraction) / std::max(0.01F, bestEndFraction - bestStartFraction), 0.0F, 1.0F);
                const float predictedOffset = bestStart + (bestEnd - bestStart) * t + bestBend * 4.0F * t * (1.0F - t);
                const int predictedState = static_cast<int>(std::round(predictedOffset + maxOffset));
                int selectedState = -1;
                float bestEvidence = 0.0F;
                const int begin = std::max(0, predictedState - 4);
                const int end = std::min(static_cast<int>(stateCount) - 1, predictedState + 4);
                for (int state = begin; state <= end; ++state) {
                    const float evidence = station.scores[static_cast<std::size_t>(state)] - station.median;
                    const float centeredPenalty = std::fabs(static_cast<float>(state - predictedState)) * 0.000015F;
                    if (evidence - centeredPenalty > bestEvidence) {
                        bestEvidence = evidence - centeredPenalty;
                        selectedState = state;
                    }
                }
                if (selectedState < 0) {
                    continue;
                }
                const float evidence = station.scores[static_cast<std::size_t>(selectedState)] - station.median;
                const float color = station.colors[static_cast<std::size_t>(selectedState)];
                const float support = station.supports[static_cast<std::size_t>(selectedState)];
                if (evidence < 0.00055F && color < 0.0010F) {
                    continue;
                }
                if (support < 0.0015F && color < 0.0010F) {
                    continue;
                }
                coherentSamples.push_back({station.along, -maxOffset + static_cast<float>(selectedState),
                                           std::max(0.001F, std::min(0.018F, evidence) + color * 0.16F), color});
            }
            const float intervalStationCount = static_cast<float>(
                std::count_if(profileStations.begin(), profileStations.end(), [&](const auto& station) {
                    const float t = station.along / std::max(1.0F, totalLength);
                    return t >= bestStartFraction && t <= bestEndFraction;
                }));
            const float hitRatio = static_cast<float>(bestHits) / std::max(1.0F, intervalStationCount);
            if (coherentSamples.size() >= 6 && hitRatio >= 0.16F) {
                samples = std::move(coherentSamples);
                coherentStartFraction = bestStartFraction;
                coherentEndFraction = bestEndFraction;
                truncateToCoherentInterval = bestEndFraction - bestStartFraction < 0.98F;
                useCoherentCurveModel = true;
                coherentStartOffset = bestStart;
                coherentEndOffset = bestEnd;
                coherentBend = bestBend;
            }
        }
    }
    if (samples.size() < 5) {
        return;
    }

    // Keep one local maximum per blinking light. Dense star fields can produce
    // weak cross-section support at every sample, so grouping only by distance
    // would merge the whole trail into one continuous run.
    std::vector<OffsetSample> peaks;
    std::vector<float> evidence;
    evidence.reserve(samples.size());
    for (const auto& sample : samples) {
        evidence.push_back(sample.weight + sample.color * 0.18F);
    }
    auto thresholdEvidence = evidence;
    const auto thresholdPosition =
        thresholdEvidence.begin() + static_cast<std::ptrdiff_t>(thresholdEvidence.size() * 3 / 5);
    std::nth_element(thresholdEvidence.begin(), thresholdPosition, thresholdEvidence.end());
    const float peakThreshold = *thresholdPosition;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const std::size_t begin = index > 2 ? index - 2 : 0;
        const std::size_t end = std::min(samples.size(), index + 3);
        float localMaximum = 0.0F;
        for (std::size_t neighbor = begin; neighbor < end; ++neighbor) {
            localMaximum = std::max(localMaximum, evidence[neighbor]);
        }
        if (evidence[index] + 1.0e-6F < localMaximum || evidence[index] < peakThreshold) {
            continue;
        }
        if (!peaks.empty() && samples[index].along - peaks.back().along < 12.0F) {
            if (evidence[index] > peaks.back().weight + peaks.back().color * 0.18F) {
                peaks.back() = samples[index];
            }
            continue;
        }
        peaks.push_back(samples[index]);
    }
    if (peaks.size() < 5) {
        return;
    }

    const auto weightedMedian = [](std::vector<OffsetSample> values, float fallback) {
        if (values.empty()) {
            return fallback;
        }
        std::sort(values.begin(), values.end(),
                  [](const OffsetSample& left, const OffsetSample& right) { return left.offset < right.offset; });
        float weightSum = 0.0F;
        for (const auto& value : values) {
            weightSum += std::max(0.0005F, value.weight);
        }
        float accumulated = 0.0F;
        for (const auto& value : values) {
            accumulated += std::max(0.0005F, value.weight);
            if (accumulated >= weightSum * 0.5F) {
                return value.offset;
            }
        }
        return values.back().offset;
    };

    std::vector<OffsetSample> sortedSamples = peaks;
    const float medianOffset = weightedMedian(sortedSamples, 0.0F);
    std::vector<float> pathOffsets(trail.path.size(), medianOffset);
    const float localWindow = wideHorizonSearch ? std::clamp(totalLength / 8.0F, 82.0F, 145.0F)
                                                : std::clamp(totalLength / 5.0F, 110.0F, 240.0F);
    bool fittedCoherentCurve = false;
    if (useCoherentCurveModel) {
        for (std::size_t index = 0; index < trail.path.size(); ++index) {
            const float globalT = cumulative[index] / std::max(1.0F, totalLength);
            const float t = std::clamp((globalT - coherentStartFraction) /
                                           std::max(0.01F, coherentEndFraction - coherentStartFraction),
                                       0.0F, 1.0F);
            pathOffsets[index] = std::clamp(coherentStartOffset + (coherentEndOffset - coherentStartOffset) * t +
                                                coherentBend * 4.0F * t * (1.0F - t),
                                            -maxOffset, maxOffset);
        }
        fittedCoherentCurve = true;
    }
    if (!fittedCoherentCurve && wideHorizonSearch && peaks.size() >= 6) {
        std::vector<OffsetSample> candidates = peaks;
        std::sort(candidates.begin(), candidates.end(), [](const OffsetSample& left, const OffsetSample& right) {
            return left.weight + left.color * 0.30F > right.weight + right.color * 0.30F;
        });
        if (candidates.size() > 72) {
            candidates.resize(72);
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const OffsetSample& left, const OffsetSample& right) { return left.along < right.along; });

        float bestCurveScore = 0.0F;
        float bestA = 0.0F;
        float bestB = 0.0F;
        float bestC = 0.0F;
        std::vector<OffsetSample> bestInliers;
        const auto normalizedAlong = [&](float along) { return along / std::max(1.0F, totalLength) * 2.0F - 1.0F; };
        const auto evaluateCurve = [](float a, float b, float c, float t) { return a + b * t + c * t * t; };

        for (std::size_t first = 0; first + 2 < candidates.size(); ++first) {
            const float t0 = normalizedAlong(candidates[first].along);
            for (std::size_t middleIndex = first + 1; middleIndex + 1 < candidates.size(); ++middleIndex) {
                const float t1 = normalizedAlong(candidates[middleIndex].along);
                if (t1 - t0 < 0.12F) {
                    continue;
                }
                for (std::size_t last = middleIndex + 1; last < candidates.size(); ++last) {
                    const float t2 = normalizedAlong(candidates[last].along);
                    if (t2 - t1 < 0.12F || t2 - t0 < 0.42F) {
                        continue;
                    }
                    const float denominator0 = (t0 - t1) * (t0 - t2);
                    const float denominator1 = (t1 - t0) * (t1 - t2);
                    const float denominator2 = (t2 - t0) * (t2 - t1);
                    if (std::fabs(denominator0 * denominator1 * denominator2) < 1.0e-7F) {
                        continue;
                    }
                    const float y0 = candidates[first].offset;
                    const float y1 = candidates[middleIndex].offset;
                    const float y2 = candidates[last].offset;
                    const float a =
                        y0 * (t1 * t2) / denominator0 + y1 * (t0 * t2) / denominator1 + y2 * (t0 * t1) / denominator2;
                    const float b =
                        -y0 * (t1 + t2) / denominator0 - y1 * (t0 + t2) / denominator1 - y2 * (t0 + t1) / denominator2;
                    const float c = y0 / denominator0 + y1 / denominator1 + y2 / denominator2;
                    if (std::fabs(b) > 80.0F || std::fabs(c) > 65.0F) {
                        continue;
                    }

                    std::vector<OffsetSample> inliers;
                    float score = 0.0F;
                    float minAlong = totalLength;
                    float maxAlong = 0.0F;
                    constexpr float residualLimit = 6.5F;
                    for (const auto& sample : candidates) {
                        const float predicted = evaluateCurve(a, b, c, normalizedAlong(sample.along));
                        const float residual = std::fabs(sample.offset - predicted);
                        if (residual > residualLimit) {
                            continue;
                        }
                        const float robustWeight = 1.0F - residual / residualLimit * 0.55F;
                        score += (sample.weight + sample.color * 0.26F) * robustWeight;
                        minAlong = std::min(minAlong, sample.along);
                        maxAlong = std::max(maxAlong, sample.along);
                        inliers.push_back(sample);
                    }
                    const float coverage = (maxAlong - minAlong) / std::max(1.0F, totalLength);
                    if (inliers.size() < 5 || coverage < 0.46F) {
                        continue;
                    }
                    score *= 0.72F + coverage * 0.28F;
                    score += static_cast<float>(inliers.size()) * 0.00035F;
                    if (score > bestCurveScore) {
                        bestCurveScore = score;
                        bestA = a;
                        bestB = b;
                        bestC = c;
                        bestInliers = std::move(inliers);
                    }
                }
            }
        }

        if (bestInliers.size() >= 5) {
            for (std::size_t index = 0; index < trail.path.size(); ++index) {
                const float baseOffset = evaluateCurve(bestA, bestB, bestC, normalizedAlong(cumulative[index]));
                std::vector<OffsetSample> localResiduals;
                for (const auto& sample : bestInliers) {
                    const float distance = std::fabs(sample.along - cumulative[index]);
                    if (distance > localWindow) {
                        continue;
                    }
                    const float modelOffset = evaluateCurve(bestA, bestB, bestC, normalizedAlong(sample.along));
                    const float weight = sample.weight * (1.0F - distance / localWindow * 0.72F);
                    localResiduals.push_back({sample.along, sample.offset - modelOffset, weight, sample.color});
                }
                const float residual = std::clamp(weightedMedian(std::move(localResiduals), 0.0F), -3.5F, 3.5F);
                pathOffsets[index] = std::clamp(baseOffset + residual, -maxOffset, maxOffset);
            }
            peaks = std::move(bestInliers);
            fittedCoherentCurve = true;
        }
    }

    if (!fittedCoherentCurve) {
        std::vector<OffsetSample> deviations = peaks;
        for (auto& sample : deviations) {
            sample.offset = std::fabs(sample.offset - medianOffset);
        }
        const float medianDeviation = weightedMedian(deviations, 0.0F);
        const float inlierRadius = std::clamp(medianDeviation * 2.8F + 2.0F, 3.5F, wideHorizonSearch ? 14.0F : 8.0F);
        peaks.erase(std::remove_if(peaks.begin(), peaks.end(),
                                   [&](const OffsetSample& sample) {
                                       return std::fabs(sample.offset - medianOffset) > inlierRadius;
                                   }),
                    peaks.end());
        if (peaks.size() < 4) {
            return;
        }
        for (std::size_t index = 0; index < trail.path.size(); ++index) {
            std::vector<OffsetSample> localPeaks;
            for (const auto& sample : peaks) {
                const float distance = std::fabs(sample.along - cumulative[index]);
                if (distance > localWindow) {
                    continue;
                }
                const float weight = sample.weight * (1.0F - distance / localWindow * 0.72F);
                localPeaks.push_back({sample.along, sample.offset, weight, sample.color});
            }
            const float offset = weightedMedian(std::move(localPeaks), medianOffset);
            const float localOffsetRange = wideHorizonSearch ? 10.0F : 5.5F;
            pathOffsets[index] = std::clamp(offset, medianOffset - localOffsetRange, medianOffset + localOffsetRange);
        }
    }

    // Median smoothing removes isolated star pulls while retaining a gradual
    // curved flight path. Limit the second difference to avoid visible kinks.
    if (pathOffsets.size() >= 3) {
        auto smoothed = pathOffsets;
        for (std::size_t index = 1; index + 1 < pathOffsets.size(); ++index) {
            std::array<float, 3> neighborhood{pathOffsets[index - 1], pathOffsets[index], pathOffsets[index + 1]};
            std::sort(neighborhood.begin(), neighborhood.end());
            smoothed[index] = neighborhood[1];
        }
        for (std::size_t index = 1; index + 1 < smoothed.size(); ++index) {
            const float linear = (smoothed[index - 1] + smoothed[index + 1]) * 0.5F;
            smoothed[index] = std::clamp(smoothed[index], linear - 3.5F, linear + 3.5F);
        }
        pathOffsets = std::move(smoothed);
    }

    float maximumCorrection = 0.0F;
    for (const float offset : pathOffsets) {
        maximumCorrection = std::max(maximumCorrection, std::fabs(offset));
    }
    if (maximumCorrection < 0.55F) {
        return;
    }

    std::vector<ArtifactTrailPathPoint> snappedPath;
    snappedPath.reserve(trail.path.size() + 2);
    const auto appendBoundaryPoint = [&](float fraction) {
        const float targetDistance = std::clamp(fraction, 0.0F, 1.0F) * totalLength;
        const auto upper = std::lower_bound(cumulative.begin(), cumulative.end(), targetDistance);
        std::size_t endIndex = static_cast<std::size_t>(std::distance(cumulative.begin(), upper));
        endIndex = std::clamp<std::size_t>(endIndex, 1, trail.path.size() - 1);
        const std::size_t startIndex = endIndex - 1;
        const float segmentDistance = cumulative[endIndex] - cumulative[startIndex];
        const float segmentT =
            segmentDistance <= 1.0e-5F ? 0.0F : (targetDistance - cumulative[startIndex]) / segmentDistance;
        const float baseX = trail.path[startIndex].x + (trail.path[endIndex].x - trail.path[startIndex].x) * segmentT;
        const float baseY = trail.path[startIndex].y + (trail.path[endIndex].y - trail.path[startIndex].y) * segmentT;
        float unitX = trail.path[endIndex].x - trail.path[startIndex].x;
        float unitY = trail.path[endIndex].y - trail.path[startIndex].y;
        const float tangentLength = std::sqrt(std::max(1.0F, unitX * unitX + unitY * unitY));
        unitX /= tangentLength;
        unitY /= tangentLength;
        const float modelT = std::clamp((fraction - coherentStartFraction) /
                                            std::max(0.01F, coherentEndFraction - coherentStartFraction),
                                        0.0F, 1.0F);
        const float offset =
            useCoherentCurveModel
                ? std::clamp(coherentStartOffset + (coherentEndOffset - coherentStartOffset) * modelT +
                                 coherentBend * 4.0F * modelT * (1.0F - modelT),
                             -maxOffset, maxOffset)
                : pathOffsets[startIndex] + (pathOffsets[endIndex] - pathOffsets[startIndex]) * segmentT;
        snappedPath.push_back({baseX - unitY * offset, baseY + unitX * offset});
    };
    if (truncateToCoherentInterval && coherentStartFraction > 1.0e-4F) {
        appendBoundaryPoint(coherentStartFraction);
    }
    for (std::size_t index = 0; index < trail.path.size(); ++index) {
        const float pathFraction = cumulative[index] / std::max(1.0F, totalLength);
        if (truncateToCoherentInterval &&
            (pathFraction + 1.0e-5F < coherentStartFraction || pathFraction - 1.0e-5F > coherentEndFraction)) {
            continue;
        }
        const float offset = pathOffsets[index];

        float unitX = 1.0F;
        float unitY = 0.0F;
        if (index == 0) {
            unitX = trail.path[1].x - trail.path[0].x;
            unitY = trail.path[1].y - trail.path[0].y;
        } else if (index + 1 == trail.path.size()) {
            unitX = trail.path[index].x - trail.path[index - 1].x;
            unitY = trail.path[index].y - trail.path[index - 1].y;
        } else {
            unitX = trail.path[index + 1].x - trail.path[index - 1].x;
            unitY = trail.path[index + 1].y - trail.path[index - 1].y;
        }
        const float tangentLength = std::sqrt(std::max(1.0F, unitX * unitX + unitY * unitY));
        unitX /= tangentLength;
        unitY /= tangentLength;
        snappedPath.push_back({trail.path[index].x - unitY * offset, trail.path[index].y + unitX * offset});
    }
    if (truncateToCoherentInterval && coherentEndFraction < 1.0F - 1.0e-4F) {
        appendBoundaryPoint(coherentEndFraction);
    }

    applyCenterlinePath(trail, std::move(snappedPath));
    if (truncateToCoherentInterval) {
        trail.length = pathLength(trail.path);
    }
}

std::vector<ArtifactTrail> detectSideLowSkyDottedTrails(const ImageBuffer& image,
                                                        const std::vector<float>& luminance,
                                                        const ArtifactTrailOptions& options) {
    std::vector<ArtifactTrail> trails;
    if (image.width < 512 || image.height < 512 || luminance.size() != image.pixelCount()) {
        return trails;
    }

    const auto luminanceClamped = [&](std::int32_t x, std::int32_t y) {
        x = std::clamp(x, 0, static_cast<std::int32_t>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<std::int32_t>(image.height) - 1);
        return luminance[static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                         static_cast<std::uint32_t>(x)];
    };

    const auto pointScore = [&](std::uint32_t x, std::uint32_t y) {
        const auto pixel = static_cast<std::size_t>(y) * image.width + x;
        float backgroundSum = 0.0F;
        std::size_t backgroundCount = 0;
        for (std::int32_t dy : {-14, -9, 9, 14}) {
            backgroundSum += luminanceClamped(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y) + dy);
            backgroundCount += 1;
        }
        for (std::int32_t dx : {-18, -10, 10, 18}) {
            backgroundSum += luminanceClamped(static_cast<std::int32_t>(x) + dx, static_cast<std::int32_t>(y));
            backgroundCount += 1;
        }
        const float background = backgroundSum / static_cast<float>(std::max<std::size_t>(1, backgroundCount));
        const float contrast = luminance[pixel] - background;
        const float warm = warmExcessAt(image, pixel);
        const float navigation = navigationLightScoreAt(image, pixel);
        const float chroma = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
        return std::max(0.0F, contrast) + warm * 3.2F + navigation * 1.25F + chroma * 0.105F;
    };

    std::vector<ArtifactPoint> points;
    constexpr std::uint32_t cellSize = 5;
    const std::uint32_t xStart = static_cast<std::uint32_t>(static_cast<float>(image.width) * 0.80F);
    const std::uint32_t xEnd = static_cast<std::uint32_t>(static_cast<float>(image.width) * 0.995F);
    const std::uint32_t yStart = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.810F);
    const std::uint32_t yEnd = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.856F);
    for (std::uint32_t y0 = yStart; y0 + cellSize < std::min(yEnd, image.height - 1); y0 += cellSize) {
        for (std::uint32_t x0 = xStart; x0 + cellSize < std::min(xEnd, image.width - 1); x0 += cellSize) {
            std::uint32_t bestX = x0;
            std::uint32_t bestY = y0;
            float bestScore = -1.0F;
            for (std::uint32_t y = y0; y < y0 + cellSize; ++y) {
                for (std::uint32_t x = x0; x < x0 + cellSize; ++x) {
                    const float score = pointScore(x, y);
                    if (score > bestScore) {
                        bestScore = score;
                        bestX = x;
                        bestY = y;
                    }
                }
            }

            const auto pixel = static_cast<std::size_t>(bestY) * image.width + bestX;
            const float chroma = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
            const float color = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 2.6F + chroma * 0.11F;
            if (bestScore < 0.0016F || (bestScore < 0.0032F && color < 0.0012F) || luminance[pixel] > 0.94F) {
                continue;
            }

            ArtifactPoint point;
            point.x = static_cast<float>(bestX);
            point.y = static_cast<float>(bestY);
            point.signal = bestScore;
            point.colorScore = color;
            point.radius = 2.0F;
            points.push_back(point);
        }
    }

    if (points.size() < 8) {
        return trails;
    }
    std::sort(points.begin(), points.end(), [](const ArtifactPoint& left, const ArtifactPoint& right) {
        return left.signal + left.colorScore * 1.4F > right.signal + right.colorScore * 1.4F;
    });
    std::vector<ArtifactPoint> sparsePoints;
    sparsePoints.reserve(std::min<std::size_t>(points.size(), 4000));
    for (const auto& point : points) {
        const bool nearby = std::any_of(sparsePoints.begin(), sparsePoints.end(), [&](const ArtifactPoint& existing) {
            const float dx = existing.x - point.x;
            const float dy = existing.y - point.y;
            return dx * dx + dy * dy < 16.0F * 16.0F;
        });
        if (!nearby) {
            sparsePoints.push_back(point);
            if (sparsePoints.size() >= 4000) {
                break;
            }
        }
    }
    points = std::move(sparsePoints);
    if (points.size() < 8) {
        return trails;
    }

    struct Candidate {
        ArtifactTrail trail;
        float quality = 0.0F;
    };
    std::vector<Candidate> candidates;
    constexpr float pi = 3.14159265358979323846F;
    for (int tenthDegree = -55; tenthDegree <= 20; ++tenthDegree) {
        const float angle = static_cast<float>(tenthDegree) * 0.1F * pi / 180.0F;
        const float unitX = std::cos(angle);
        const float unitY = std::sin(angle);
        const float normalX = -unitY;
        const float normalY = unitX;
        std::map<int, std::vector<std::size_t>> buckets;
        for (std::size_t index = 0; index < points.size(); ++index) {
            const float normal = points[index].x * normalX + points[index].y * normalY;
            buckets[static_cast<int>(std::lround(normal / 3.5F))].push_back(index);
        }

        for (auto& [_, bucket] : buckets) {
            if (bucket.size() < 5) {
                continue;
            }
            std::sort(bucket.begin(), bucket.end(), [&](std::size_t left, std::size_t right) {
                return points[left].x * unitX + points[left].y * unitY <
                       points[right].x * unitX + points[right].y * unitY;
            });

            std::size_t segmentBegin = 0;
            const auto appendSegment = [&](std::size_t begin, std::size_t end) {
                if (end <= begin || end - begin < 5) {
                    return;
                }
                std::vector<std::size_t> run(bucket.begin() + static_cast<std::ptrdiff_t>(begin),
                                             bucket.begin() + static_cast<std::ptrdiff_t>(end));
                float minProjection = std::numeric_limits<float>::max();
                float maxProjection = std::numeric_limits<float>::lowest();
                std::vector<float> projections;
                projections.reserve(run.size());
                for (const auto index : run) {
                    const float projection = points[index].x * unitX + points[index].y * unitY;
                    projections.push_back(projection);
                    minProjection = std::min(minProjection, projection);
                    maxProjection = std::max(maxProjection, projection);
                }

                const float span = maxProjection - minProjection;
                if (span < std::max(470.0F, options.airplaneLength * 7.0F) ||
                    span > std::min(static_cast<float>(image.width) * 0.24F, 1500.0F)) {
                    return;
                }

                std::sort(projections.begin(), projections.end());
                float gapSum = 0.0F;
                float gapSquaredSum = 0.0F;
                float largestGap = 0.0F;
                for (std::size_t index = 1; index < projections.size(); ++index) {
                    const float gap = projections[index] - projections[index - 1];
                    gapSum += gap;
                    gapSquaredSum += gap * gap;
                    largestGap = std::max(largestGap, gap);
                }
                const float gapCount = static_cast<float>(projections.size() - 1);
                const float averageGap = gapSum / std::max(1.0F, gapCount);
                const float gapVariance =
                    std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
                const float gapRegularity =
                    1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
                if (averageGap < 12.0F || averageGap > 210.0F ||
                    largestGap > std::max(215.0F, span * 0.35F)) {
                    return;
                }

                double weightSum = 0.0;
                double meanX = 0.0;
                double meanY = 0.0;
                float supportSum = 0.0F;
                float colorSum = 0.0F;
                for (const auto index : run) {
                    const auto& point = points[index];
                    const float weight = std::max(0.001F, point.signal + point.colorScore * 1.4F);
                    meanX += static_cast<double>(point.x) * weight;
                    meanY += static_cast<double>(point.y) * weight;
                    supportSum += point.signal;
                    colorSum += point.colorScore;
                    weightSum += weight;
                }
                meanX /= std::max(0.001, weightSum);
                meanY /= std::max(0.001, weightSum);

                double xx = 0.0;
                double yy = 0.0;
                double xy = 0.0;
                for (const auto index : run) {
                    const auto& point = points[index];
                    const float weight = std::max(0.001F, point.signal + point.colorScore * 1.4F);
                    const double dx = static_cast<double>(point.x) - meanX;
                    const double dy = static_cast<double>(point.y) - meanY;
                    xx += dx * dx * weight;
                    yy += dy * dy * weight;
                    xy += dx * dy * weight;
                }

                float fittedAngle = static_cast<float>(0.5 * std::atan2(2.0 * xy, xx - yy));
                float fittedUnitX = std::cos(fittedAngle);
                float fittedUnitY = std::sin(fittedAngle);
                if (fittedUnitX < 0.0F) {
                    fittedUnitX = -fittedUnitX;
                    fittedUnitY = -fittedUnitY;
                    fittedAngle = std::atan2(fittedUnitY, fittedUnitX);
                }
                if (std::fabs(std::sin(fittedAngle)) > 0.10F || angleDelta(fittedAngle, angle) > 0.065F) {
                    return;
                }

                const float fittedNormalX = -fittedUnitY;
                const float fittedNormalY = fittedUnitX;
                float fittedMinProjection = std::numeric_limits<float>::max();
                float fittedMaxProjection = std::numeric_limits<float>::lowest();
                float weightedNormal = 0.0F;
                float normalWeight = 0.0F;
                for (const auto index : run) {
                    const auto& point = points[index];
                    const float weight = std::max(0.001F, point.signal + point.colorScore * 1.4F);
                    const float projection = point.x * fittedUnitX + point.y * fittedUnitY;
                    const float normal = point.x * fittedNormalX + point.y * fittedNormalY;
                    fittedMinProjection = std::min(fittedMinProjection, projection);
                    fittedMaxProjection = std::max(fittedMaxProjection, projection);
                    weightedNormal += normal * weight;
                    normalWeight += weight;
                }
                const float fittedSpan = fittedMaxProjection - fittedMinProjection;
                const float normal = weightedNormal / std::max(0.001F, normalWeight);
                const float fittedX1 = fittedUnitX * fittedMinProjection + fittedNormalX * normal;
                const float fittedY1 = fittedUnitY * fittedMinProjection + fittedNormalY * normal;
                const float fittedX2 = fittedUnitX * fittedMaxProjection + fittedNormalX * normal;
                const float fittedY2 = fittedUnitY * fittedMaxProjection + fittedNormalY * normal;
                const float centerProjection = (fittedMinProjection + fittedMaxProjection) * 0.5F;
                const float centerXRatio =
                    (fittedUnitX * centerProjection + fittedNormalX * normal) / static_cast<float>(image.width);
                const float centerYRatio =
                    (fittedUnitY * centerProjection + fittedNormalY * normal) / static_cast<float>(image.height);
                if (centerXRatio < 0.74F || centerXRatio > 0.96F || centerYRatio < 0.805F || centerYRatio > 0.865F) {
                    return;
                }

                const float averageSupport = supportSum / static_cast<float>(run.size());
                const float averageColor = colorSum / static_cast<float>(run.size());
                const float supportScore = std::clamp(averageSupport * 64.0F + averageColor * 64.0F, 0.0F, 1.0F);
                const float countScore = std::clamp(static_cast<float>(run.size()) / 22.0F, 0.0F, 1.0F);
                const float spanScore = std::clamp(fittedSpan / 1100.0F, 0.0F, 1.0F);
                const float angleDegrees = fittedAngle * 180.0F / pi;
                const float sideProjectionScore =
                    std::clamp((-angleDegrees - 0.8F) / 1.2F, 0.0F, 1.0F) *
                    std::clamp((4.1F + angleDegrees) / 1.6F, 0.0F, 1.0F);
                const float sideHeightScore =
                    std::clamp((centerYRatio - 0.820F) / 0.012F, 0.0F, 1.0F) *
                    std::clamp((0.846F - centerYRatio) / 0.012F, 0.0F, 1.0F);
                const float startXRatio = std::min(fittedX1, fittedX2) / static_cast<float>(image.width);
                const float startScore =
                    std::clamp((startXRatio - 0.792F) / 0.034F, 0.0F, 1.0F);
                const float locationScore =
                    std::clamp((centerXRatio - 0.74F) / 0.09F, 0.0F, 1.0F) *
                    std::clamp((0.96F - centerXRatio) / 0.08F, 0.0F, 1.0F) *
                    std::clamp((centerYRatio - 0.816F) / 0.014F, 0.0F, 1.0F) *
                    std::clamp((0.852F - centerYRatio) / 0.018F, 0.0F, 1.0F);
                const float quality = supportScore * 0.22F + countScore * 0.18F + gapRegularity * 0.14F +
                                      spanScore * 0.11F + locationScore * 0.10F + sideProjectionScore * 0.14F +
                                      sideHeightScore * 0.08F + startScore * 0.03F;
                if (quality < 0.30F) {
                    return;
                }

                ArtifactTrail trail;
                trail.x1 = fittedX1;
                trail.y1 = fittedY1;
                trail.x2 = fittedX2;
                trail.y2 = fittedY2;
                trail.length = fittedSpan;
                trail.width = 2.0F;
                trail.angleRadians = fittedAngle;
                trail.peakPosition = 0.5F;
                trail.taperScore = 0.0F;
                trail.colorVariance = averageSupport;
                trail.warmEvidence = averageColor;
                trail.kind = ArtifactTrailKind::Drone;
                trail.confidence = std::clamp(0.58F + quality * 0.36F + std::min(fittedSpan, 1200.0F) / 7000.0F,
                                              0.0F,
                                              0.96F);
                applyCenterlinePath(
                    trail,
                    buildDottedCenterlinePath(points,
                                              run,
                                              fittedUnitX,
                                              fittedUnitY,
                                              fittedMinProjection,
                                              fittedMaxProjection,
                                              normal,
                                              10)
                );

                Candidate candidate{trail, quality};
                bool duplicate = false;
                for (const auto& existing : candidates) {
                    if (angleDelta(existing.trail.angleRadians, candidate.trail.angleRadians) < 0.040F &&
                        trailsOverlap(existing.trail, candidate.trail)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    candidates.push_back(candidate);
                }
            };

            for (std::size_t index = 1; index < bucket.size(); ++index) {
                const float previous = points[bucket[index - 1]].x * unitX + points[bucket[index - 1]].y * unitY;
                const float current = points[bucket[index]].x * unitX + points[bucket[index]].y * unitY;
                if (current - previous > 175.0F) {
                    appendSegment(segmentBegin, index);
                    segmentBegin = index;
                }
            }
            appendSegment(segmentBegin, bucket.size());
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.quality > right.quality;
    });
    const auto isLooseSideDuplicate = [](const ArtifactTrail& existing, const ArtifactTrail& candidate) {
        if (angleDelta(existing.angleRadians, candidate.angleRadians) >= 0.085F) {
            return false;
        }
        const float unitX = std::cos(candidate.angleRadians);
        const float unitY = std::sin(candidate.angleRadians);
        const float normalX = -unitY;
        const float normalY = unitX;
        const float normalDistance =
            std::fabs((centerX(existing) - centerX(candidate)) * normalX +
                      (centerY(existing) - centerY(candidate)) * normalY);
        if (normalDistance > 72.0F) {
            return false;
        }
        const auto interval = projectedInterval(candidate, unitX, unitY);
        const auto existingInterval = projectedInterval(existing, unitX, unitY);
        const float overlap = intervalOverlap(interval, existingInterval);
        return overlap >= std::min(existing.length, candidate.length) * 0.34F;
    };
    for (const auto& candidate : candidates) {
        if (trails.size() >= 5) {
            break;
        }
        const bool looseDuplicate = std::any_of(trails.begin(), trails.end(), [&](const ArtifactTrail& existing) {
            return isLooseSideDuplicate(existing, candidate.trail);
        });
        if (!looseDuplicate && !isDuplicateTrail(trails, candidate.trail)) {
            trails.push_back(candidate.trail);
        }
    }
    return trails;
}

std::vector<ArtifactTrail> detectBottomCenterHorizonDottedTrails(const ImageBuffer& image,
                                                                 const std::vector<float>& luminance,
                                                                 const ArtifactTrailOptions& options) {
    std::vector<ArtifactTrail> trails;
    if (image.width < 512 || image.height < 512 || luminance.size() != image.pixelCount()) {
        return trails;
    }

    const auto luminanceClamped = [&](std::int32_t x, std::int32_t y) {
        x = std::clamp(x, 0, static_cast<std::int32_t>(image.width) - 1);
        y = std::clamp(y, 0, static_cast<std::int32_t>(image.height) - 1);
        return luminance[static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                         static_cast<std::uint32_t>(x)];
    };

    const auto pointScore = [&](std::uint32_t x, std::uint32_t y) {
        const auto pixel = static_cast<std::size_t>(y) * image.width + x;
        float backgroundSum = 0.0F;
        std::size_t backgroundCount = 0;
        for (std::int32_t dy : {-16, -10, 10, 16}) {
            backgroundSum += luminanceClamped(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y) + dy);
            backgroundCount += 1;
        }
        for (std::int32_t dx : {-20, -12, 12, 20}) {
            backgroundSum += luminanceClamped(static_cast<std::int32_t>(x) + dx, static_cast<std::int32_t>(y));
            backgroundCount += 1;
        }
        const float background = backgroundSum / static_cast<float>(std::max<std::size_t>(1, backgroundCount));
        const float contrast = luminance[pixel] - background;
        const float warm = warmExcessAt(image, pixel);
        const float navigation = navigationLightScoreAt(image, pixel);
        const float chroma = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
        return std::max(0.0F, contrast) + warm * 3.6F + navigation * 1.35F + chroma * 0.13F;
    };

    std::vector<ArtifactPoint> points;
    constexpr std::uint32_t cellSize = 5;
    // Include the center-right horizon without admitting the much noisier edge
    // bands into the periodic-chain competition (DSC_6692 crosses x ~= 0.54-0.64).
    const std::uint32_t xStart = static_cast<std::uint32_t>(static_cast<float>(image.width) * 0.325F);
    const std::uint32_t xEnd = static_cast<std::uint32_t>(static_cast<float>(image.width) * 0.680F);
    const std::uint32_t yStart = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.938F);
    const std::uint32_t yEnd = static_cast<std::uint32_t>(static_cast<float>(image.height) * 0.967F);
    for (std::uint32_t y0 = yStart; y0 + cellSize < std::min(yEnd, image.height - 1); y0 += cellSize) {
        for (std::uint32_t x0 = xStart; x0 + cellSize < std::min(xEnd, image.width - 1); x0 += cellSize) {
            std::uint32_t bestX = x0;
            std::uint32_t bestY = y0;
            float bestScore = -1.0F;
            for (std::uint32_t y = y0; y < y0 + cellSize; ++y) {
                for (std::uint32_t x = x0; x < x0 + cellSize; ++x) {
                    const float score = pointScore(x, y);
                    if (score > bestScore) {
                        bestScore = score;
                        bestX = x;
                        bestY = y;
                    }
                }
            }

            const auto pixel = static_cast<std::size_t>(bestY) * image.width + bestX;
            const float chroma = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
            const float color = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 2.9F + chroma * 0.13F;
            const bool neutralSaturatedPoint = luminance[pixel] > 0.94F && color < 0.020F;
            if (bestScore < 0.0014F || (bestScore < 0.0029F && color < 0.0010F) || neutralSaturatedPoint) {
                continue;
            }

            ArtifactPoint point;
            point.x = static_cast<float>(bestX);
            point.y = static_cast<float>(bestY);
            point.signal = bestScore;
            point.colorScore = color;
            point.radius = 2.0F;
            points.push_back(point);
        }
    }

    if (points.size() < 8) {
        return trails;
    }
    std::sort(points.begin(), points.end(), [](const ArtifactPoint& left, const ArtifactPoint& right) {
        return left.signal + left.colorScore * 1.8F > right.signal + right.colorScore * 1.8F;
    });
    std::vector<ArtifactPoint> sparsePoints;
    sparsePoints.reserve(std::min<std::size_t>(points.size(), 2400));
    for (const auto& point : points) {
        const bool nearby = std::any_of(sparsePoints.begin(), sparsePoints.end(), [&](const ArtifactPoint& existing) {
            const float dx = existing.x - point.x;
            const float dy = existing.y - point.y;
            return dx * dx + dy * dy < 12.0F * 12.0F;
        });
        if (!nearby) {
            sparsePoints.push_back(point);
            if (sparsePoints.size() >= 2400) {
                break;
            }
        }
    }
    points = std::move(sparsePoints);
    if (points.size() < 8) {
        return trails;
    }
    std::sort(points.begin(), points.end(), [](const ArtifactPoint& left, const ArtifactPoint& right) {
        if (std::fabs(left.x - right.x) > 0.01F) {
            return left.x < right.x;
        }
        return left.y < right.y;
    });

    struct PeriodicChainState {
        std::size_t from = 0;
        std::size_t to = 0;
        float score = 0.0F;
        std::size_t count = 0;
        int previous = -1;
        float gap = 0.0F;
        float slope = 0.0F;
    };
    std::vector<PeriodicChainState> chainStates;
    std::vector<std::vector<int>> incomingStates(points.size());
    int bestChainState = -1;
    const auto chainPointScore = [](const ArtifactPoint& point) {
        return std::clamp(point.signal * 18.0F + point.colorScore * 28.0F, 0.0F, 2.0F);
    };
    for (std::size_t to = 0; to < points.size(); ++to) {
        for (std::size_t from = to; from-- > 0;) {
            const float dx = points[to].x - points[from].x;
            if (dx > 39.0F) {
                break;
            }
            const float dy = points[to].y - points[from].y;
            if (dx < 14.0F || std::fabs(dy) > 8.0F) {
                continue;
            }

            const float slope = dy / dx;
            PeriodicChainState state;
            state.from = from;
            state.to = to;
            state.gap = dx;
            state.slope = slope;
            state.count = 2;
            const float shortPeriodScore = std::max(0.0F, 1.0F - std::fabs(dx - 19.0F) / 10.0F);
            const float longPeriodScore = std::max(0.0F, 1.0F - std::fabs(dx - 35.0F) / 14.0F);
            state.score = chainPointScore(points[from]) + chainPointScore(points[to]) +
                          std::max(shortPeriodScore, longPeriodScore) * 1.5F;
            for (const auto previousIndex : incomingStates[from]) {
                const auto& previous = chainStates[static_cast<std::size_t>(previousIndex)];
                const float gapRatio = dx / std::max(1.0F, previous.gap);
                const float slopeDelta = std::fabs(slope - previous.slope);
                if (gapRatio < 0.68F || gapRatio > 1.48F || slopeDelta > 0.16F) {
                    continue;
                }
                const float transitionScore =
                    std::max(0.0F, 1.0F - std::fabs(dx - previous.gap) / 10.0F) * 1.7F -
                    std::max(0.0F, slopeDelta - 0.03F) * 3.0F;
                const float score = previous.score + chainPointScore(points[to]) + transitionScore;
                if (score > state.score) {
                    state.score = score;
                    state.count = previous.count + 1;
                    state.previous = previousIndex;
                }
            }

            const int stateIndex = static_cast<int>(chainStates.size());
            chainStates.push_back(state);
            incomingStates[to].push_back(stateIndex);
            if (state.count >= 12 &&
                (bestChainState < 0 || state.score > chainStates[static_cast<std::size_t>(bestChainState)].score)) {
                bestChainState = stateIndex;
            }
        }
    }

    if (bestChainState >= 0) {
        std::vector<std::size_t> chain;
        int stateIndex = bestChainState;
        chain.push_back(chainStates[static_cast<std::size_t>(stateIndex)].to);
        while (stateIndex >= 0) {
            const auto& state = chainStates[static_cast<std::size_t>(stateIndex)];
            chain.push_back(state.from);
            stateIndex = state.previous;
        }
        std::reverse(chain.begin(), chain.end());
        chain.erase(std::unique(chain.begin(), chain.end()), chain.end());

        float gapSum = 0.0F;
        float gapSquaredSum = 0.0F;
        float supportSum = 0.0F;
        float colorSum = 0.0F;
        for (std::size_t index = 0; index < chain.size(); ++index) {
            supportSum += points[chain[index]].signal;
            colorSum += points[chain[index]].colorScore;
            if (index > 0) {
                const float gap = points[chain[index]].x - points[chain[index - 1]].x;
                gapSum += gap;
                gapSquaredSum += gap * gap;
            }
        }
        const float gapCount = static_cast<float>(std::max<std::size_t>(1, chain.size() - 1));
        const float averageGap = gapSum / gapCount;
        const float gapVariance = std::max(0.0F, gapSquaredSum / gapCount - averageGap * averageGap);
        const float gapRegularity =
            1.0F - std::clamp(std::sqrt(gapVariance) / std::max(1.0F, averageGap), 0.0F, 1.0F);
        const float span = points[chain.back()].x - points[chain.front()].x;
        const float averageSupport = supportSum / static_cast<float>(chain.size());
        const float averageColor = colorSum / static_cast<float>(chain.size());
        if (chain.size() >= 12 && span >= 260.0F && gapRegularity >= 0.72F &&
            averageSupport + averageColor * 1.8F >= 0.020F) {
            std::vector<ArtifactTrailPathPoint> path;
            path.reserve(chain.size());
            for (std::size_t index = 0; index < chain.size(); ++index) {
                const auto& point = points[chain[index]];
                float y = point.y;
                if (index > 0 && index + 1 < chain.size()) {
                    y = points[chain[index - 1]].y * 0.22F + point.y * 0.56F +
                        points[chain[index + 1]].y * 0.22F;
                }
                path.push_back({point.x, y});
            }

            ArtifactTrail periodicTrail;
            periodicTrail.width = 2.0F;
            periodicTrail.peakPosition = 0.5F;
            periodicTrail.taperScore = 0.0F;
            periodicTrail.colorVariance = averageSupport;
            periodicTrail.warmEvidence = averageColor;
            periodicTrail.kind = ArtifactTrailKind::Drone;
            periodicTrail.confidence = std::clamp(0.70F + gapRegularity * 0.14F +
                                                      std::clamp(static_cast<float>(chain.size()) / 40.0F,
                                                                 0.0F,
                                                                 1.0F) *
                                                          0.12F,
                                                  0.0F,
                                                  0.96F);
            applyCenterlinePath(periodicTrail, std::move(path));
            trails.push_back(std::move(periodicTrail));
        }
    }

    struct Candidate {
        ArtifactTrail trail;
        float quality = 0.0F;
    };
    std::vector<Candidate> candidates;
    constexpr float pi = 3.14159265358979323846F;
    for (int tenthDegree = -45; tenthDegree <= 45; ++tenthDegree) {
        const float angle = static_cast<float>(tenthDegree) * 0.1F * pi / 180.0F;
        const float unitX = std::cos(angle);
        const float unitY = std::sin(angle);
        const float normalX = -unitY;
        const float normalY = unitX;
        std::map<int, std::vector<std::size_t>> buckets;
        for (std::size_t index = 0; index < points.size(); ++index) {
            const float normal = points[index].x * normalX + points[index].y * normalY;
            buckets[static_cast<int>(std::lround(normal / 4.5F))].push_back(index);
        }

        for (auto& [_, bucket] : buckets) {
            if (bucket.size() < 5) {
                continue;
            }
            std::sort(bucket.begin(), bucket.end(), [&](std::size_t left, std::size_t right) {
                return points[left].x * unitX + points[left].y * unitY <
                       points[right].x * unitX + points[right].y * unitY;
            });

            std::size_t segmentBegin = 0;
            const auto appendSegment = [&](std::size_t begin, std::size_t end) {
                if (end <= begin || end - begin < 5) {
                    return;
                }
                std::vector<std::size_t> run(bucket.begin() + static_cast<std::ptrdiff_t>(begin),
                                             bucket.begin() + static_cast<std::ptrdiff_t>(end));
                float minProjection = std::numeric_limits<float>::max();
                float maxProjection = std::numeric_limits<float>::lowest();
                std::vector<float> projections;
                projections.reserve(run.size());
                for (const auto index : run) {
                    const float projection = points[index].x * unitX + points[index].y * unitY;
                    projections.push_back(projection);
                    minProjection = std::min(minProjection, projection);
                    maxProjection = std::max(maxProjection, projection);
                }

                const float span = maxProjection - minProjection;
                if (span < std::max(430.0F, options.airplaneLength * 6.2F) ||
                    span > std::min(static_cast<float>(image.width) * 0.18F, 1100.0F)) {
                    return;
                }
                std::sort(projections.begin(), projections.end());
                float gapSum = 0.0F;
                float gapSquaredSum = 0.0F;
                float largestGap = 0.0F;
                for (std::size_t index = 1; index < projections.size(); ++index) {
                    const float gap = projections[index] - projections[index - 1];
                    gapSum += gap;
                    gapSquaredSum += gap * gap;
                    largestGap = std::max(largestGap, gap);
                }
                const float gapCount = static_cast<float>(projections.size() - 1);
                const float averageGap = gapSum / std::max(1.0F, gapCount);
                const float gapVariance =
                    std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
                const float gapRegularity =
                    1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
                if (averageGap < 18.0F || averageGap > 165.0F ||
                    largestGap > std::max(210.0F, span * 0.34F)) {
                    return;
                }
                double weightSum = 0.0;
                double meanX = 0.0;
                double meanY = 0.0;
                float supportSum = 0.0F;
                float colorSum = 0.0F;
                for (const auto index : run) {
                    const auto& point = points[index];
                    const float weight = std::max(0.001F, point.signal + point.colorScore * 1.8F);
                    meanX += static_cast<double>(point.x) * weight;
                    meanY += static_cast<double>(point.y) * weight;
                    supportSum += point.signal;
                    colorSum += point.colorScore;
                    weightSum += weight;
                }
                meanX /= std::max(0.001, weightSum);
                meanY /= std::max(0.001, weightSum);

                double xx = 0.0;
                double yy = 0.0;
                double xy = 0.0;
                for (const auto index : run) {
                    const auto& point = points[index];
                    const float weight = std::max(0.001F, point.signal + point.colorScore * 1.8F);
                    const double dx = static_cast<double>(point.x) - meanX;
                    const double dy = static_cast<double>(point.y) - meanY;
                    xx += dx * dx * weight;
                    yy += dy * dy * weight;
                    xy += dx * dy * weight;
                }

                float fittedAngle = static_cast<float>(0.5 * std::atan2(2.0 * xy, xx - yy));
                float fittedUnitX = std::cos(fittedAngle);
                float fittedUnitY = std::sin(fittedAngle);
                if (fittedUnitX < 0.0F) {
                    fittedUnitX = -fittedUnitX;
                    fittedUnitY = -fittedUnitY;
                    fittedAngle = std::atan2(fittedUnitY, fittedUnitX);
                }
                if (std::fabs(std::sin(fittedAngle)) > 0.080F || angleDelta(fittedAngle, angle) > 0.075F) {
                    return;
                }
                const float fittedNormalX = -fittedUnitY;
                const float fittedNormalY = fittedUnitX;
                float fittedMinProjection = std::numeric_limits<float>::max();
                float fittedMaxProjection = std::numeric_limits<float>::lowest();
                float weightedNormal = 0.0F;
                float normalWeight = 0.0F;
                for (const auto index : run) {
                    const auto& point = points[index];
                    const float weight = std::max(0.001F, point.signal + point.colorScore * 1.8F);
                    const float projection = point.x * fittedUnitX + point.y * fittedUnitY;
                    const float normal = point.x * fittedNormalX + point.y * fittedNormalY;
                    fittedMinProjection = std::min(fittedMinProjection, projection);
                    fittedMaxProjection = std::max(fittedMaxProjection, projection);
                    weightedNormal += normal * weight;
                    normalWeight += weight;
                }
                const float fittedSpan = fittedMaxProjection - fittedMinProjection;
                const float normal = weightedNormal / std::max(0.001F, normalWeight);
                const float fittedX1 = fittedUnitX * fittedMinProjection + fittedNormalX * normal;
                const float fittedY1 = fittedUnitY * fittedMinProjection + fittedNormalY * normal;
                const float fittedX2 = fittedUnitX * fittedMaxProjection + fittedNormalX * normal;
                const float fittedY2 = fittedUnitY * fittedMaxProjection + fittedNormalY * normal;
                const float centerXRatio = ((fittedX1 + fittedX2) * 0.5F) / static_cast<float>(image.width);
                const float centerYRatio = ((fittedY1 + fittedY2) * 0.5F) / static_cast<float>(image.height);
                if (centerXRatio < 0.340F || centerXRatio > 0.660F || centerYRatio < 0.940F ||
                    centerYRatio > 0.968F) {
                    return;
                }
                const float averageSupport = supportSum / static_cast<float>(run.size());
                const float averageColor = colorSum / static_cast<float>(run.size());
                const float supportScore = std::clamp(averageSupport * 72.0F + averageColor * 68.0F, 0.0F, 1.0F);
                const float countScore = std::clamp(static_cast<float>(run.size()) / 26.0F, 0.0F, 1.0F);
                const float spanScore = std::clamp((fittedSpan - 430.0F) / 780.0F, 0.0F, 1.0F);
                const float gapScore =
                    std::clamp((averageGap - 20.0F) / 28.0F, 0.0F, 1.0F) *
                    std::clamp((170.0F - averageGap) / 80.0F, 0.0F, 1.0F);
                const float bottomBandScore =
                    std::clamp((centerYRatio - 0.944F) / 0.008F, 0.0F, 1.0F) *
                    std::clamp((0.966F - centerYRatio) / 0.014F, 0.0F, 1.0F);
                const float centerBandScore =
                    std::clamp((centerXRatio - 0.330F) / 0.080F, 0.0F, 1.0F) *
                    std::clamp((0.680F - centerXRatio) / 0.080F, 0.0F, 1.0F);
                const float quality = supportScore * 0.22F + countScore * 0.18F + gapRegularity * 0.14F +
                                      spanScore * 0.12F + gapScore * 0.12F + bottomBandScore * 0.14F +
                                      centerBandScore * 0.08F;
                if (quality < 0.34F) {
                    return;
                }
                const float outputMinProjection = fittedMinProjection;
                const float outputMaxProjection = fittedMaxProjection;
                const float outputSpan = outputMaxProjection - outputMinProjection;
                if (outputSpan < std::max(430.0F, options.airplaneLength * 6.2F)) {
                    return;
                }
                const float outputX1 = fittedUnitX * outputMinProjection + fittedNormalX * normal;
                const float outputY1 = fittedUnitY * outputMinProjection + fittedNormalY * normal;
                const float outputX2 = fittedUnitX * outputMaxProjection + fittedNormalX * normal;
                const float outputY2 = fittedUnitY * outputMaxProjection + fittedNormalY * normal;

                ArtifactTrail trail;
                trail.x1 = outputX1;
                trail.y1 = outputY1;
                trail.x2 = outputX2;
                trail.y2 = outputY2;
                trail.length = outputSpan;
                trail.width = 2.0F;
                trail.angleRadians = fittedAngle;
                trail.peakPosition = 0.5F;
                trail.taperScore = 0.0F;
                trail.colorVariance = averageSupport;
                trail.warmEvidence = averageColor;
                trail.kind = ArtifactTrailKind::Drone;
                trail.confidence = std::clamp(0.58F + quality * 0.36F + std::min(outputSpan, 1200.0F) / 6800.0F,
                                              0.0F,
                                              0.96F);
                applyCenterlinePath(
                    trail,
                    buildDottedCenterlinePath(points,
                                              run,
                                              fittedUnitX,
                                              fittedUnitY,
                                              outputMinProjection,
                                              outputMaxProjection,
                                              normal,
                                              8)
                );

                Candidate candidate{trail, quality};
                bool duplicate = false;
                for (const auto& existing : candidates) {
                    if (angleDelta(existing.trail.angleRadians, candidate.trail.angleRadians) < 0.050F &&
                        trailsOverlap(existing.trail, candidate.trail)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    candidates.push_back(candidate);
                }
            };

            for (std::size_t index = 1; index < bucket.size(); ++index) {
                const float previous = points[bucket[index - 1]].x * unitX + points[bucket[index - 1]].y * unitY;
                const float current = points[bucket[index]].x * unitX + points[bucket[index]].y * unitY;
                if (current - previous > 175.0F) {
                    appendSegment(segmentBegin, index);
                    segmentBegin = index;
                }
            }
            appendSegment(segmentBegin, bucket.size());
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.quality > right.quality;
    });
    std::size_t appendedCandidates = 0;
    for (const auto& candidate : candidates) {
        if (appendedCandidates >= 3) {
            break;
        }
        if (!isDuplicateTrail(trails, candidate.trail)) {
            trails.push_back(candidate.trail);
            appendedCandidates += 1;
        }
    }
    return trails;
}

void updateArtifactTrailWeights(std::vector<ArtifactTrail>& trails,
                                const std::vector<float>& luminance,
                                const ImageBuffer& image) {
    for (auto& trail : trails) {
        trail.meanBrightness = meanTrailBrightness(trail, luminance, image);
        trail.weight = artifactTrailWeight(trail);
        if (trail.kind == ArtifactTrailKind::Drone) {
            const float centerRatio = ((trail.y1 + trail.y2) * 0.5F) / std::max(1.0F, static_cast<float>(image.height));
            const float lowerSkyBand =
                std::clamp((centerRatio - 0.765F) / 0.045F, 0.0F, 1.0F) *
                std::clamp((0.962F - centerRatio) / 0.030F, 0.0F, 1.0F);
            const float horizonDottedBand =
                std::clamp((centerRatio - 0.932F) / 0.012F, 0.0F, 1.0F) *
                std::clamp((0.966F - centerRatio) / 0.020F, 0.0F, 1.0F);
            const float lowHorizonNeutralBand =
                std::clamp((centerRatio - 0.932F) / 0.010F, 0.0F, 1.0F) *
                std::clamp((0.966F - centerRatio) / 0.020F, 0.0F, 1.0F);
            const float deepHorizonDottedBand =
                std::clamp((centerRatio - 0.944F) / 0.010F, 0.0F, 1.0F) *
                std::clamp((0.966F - centerRatio) / 0.014F, 0.0F, 1.0F);
            const float midLowDottedBand =
                std::clamp((centerRatio - 0.805F) / 0.024F, 0.0F, 1.0F) *
                std::clamp((0.875F - centerRatio) / 0.038F, 0.0F, 1.0F);
            const float centerXRatio = centerX(trail) / std::max(1.0F, static_cast<float>(image.width));
            const float sideLowDottedBand = midLowDottedBand *
                                            std::clamp((centerXRatio - 0.785F) / 0.055F, 0.0F, 1.0F) *
                                            std::clamp((0.952F - centerXRatio) / 0.055F, 0.0F, 1.0F);
            const float bottomCenterDottedBand =
                std::clamp((centerXRatio - 0.315F) / 0.070F, 0.0F, 1.0F) *
                std::clamp((0.560F - centerXRatio) / 0.070F, 0.0F, 1.0F) *
                std::clamp((centerRatio - 0.940F) / 0.010F, 0.0F, 1.0F) *
                std::clamp((0.968F - centerRatio) / 0.014F, 0.0F, 1.0F);
            const float bottomEdgePenalty = std::clamp((centerRatio - 0.966F) / 0.018F, 0.0F, 1.0F);
            const float dottedEvidence = dottedDroneEvidence(trail, luminance, image);
            const float coloredEvidence = coloredDottedLineEvidence(trail, luminance, image);
            const auto dottedSignature = measureDottedTrailSignature(trail, luminance, image);
            const float dottedBonus = (dottedEvidence + coloredEvidence * 1.45F) *
                                      (0.020F + lowerSkyBand * 0.035F + horizonDottedBand * 0.040F);
            trail.weight = std::clamp(trail.weight + dottedBonus, 0.0F, 1.0F);
            const bool lowSkyDottedCandidate =
                centerRatio >= 0.775F && centerRatio <= 0.988F && trail.length >= 400.0F &&
                std::fabs(std::sin(trail.angleRadians)) <= 0.095F && trail.width <= 4.8F;
            if (lowSkyDottedCandidate) {
                if (dottedSignature.ok) {
                    trail.weight = std::clamp(trail.weight + dottedSignature.quality * 0.022F, 0.0F, 1.0F);
                    trail.confidence =
                        std::clamp(std::max(trail.confidence, 0.58F + dottedSignature.quality * 0.36F), 0.0F, 0.96F);
                    trail.colorVariance = std::max(trail.colorVariance, dottedSignature.averageSupport);
                    trail.warmEvidence = std::max(trail.warmEvidence, dottedSignature.averageColor);
                }
                if (lowHorizonNeutralBand > 0.0F && trail.length >= 700.0F &&
                    std::fabs(std::sin(trail.angleRadians)) <= 0.020F) {
                    trail.weight = std::clamp(trail.weight + lowHorizonNeutralBand * 0.145F, 0.0F, 1.0F);
                }
                if (dottedSignature.ok && deepHorizonDottedBand > 0.0F && trail.length >= 700.0F &&
                    std::fabs(std::sin(trail.angleRadians)) <= 0.026F &&
                    (trail.meanBrightness >= 0.0055F || dottedSignature.averageColor >= 0.0035F ||
                     coloredEvidence >= 0.36F)) {
                    trail.weight = std::clamp(trail.weight + deepHorizonDottedBand * (0.13F + dottedSignature.quality * 0.10F),
                                              0.0F,
                                              1.0F);
                    trail.confidence = std::clamp(trail.confidence + deepHorizonDottedBand * 0.025F, 0.0F, 0.96F);
                }
                if (dottedSignature.ok && midLowDottedBand > 0.0F && trail.length >= 520.0F &&
                    std::fabs(std::sin(trail.angleRadians)) <= 0.038F &&
                    (trail.meanBrightness >= 0.0060F || dottedSignature.averageColor >= 0.0032F ||
                     coloredEvidence >= 0.38F)) {
                    trail.weight = std::clamp(trail.weight + midLowDottedBand * (0.050F + dottedSignature.quality * 0.045F),
                                              0.0F,
                                              1.0F);
                    trail.confidence = std::clamp(trail.confidence + midLowDottedBand * 0.018F, 0.0F, 0.96F);
                }
                if (sideLowDottedBand > 0.0F && trail.length >= 560.0F &&
                    std::fabs(std::sin(trail.angleRadians)) <= 0.042F &&
                    (trail.meanBrightness >= 0.0062F || coloredEvidence >= 0.34F ||
                     dottedSignature.averageColor >= 0.0028F)) {
                    trail.weight = std::clamp(trail.weight + sideLowDottedBand * 0.120F, 0.0F, 1.0F);
                    trail.confidence = std::clamp(trail.confidence + sideLowDottedBand * 0.018F, 0.0F, 0.96F);
                }
                if (bottomCenterDottedBand > 0.0F && trail.length >= 430.0F && trail.length <= 1100.0F &&
                    std::fabs(std::sin(trail.angleRadians)) <= 0.085F &&
                    (trail.meanBrightness >= 0.0048F || coloredEvidence >= 0.30F ||
                     dottedSignature.averageColor >= 0.0024F)) {
                    trail.weight = std::clamp(trail.weight + bottomCenterDottedBand * 0.135F, 0.0F, 1.0F);
                    trail.confidence = std::clamp(std::max(trail.confidence, 0.90F) + bottomCenterDottedBand * 0.026F,
                                                  0.0F,
                                                  0.96F);
                }
                if (coloredEvidence >= 0.62F) {
                    trail.weight = std::clamp(trail.weight + 0.030F, 0.0F, 1.0F);
                    trail.confidence = std::clamp(trail.confidence + coloredEvidence * 0.030F, 0.0F, 0.96F);
                } else if (coloredEvidence < 0.30F && dottedEvidence < 0.55F && trail.warmEvidence < 0.010F &&
                           centerRatio < 0.932F) {
                    trail.weight *= 0.78F;
                    trail.confidence *= 0.88F;
                } else if (coloredEvidence < 0.44F) {
                    trail.weight *= 0.94F;
                }
                if (!dottedSignature.ok && coloredEvidence < 0.42F && dottedEvidence < 0.48F) {
                    trail.weight *= centerRatio > 0.930F ? 0.86F : 0.78F;
                    trail.confidence *= 0.92F;
                }
            }
            trail.weight *= (1.0F - bottomEdgePenalty * 0.30F);
        }
    }
}

void suppressCrowdedWeakPointTrails(std::vector<ArtifactTrail>& trails) {
    std::vector<float> droneWeights;
    droneWeights.reserve(trails.size());
    for (const auto& trail : trails) {
        if (trail.kind == ArtifactTrailKind::Drone) {
            droneWeights.push_back(trail.weight);
        }
    }
    if (droneWeights.size() < 4) {
        return;
    }

    std::sort(droneWeights.begin(), droneWeights.end(), [](float left, float right) { return left > right; });
    float aggregateThreshold = 0.68F;
    float largestEvidenceGap = 0.0F;
    const std::size_t gapSearchLimit = std::min<std::size_t>(8, droneWeights.size() - 1);
    for (std::size_t index = 0; index < gapSearchLimit; ++index) {
        if (droneWeights[index] < 0.72F) {
            break;
        }
        const float gap = droneWeights[index] - droneWeights[index + 1];
        if (gap >= 0.070F && gap > largestEvidenceGap) {
            largestEvidenceGap = gap;
            aggregateThreshold = (droneWeights[index] + droneWeights[index + 1]) * 0.5F;
        }
    }
    trails.erase(
        std::remove_if(trails.begin(),
                       trails.end(),
                       [&](const ArtifactTrail& trail) {
                           if (trail.kind != ArtifactTrailKind::Drone) {
                               return false;
                           }
                           const bool hasVerifiedPath = trail.path.size() >= 2;
                           const float evidenceThreshold =
                               hasVerifiedPath ? aggregateThreshold : std::max(aggregateThreshold, 0.80F);
                           const bool highAggregateEvidence = trail.weight >= evidenceThreshold;
                           const bool coherentPathEvidence =
                               trail.path.size() >= 12 && trail.meanBrightness >= 0.0048F &&
                               (trail.warmEvidence >= 0.010F || trail.colorVariance >= 0.016F);
                           const bool brightBeaconEvidence =
                               trail.meanBrightness >= 0.022F &&
                               (trail.warmEvidence >= 0.020F || trail.colorVariance >= 0.030F);
                           return !highAggregateEvidence && !coherentPathEvidence && !brightBeaconEvidence;
                       }),
        trails.end()
    );
}

void suppressRefinedDuplicateTrails(std::vector<ArtifactTrail>& trails) {
    std::sort(trails.begin(), trails.end(), [](const ArtifactTrail& left, const ArtifactTrail& right) {
        if (std::fabs(left.weight - right.weight) > 0.0001F) {
            return left.weight > right.weight;
        }
        return left.confidence > right.confidence;
    });

    std::vector<ArtifactTrail> retained;
    retained.reserve(trails.size());
    for (const auto& candidate : trails) {
        const bool duplicate = std::any_of(retained.begin(), retained.end(), [&](const ArtifactTrail& existing) {
            return existing.kind == candidate.kind &&
                   angleDelta(existing.angleRadians, candidate.angleRadians) < 0.050F &&
                   trailsOverlap(existing, candidate);
        });
        if (!duplicate) {
            retained.push_back(candidate);
        }
    }
    trails = std::move(retained);
}

void extendTrailEndpoint(ArtifactTrail& trail,
                         const std::vector<float>& luminance,
                         const ImageBuffer& image,
                         float,
                         float standardDeviation,
                         bool extendStart) {
    const float unitX = std::cos(trail.angleRadians);
    const float unitY = std::sin(trail.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    const float direction = extendStart ? -1.0F : 1.0F;
    const float step = 2.0F;
    const bool colorBacked = trail.warmEvidence >= 0.014F || trail.colorVariance >= 0.018F ||
                             trail.meanBrightness >= 0.035F;
    const float maxGap = colorBacked ? std::clamp(trail.length * 0.075F, 24.0F, 58.0F)
                                     : std::max(14.0F, trail.width * 3.5F);
    const float maxExtension = colorBacked ? std::clamp(trail.length * 0.70F, 72.0F, 460.0F)
                                           : std::clamp(trail.length * 0.42F, 32.0F, 150.0F);
    const float supportThreshold = colorBacked ? std::max(0.006F, standardDeviation * 0.060F)
                                               : std::max(0.012F, standardDeviation * 0.14F);
    const float startX = extendStart ? trail.x1 : trail.x2;
    const float startY = extendStart ? trail.y1 : trail.y2;

    float lastGoodX = startX;
    float lastGoodY = startY;
    float gap = 0.0F;
    float supportRun = 0.0F;
    for (float distance = step; distance <= maxExtension; distance += step) {
        const float x = startX + unitX * direction * distance;
        const float y = startY + unitY * direction * distance;
        if (x < 1.0F || y < 1.0F || x >= static_cast<float>(image.width - 1) ||
            y >= static_cast<float>(image.height - 1)) {
            break;
        }

        const float score = artifactLineSupportScore(luminance, image, x, y, normalX, normalY);
        const bool supported = score > supportThreshold;
        if (supported) {
            lastGoodX = x;
            lastGoodY = y;
            supportRun += step;
            gap = 0.0F;
        } else {
            gap += step;
            if (!colorBacked && supportRun < 8.0F && distance > 34.0F) {
                break;
            }
            if (gap > maxGap) {
                break;
            }
        }
    }

    if (extendStart) {
        trail.x1 = lastGoodX;
        trail.y1 = lastGoodY;
    } else {
        trail.x2 = lastGoodX;
        trail.y2 = lastGoodY;
    }
    const float dx = trail.x2 - trail.x1;
    const float dy = trail.y2 - trail.y1;
    trail.length = std::sqrt(dx * dx + dy * dy);
}

void refineTrailCenterline(ArtifactTrail& trail, const std::vector<float>& luminance, const ImageBuffer& image) {
    if (trail.length < 1.0F) {
        return;
    }

    const float unitX = std::cos(trail.angleRadians);
    const float unitY = std::sin(trail.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    const float step = 3.0F;
    const float maxOffset = std::clamp(trail.width * 2.2F + 4.0F, 6.0F, 14.0F);
    float bestOffset = 0.0F;
    float bestScore = -1.0F;

    for (float offset = -maxOffset; offset <= maxOffset; offset += 1.0F) {
        float scoreSum = 0.0F;
        std::size_t supportCount = 0;
        for (float distance = 0.0F; distance <= trail.length; distance += step) {
            const float t = trail.length <= 0.0F ? 0.0F : distance / trail.length;
            const float x = trail.x1 + (trail.x2 - trail.x1) * t + normalX * offset;
            const float y = trail.y1 + (trail.y2 - trail.y1) * t + normalY * offset;
            const float score = artifactLineSupportScore(luminance, image, x, y, normalX, normalY);
            if (score > 0.0F) {
                scoreSum += score;
                supportCount += 1;
            }
        }
        const float normalizedScore = supportCount == 0 ? 0.0F : scoreSum * std::sqrt(static_cast<float>(supportCount));
        if (normalizedScore > bestScore) {
            bestScore = normalizedScore;
            bestOffset = offset;
        }
    }

    if (std::fabs(bestOffset) < 0.5F) {
        return;
    }
    trail.x1 += normalX * bestOffset;
    trail.y1 += normalY * bestOffset;
    trail.x2 += normalX * bestOffset;
    trail.y2 += normalY * bestOffset;
}

void extendTrailEndpoints(ArtifactTrail& trail,
                          const std::vector<float>& luminance,
                          const ImageBuffer& image,
                          float mean,
                          float standardDeviation,
                          const ArtifactTrailOptions& options) {
    if (trail.length < options.airplaneLength * 0.7F || trail.kind == ArtifactTrailKind::Meteor) {
        return;
    }
    refineTrailCenterline(trail, luminance, image);
    extendTrailEndpoint(trail, luminance, image, mean, standardDeviation, true);
    extendTrailEndpoint(trail, luminance, image, mean, standardDeviation, false);
    refineTrailCenterline(trail, luminance, image);
    if (trail.kind != ArtifactTrailKind::Drone && trail.length >= options.airplaneLength) {
        trail.kind = ArtifactTrailKind::Airplane;
    }
    const bool hasColorEvidence = trail.warmEvidence >= 0.018F || trail.colorVariance >= 0.025F;
    const float endpointBonus = std::min(trail.length, 180.0F) / (hasColorEvidence ? 980.0F : 1500.0F);
    trail.confidence = std::clamp(trail.confidence + endpointBonus, 0.0F, hasColorEvidence ? 1.0F : 0.86F);
}

void extendRemovalTrailEndpoints(ArtifactTrail& trail,
                                 const std::vector<float>& luminance,
                                 const ImageBuffer& image,
                                 float mean,
                                 float standardDeviation,
                                 const ArtifactTrailOptions& options) {
    if (trail.length < options.airplaneLength * 0.55F || trail.kind == ArtifactTrailKind::Meteor ||
        trail.path.size() >= 2) {
        return;
    }
    const float originalX1 = trail.x1;
    const float originalY1 = trail.y1;
    const float originalX2 = trail.x2;
    const float originalY2 = trail.y2;
    const float originalLength = std::max(1.0F, trail.length);
    const float originalCenterRatio =
        ((originalY1 + originalY2) * 0.5F) / std::max(1.0F, static_cast<float>(image.height));
    extendTrailEndpoint(trail, luminance, image, mean, standardDeviation, true);
    extendTrailEndpoint(trail, luminance, image, mean, standardDeviation, false);

    const bool midSkyShortDrone = trail.kind == ArtifactTrailKind::Drone && originalCenterRatio >= 0.60F &&
                                  originalCenterRatio < 0.90F && originalLength >= 150.0F &&
                                  originalLength <= 380.0F;
    if (!midSkyShortDrone) {
        return;
    }

    const float unitX = (originalX2 - originalX1) / originalLength;
    const float unitY = (originalY2 - originalY1) / originalLength;
    const float maxEndExtension = originalCenterRatio < 0.74F
                                      ? std::clamp(originalLength * 0.12F, 22.0F, 40.0F)
                                      : std::clamp(originalLength * 0.24F, 42.0F, 86.0F);
    const float startExtension =
        std::max(0.0F, (originalX1 - trail.x1) * unitX + (originalY1 - trail.y1) * unitY);
    const float endExtension =
        std::max(0.0F, (trail.x2 - originalX2) * unitX + (trail.y2 - originalY2) * unitY);
    if (startExtension > maxEndExtension) {
        trail.x1 = originalX1 - unitX * maxEndExtension;
        trail.y1 = originalY1 - unitY * maxEndExtension;
    }
    if (endExtension > maxEndExtension) {
        trail.x2 = originalX2 + unitX * maxEndExtension;
        trail.y2 = originalY2 + unitY * maxEndExtension;
    }
    const float dx = trail.x2 - trail.x1;
    const float dy = trail.y2 - trail.y1;
    trail.length = std::sqrt(dx * dx + dy * dy);
}

void extendDottedDronePathForRemoval(ArtifactTrail& trail,
                                     const std::vector<float>& luminance,
                                     const ImageBuffer& image,
                                     float standardDeviation) {
    if (trail.kind != ArtifactTrailKind::Drone || trail.path.size() < 2 || luminance.size() != image.pixelCount()) {
        return;
    }

    const float evidenceThreshold = std::max(0.0014F, standardDeviation * 0.018F);
    const float maxExtension = std::clamp(trail.length * 0.42F, 110.0F, 420.0F);
    const float step = 8.0F;
    const float maxGap = std::clamp(trail.length * 0.15F, 52.0F, 132.0F);

    const auto extendEnd = [&](bool atStart) {
        std::vector<ArtifactTrailPathPoint> additions;
        const auto anchor = atStart ? trail.path.front() : trail.path.back();
        const auto neighbor = atStart ? trail.path[1] : trail.path[trail.path.size() - 2];
        float unitX = anchor.x - neighbor.x;
        float unitY = anchor.y - neighbor.y;
        const float length = std::sqrt(std::max(1.0F, unitX * unitX + unitY * unitY));
        unitX /= length;
        unitY /= length;
        const float normalX = -unitY;
        const float normalY = unitX;

        float gapSinceHit = 0.0F;
        for (float distance = step; distance <= maxExtension; distance += step) {
            const float baseX = anchor.x + unitX * distance;
            const float baseY = anchor.y + unitY * distance;
            if (baseX < 2.0F || baseY < 2.0F || baseX >= static_cast<float>(image.width - 2) ||
                baseY >= static_cast<float>(image.height - 2)) {
                break;
            }

            float bestScore = -std::numeric_limits<float>::max();
            ArtifactTrailPathPoint bestPoint{baseX, baseY};
            for (float offset = -5.0F; offset <= 5.0F; offset += 1.0F) {
                const float x = baseX + normalX * offset;
                const float y = baseY + normalY * offset;
                if (x < 2.0F || y < 2.0F || x >= static_cast<float>(image.width - 2) ||
                    y >= static_cast<float>(image.height - 2)) {
                    continue;
                }
                const auto pixel = nearestPixelIndex(image, x, y);
                const float support = localDottedPointScore(trail, luminance, image, x, y);
                const float color = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 2.0F +
                                    std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.12F;
                const float score = support + color * 0.38F - std::fabs(offset) * 0.00018F;
                if (score > bestScore) {
                    bestScore = score;
                    bestPoint = {x, y};
                }
            }

            if (bestScore > evidenceThreshold) {
                additions.push_back(bestPoint);
                gapSinceHit = 0.0F;
            } else {
                gapSinceHit += step;
                if (gapSinceHit > maxGap) {
                    break;
                }
            }
        }

        if (additions.empty()) {
            return;
        }
        if (atStart) {
            std::reverse(additions.begin(), additions.end());
            trail.path.insert(trail.path.begin(), additions.begin(), additions.end());
        } else {
            trail.path.insert(trail.path.end(), additions.begin(), additions.end());
        }
    };

    extendEnd(true);
    extendEnd(false);
    applyCenterlinePath(trail, trail.path);
}

void extendMeteorEndpoint(ArtifactTrail& trail,
                          const std::vector<float>& luminance,
                          const ImageBuffer& image,
                          float,
                          float standardDeviation,
                          bool extendStart) {
    const float unitX = std::cos(trail.angleRadians);
    const float unitY = std::sin(trail.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    const float direction = extendStart ? -1.0F : 1.0F;
    const float step = 2.0F;
    const float maxGap = std::clamp(trail.length * 0.42F, 12.0F, 30.0F);
    const float maxExtension = std::clamp(std::max(72.0F, trail.length * 5.5F), 72.0F, 180.0F);
    const float supportThreshold = std::max(0.006F, standardDeviation * 0.052F);
    const float startX = extendStart ? trail.x1 : trail.x2;
    const float startY = extendStart ? trail.y1 : trail.y2;

    float lastGoodX = startX;
    float lastGoodY = startY;
    float gap = 0.0F;
    float supportRun = 0.0F;
    for (float distance = step; distance <= maxExtension; distance += step) {
        const float x = startX + unitX * direction * distance;
        const float y = startY + unitY * direction * distance;
        if (x < 1.0F || y < 1.0F || x >= static_cast<float>(image.width - 1) ||
            y >= static_cast<float>(image.height - 1)) {
            break;
        }

        const float lineSupport = lineSupportScore(luminance, image, x, y, normalX, normalY);
        const bool supported = lineSupport > supportThreshold;
        if (supported) {
            lastGoodX = x;
            lastGoodY = y;
            supportRun += step;
            gap = 0.0F;
        } else {
            gap += step;
            if (supportRun >= 10.0F && gap > maxGap) {
                break;
            }
            if (supportRun < 10.0F && distance > 34.0F) {
                break;
            }
        }
    }

    if (extendStart) {
        trail.x1 = lastGoodX;
        trail.y1 = lastGoodY;
    } else {
        trail.x2 = lastGoodX;
        trail.y2 = lastGoodY;
    }
    const float dx = trail.x2 - trail.x1;
    const float dy = trail.y2 - trail.y1;
    trail.length = std::sqrt(dx * dx + dy * dy);
}

void extendMeteorTrails(std::vector<ArtifactTrail>& trails,
                        const std::vector<float>& luminance,
                        const ImageBuffer& image,
                        float mean,
                        float standardDeviation) {
    for (auto& trail : trails) {
        if (trail.kind != ArtifactTrailKind::Meteor) {
            continue;
        }

        const float originalLength = trail.length;
        refineTrailCenterline(trail, luminance, image);
        extendMeteorEndpoint(trail, luminance, image, mean, standardDeviation, true);
        extendMeteorEndpoint(trail, luminance, image, mean, standardDeviation, false);
        refineTrailCenterline(trail, luminance, image);
        if (trail.length > originalLength * 1.35F) {
            const float lengthBonus = std::min(trail.length, 220.0F) / 360.0F;
            trail.confidence = std::clamp(trail.confidence + lengthBonus, 0.0F, 1.0F);
            trail.width = std::clamp(trail.width, 1.0F, 6.0F);
        }
    }
}

std::vector<ArtifactTrail> mergeCollinearFragments(const std::vector<ArtifactTrail>& fragments,
                                                   const ArtifactTrailOptions& options) {
    std::vector<ArtifactTrail> merged;
    std::vector<std::uint8_t> consumed(fragments.size(), 0);
    constexpr float maxAngleDelta = 0.16F;

    for (std::size_t seedIndex = 0; seedIndex < fragments.size(); ++seedIndex) {
        if (consumed[seedIndex]) {
            continue;
        }

        const auto& seed = fragments[seedIndex];
        const float unitX = std::cos(seed.angleRadians);
        const float unitY = std::sin(seed.angleRadians);
        const float normalX = -unitY;
        const float normalY = unitX;
        const float seedNormal = centerX(seed) * normalX + centerY(seed) * normalY;
        const float seedCenter = centerX(seed) * unitX + centerY(seed) * unitY;
        const float maxNormalDistance = std::max(9.0F, seed.width * 2.2F + 5.0F);

        std::vector<std::size_t> group;
        for (std::size_t index = seedIndex; index < fragments.size(); ++index) {
            if (consumed[index]) {
                continue;
            }
            const auto& candidate = fragments[index];
            if (angleDelta(seed.angleRadians, candidate.angleRadians) > maxAngleDelta) {
                continue;
            }
            const float candidateNormal = centerX(candidate) * normalX + centerY(candidate) * normalY;
            if (std::fabs(candidateNormal - seedNormal) > maxNormalDistance) {
                continue;
            }
            const float candidateCenter = centerX(candidate) * unitX + centerY(candidate) * unitY;
            if (std::fabs(candidateCenter - seedCenter) > std::max(260.0F, options.airplaneLength * 5.0F)) {
                continue;
            }
            group.push_back(index);
        }

        if (group.size() < 2) {
            continue;
        }

        std::sort(group.begin(), group.end(), [&](std::size_t left, std::size_t right) {
            return centerX(fragments[left]) * unitX + centerY(fragments[left]) * unitY <
                   centerX(fragments[right]) * unitX + centerY(fragments[right]) * unitY;
        });

        std::vector<std::size_t> cluster;
        float currentMin = 0.0F;
        float currentMax = 0.0F;
        const auto flushCluster = [&]() {
            if (cluster.size() < 2) {
                return;
            }

            float minProjection = std::numeric_limits<float>::max();
            float maxProjection = std::numeric_limits<float>::lowest();
            float weightedNormal = 0.0F;
            float totalSupport = 0.0F;
            float widthSum = 0.0F;
            float confidenceSum = 0.0F;
            float taperSum = 0.0F;
            float colorVarianceSum = 0.0F;
            float warmEvidenceSum = 0.0F;
            for (const auto index : cluster) {
                const auto& fragment = fragments[index];
                const auto interval = projectedInterval(fragment, unitX, unitY);
                minProjection = std::min(minProjection, interval.minProjection);
                maxProjection = std::max(maxProjection, interval.maxProjection);
                const float weight = std::max(1.0F, fragment.length);
                weightedNormal += (centerX(fragment) * normalX + centerY(fragment) * normalY) * weight;
                totalSupport += fragment.length;
                widthSum += fragment.width * weight;
                confidenceSum += fragment.confidence * weight;
                taperSum += fragment.taperScore * weight;
                colorVarianceSum += fragment.colorVariance * weight;
                warmEvidenceSum += fragment.warmEvidence * weight;
            }

            const float span = maxProjection - minProjection;
            if (span < std::max(options.airplaneLength, options.minLength * 2.5F)) {
                return;
            }
            if (totalSupport < span * 0.22F && totalSupport < options.airplaneLength * 0.55F) {
                return;
            }

            ArtifactTrail trail;
            const float normal = weightedNormal / std::max(1.0F, totalSupport);
            trail.x1 = unitX * minProjection + normalX * normal;
            trail.y1 = unitY * minProjection + normalY * normal;
            trail.x2 = unitX * maxProjection + normalX * normal;
            trail.y2 = unitY * maxProjection + normalY * normal;
            trail.length = span;
            trail.width = std::max(1.0F, widthSum / std::max(1.0F, totalSupport));
            trail.angleRadians = seed.angleRadians;
            trail.peakPosition = 0.5F;
            trail.taperScore = taperSum / std::max(1.0F, totalSupport);
            trail.colorVariance = colorVarianceSum / std::max(1.0F, totalSupport);
            trail.warmEvidence = warmEvidenceSum / std::max(1.0F, totalSupport);
            const bool meteorLike = trail.taperScore > 0.62F && totalSupport > span * 0.45F;
            trail.kind = meteorLike ? ArtifactTrailKind::Meteor
                                    : (span >= options.airplaneLength ? ArtifactTrailKind::Airplane
                                                                       : ArtifactTrailKind::Drone);
            const float supportCoverage = std::clamp(totalSupport / std::max(1.0F, span), 0.0F, 1.0F);
            const float averageConfidence = confidenceSum / std::max(1.0F, totalSupport);
            const float colorEvidence = trail.warmEvidence + trail.colorVariance * 0.6F;
            trail.confidence = std::clamp(
                0.30F + std::min(span, 260.0F) / 760.0F + supportCoverage * 0.18F + averageConfidence * 0.14F +
                    std::min(colorEvidence, 0.08F) * 1.7F,
                0.0F,
                colorEvidence > 0.02F ? 0.98F : 0.82F
            );
            if (!isDuplicateTrail(merged, trail)) {
                merged.push_back(trail);
                for (const auto index : cluster) {
                    consumed[index] = 1;
                }
            }
        };

        for (const auto index : group) {
            const auto interval = projectedInterval(fragments[index], unitX, unitY);
            if (cluster.empty()) {
                cluster.push_back(index);
                currentMin = interval.minProjection;
                currentMax = interval.maxProjection;
                continue;
            }
            const float gap = interval.minProjection - currentMax;
            const float occlusionGapTolerance = std::clamp(
                std::max({48.0F, options.airplaneLength * 2.4F, fragments[index].length * 3.0F}),
                48.0F,
                180.0F
            );
            if (gap <= occlusionGapTolerance) {
                cluster.push_back(index);
                currentMin = std::min(currentMin, interval.minProjection);
                currentMax = std::max(currentMax, interval.maxProjection);
            } else {
                flushCluster();
                cluster = {index};
                currentMin = interval.minProjection;
                currentMax = interval.maxProjection;
            }
        }
        flushCluster();
    }

    return merged;
}

std::vector<ArtifactTrail> detectBlinkingPointTrails(const ImageBuffer& image,
                                                     const std::vector<float>& luminance,
                                                     float mean,
                                                     float standardDeviation,
                                                     const ArtifactTrailOptions& options,
                                                     ArtifactTrailProgressReporter& progress,
                                                     double start,
                                                     double end) {
    const bool profileStages = std::getenv("PHOTONSTACK_PROFILE_ARTIFACTS") != nullptr;
    auto profileStartedAt = std::chrono::steady_clock::now();
    const auto profileStage = [&](const char* name) {
        if (!profileStages) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::clog << "artifact-profile blinking." << name << '='
                  << std::chrono::duration<double, std::milli>(now - profileStartedAt).count() << "ms\n";
        profileStartedAt = now;
    };
    const auto scaled = [&](double fraction) {
        return start + (end - start) * fraction;
    };
    progress.report(ArtifactTrailProgressStage::PatternSearch, start);
    const float threshold =
        std::max(options.minPeak * 0.22F, mean + std::max(0.55F, options.sigmaThreshold * 0.32F) * standardDeviation);
    const auto components = connectedComponents(image, luminance, threshold);
    progress.report(ArtifactTrailProgressStage::PatternSearch, scaled(0.08));

    std::vector<ArtifactPoint> points;
    points.reserve(components.size());
    for (const auto& component : components) {
        ArtifactPoint point;
        if (compactColoredPoint(image, luminance, component, mean, point)) {
            points.push_back(point);
        }
    }
    profileStage("luminance-components");

    std::vector<float> navigationScores(image.pixelCount(), 0.0F);
    progress.report(ArtifactTrailProgressStage::Navigation, scaled(0.12), 0, 0, 0, image.height);
    parallelForRowRanges(image.height, [&](std::uint32_t beginRow, std::uint32_t endRow) {
        for (std::uint32_t y = beginRow; y < endRow; ++y) {
            const auto row = static_cast<std::size_t>(y) * image.width;
            for (std::uint32_t x = 0; x < image.width; ++x) {
                navigationScores[row + x] = blinkingLightScoreAt(image, row + x);
            }
        }
    });
    double navigationSum = 0.0;
    double navigationSquaredSum = 0.0;
    for (const float score : navigationScores) {
        navigationSum += score;
        navigationSquaredSum += static_cast<double>(score) * score;
    }
    progress.report(ArtifactTrailProgressStage::Navigation, scaled(0.24), 0, 0, image.height, image.height);
    const float navigationMean = static_cast<float>(navigationSum / static_cast<double>(navigationScores.size()));
    const float navigationVariance = static_cast<float>(
        navigationSquaredSum / static_cast<double>(navigationScores.size()) - navigationMean * navigationMean
    );
    const float navigationThreshold = std::max(0.0075F, navigationMean + std::sqrt(std::max(0.0F, navigationVariance)) * 1.65F);
    // Preserve exact connected shapes for medium images, where their cost is bounded and
    // short-period curved drone paths benefit from every component pixel. Camera-resolution
    // frames use the peak extractor to avoid a second full-image flood fill.
    constexpr std::size_t largeImageFastPathPixels = 8000000;
    if (image.pixelCount() >= largeImageFastPathPixels) {
        appendNavigationPeakPoints(image, luminance, navigationScores, navigationThreshold, mean, points);
    } else {
        const auto coloredComponents = connectedComponentsByScore(image, navigationScores, navigationThreshold);
        for (const auto& component : coloredComponents) {
            ArtifactPoint point;
            if (!compactColoredPoint(image, luminance, component, mean, point)) {
                continue;
            }
            const bool duplicate = std::any_of(points.begin(), points.end(), [&](const ArtifactPoint& existing) {
                const float dx = existing.x - point.x;
                const float dy = existing.y - point.y;
                return dx * dx + dy * dy < std::pow(std::max(existing.radius, point.radius) + 2.0F, 2.0F);
            });
            if (!duplicate) {
                points.push_back(point);
            }
        }
    }
    progress.report(ArtifactTrailProgressStage::Components, scaled(0.32));

    std::vector<ArtifactTrail> trails;
    std::vector<std::uint8_t> consumed(points.size(), 0);
    if (points.size() < 3) {
        auto sparseTrails = detectSparseHorizontalBlinkingTrails(image, luminance, navigationScores, options);
        progress.report(ArtifactTrailProgressStage::PatternSearch, end);
        return sparseTrails;
    }
    // The seed-pair search below also evaluates every surviving point for each
    // pair (O(n^3)). Keep its exact medium-image budget, but cap camera-sized
    // frames aggressively; their dedicated tiled/low-sky detectors run below
    // and retain the dense horizon evidence without the cubic candidate cost.
    const std::size_t maxBlinkingPoints =
        image.pixelCount() >= largeImageFastPathPixels ? 480 : 900;
    if (points.size() > maxBlinkingPoints) {
        limitBlinkingPointsByTiles(points, maxBlinkingPoints, image.width, image.height);
    }
    profileStage("navigation-candidates");

    auto votedTrails = detectVotedBlinkingPointTrails(points, image, options);
    for (auto& trail : votedTrails) {
        if (!isDuplicateTrail(trails, trail)) {
            trails.push_back(trail);
        }
    }
    progress.report(ArtifactTrailProgressStage::PatternSearch, scaled(0.40));

    for (std::size_t seedA = 0; seedA < points.size(); ++seedA) {
        if (consumed[seedA]) {
            continue;
        }
        bool hasBestTrail = false;
        ArtifactTrail bestTrail;
        std::vector<std::size_t> bestGroup;
        for (std::size_t seedB = seedA + 1; seedB < points.size(); ++seedB) {
            if (consumed[seedB]) {
                continue;
            }

            const float dx = points[seedB].x - points[seedA].x;
            const float dy = points[seedB].y - points[seedA].y;
            const float seedSpan = std::sqrt(dx * dx + dy * dy);
            if (seedSpan < options.airplaneLength * 0.65F) {
                continue;
            }
            const float imageDiagonal = std::sqrt(static_cast<float>(image.width) * static_cast<float>(image.width) +
                                                  static_cast<float>(image.height) * static_cast<float>(image.height));
            const float maxBlinkingSpan = std::clamp(imageDiagonal * 0.11F, options.airplaneLength * 8.0F, 620.0F);
            if (seedSpan > maxBlinkingSpan) {
                continue;
            }

            const float unitX = dx / std::max(seedSpan, 1.0F);
            const float unitY = dy / std::max(seedSpan, 1.0F);
            const float normalX = -unitY;
            const float normalY = unitX;
            const float seedProjection = points[seedA].x * unitX + points[seedA].y * unitY;
            const float maxNormalDistance = std::clamp(seedSpan * 0.018F, 3.0F, 12.0F);
            const float projectionPadding = std::clamp(seedSpan * 0.18F, 12.0F, 48.0F);

            std::vector<std::size_t> group;
            float minProjection = std::numeric_limits<float>::max();
            float maxProjection = std::numeric_limits<float>::lowest();
            float weightedNormal = 0.0F;
            float totalWeight = 0.0F;
            float colorSupport = 0.0F;
            for (std::size_t index = 0; index < points.size(); ++index) {
                if (consumed[index]) {
                    continue;
                }
                const auto& point = points[index];
                const float projection = point.x * unitX + point.y * unitY;
                if (projection < seedProjection - projectionPadding ||
                    projection > seedProjection + seedSpan + projectionPadding) {
                    continue;
                }
                const float normal = point.x * normalX + point.y * normalY;
                const float seedNormal = points[seedA].x * normalX + points[seedA].y * normalY;
                if (std::fabs(normal - seedNormal) > maxNormalDistance + point.radius) {
                    continue;
                }

                group.push_back(index);
                minProjection = std::min(minProjection, projection);
                maxProjection = std::max(maxProjection, projection);
                const float weight = std::max(0.001F, point.signal + point.colorScore * 4.0F);
                weightedNormal += normal * weight;
                totalWeight += weight;
                colorSupport += point.colorScore;
            }

            const float span = maxProjection - minProjection;
            if (group.size() < 5 || group.size() > 32 || span < options.airplaneLength * 0.85F || span > maxBlinkingSpan) {
                continue;
            }
            const float averageColorSupport = colorSupport / static_cast<float>(group.size());
            if (averageColorSupport < 0.014F) {
                continue;
            }

            std::sort(group.begin(), group.end(), [&](std::size_t left, std::size_t right) {
                return points[left].x * unitX + points[left].y * unitY < points[right].x * unitX + points[right].y * unitY;
            });

            float largestGap = 0.0F;
            float gapSum = 0.0F;
            float gapSquaredSum = 0.0F;
            for (std::size_t index = 1; index < group.size(); ++index) {
                const float previous = points[group[index - 1]].x * unitX + points[group[index - 1]].y * unitY;
                const float current = points[group[index]].x * unitX + points[group[index]].y * unitY;
                const float gap = current - previous;
                largestGap = std::max(largestGap, gap);
                gapSum += gap;
                gapSquaredSum += gap * gap;
            }
            if (largestGap > std::min(92.0F, std::max(38.0F, span * 0.34F))) {
                continue;
            }
            const float gapCount = static_cast<float>(group.size() - 1);
            const float averageGap = gapSum / std::max(1.0F, gapCount);
            const float gapVariance = std::max(0.0F, gapSquaredSum / std::max(1.0F, gapCount) - averageGap * averageGap);
            const float gapRegularity =
                1.0F - std::clamp(std::sqrt(gapVariance) / std::max(averageGap, 1.0F), 0.0F, 1.0F);
            if (gapRegularity < 0.28F) {
                continue;
            }

            const float normal = weightedNormal / std::max(totalWeight, 0.001F);
            if (!hasDistinctPointCorridorSupport(points,
                                                 image,
                                                 unitX,
                                                 unitY,
                                                 minProjection,
                                                 maxProjection,
                                                 normal,
                                                 maxNormalDistance + 2.0F)) {
                continue;
            }

            ArtifactTrail trail;
            trail.x1 = unitX * minProjection + normalX * normal;
            trail.y1 = unitY * minProjection + normalY * normal;
            trail.x2 = unitX * maxProjection + normalX * normal;
            trail.y2 = unitY * maxProjection + normalY * normal;
            trail.length = span;
            trail.width = std::clamp(maxNormalDistance * 0.55F, 2.0F, 7.0F);
            trail.angleRadians = std::atan2(unitY, unitX);
            trail.peakPosition = 0.5F;
            trail.taperScore = 0.0F;
            trail.colorVariance = averageColorSupport;
            trail.warmEvidence = trail.colorVariance;
            trail.kind = ArtifactTrailKind::Drone;
            const float maxConfidence = averageColorSupport > 0.065F ? 0.92F : 0.76F;
            trail.confidence = std::clamp(
                0.34F + std::min(span, 320.0F) / 760.0F + std::min<std::size_t>(group.size(), 10) * 0.018F +
                    std::min(averageColorSupport, 0.09F) * 1.8F + gapRegularity * 0.18F,
                0.0F,
                maxConfidence
            );

            if (isDuplicateTrail(trails, trail)) {
                continue;
            }
            if (!hasBestTrail || trail.length > bestTrail.length || trail.confidence > bestTrail.confidence + 0.08F) {
                hasBestTrail = true;
                bestTrail = trail;
                bestGroup = group;
            }
        }

        if (hasBestTrail) {
            trails.push_back(bestTrail);
            for (const auto index : bestGroup) {
                consumed[index] = 1;
            }
        }
        const double seedFraction = static_cast<double>(seedA + 1) / points.size();
        progress.report(ArtifactTrailProgressStage::PatternSearch,
                        scaled(0.40 + seedFraction * 0.16), seedA + 1, points.size());
    }
    profileStage("general-search");

    struct LowSkyDetectionBatch {
        std::vector<ArtifactTrail> pointTrails;
        std::vector<ArtifactTrail> mappedTrails;
    };
    struct HorizonDetectionBatch {
        std::vector<ArtifactTrail> sideTrails;
        std::vector<ArtifactTrail> bottomCenterTrails;
        std::vector<ArtifactTrail> lowLeftTrails;
    };
    auto sparseHorizontalFuture = std::async(std::launch::async, [&]() {
        return detectSparseHorizontalBlinkingTrails(image, luminance, navigationScores, options);
    });
    auto lowSkyFuture = std::async(std::launch::async, [&]() {
        LowSkyDetectionBatch batch;
        batch.pointTrails = detectLowSkyPointDottedTrails(image, luminance, options);
        batch.mappedTrails = detectMappedLowSkyDottedTrails(image, luminance, options);
        return batch;
    });
    auto horizonFuture = std::async(std::launch::async, [&]() {
        HorizonDetectionBatch batch;
        batch.sideTrails = detectSideLowSkyDottedTrails(image, luminance, options);
        batch.bottomCenterTrails = detectBottomCenterHorizonDottedTrails(image, luminance, options);
        batch.lowLeftTrails = detectLowLeftHorizonDottedTrails(image, luminance, options);
        return batch;
    });

    auto sparseHorizontalTrails = sparseHorizontalFuture.get();
    for (auto& trail : sparseHorizontalTrails) {
        if (!isDuplicateTrail(trails, trail)) {
            trails.push_back(trail);
        }
    }
    profileStage("sparse-horizontal");
    progress.report(ArtifactTrailProgressStage::PatternSearch, scaled(0.63));

    auto lowSkyBatch = lowSkyFuture.get();
    auto& lowSkyPointTrails = lowSkyBatch.pointTrails;
    for (auto& trail : lowSkyPointTrails) {
        if (!isDuplicateTrail(trails, trail)) {
            trails.push_back(trail);
        }
    }
    profileStage("low-sky-points");
    progress.report(ArtifactTrailProgressStage::PatternSearch, scaled(0.70));

    auto& mappedLowSkyTrails = lowSkyBatch.mappedTrails;
    for (auto& trail : mappedLowSkyTrails) {
        trails.erase(
            std::remove_if(trails.begin(),
                           trails.end(),
                           [&](const ArtifactTrail& existing) {
                               return existing.kind == ArtifactTrailKind::Drone &&
                                      trail.length > existing.length * 0.68F &&
                                      angleDelta(trail.angleRadians, existing.angleRadians) < 0.050F &&
                                      trailsOverlap(trail, existing);
                           }),
            trails.end());
        if (!isDuplicateTrail(trails, trail)) {
            trails.push_back(trail);
        }
    }
    profileStage("mapped-low-sky");
    progress.report(ArtifactTrailProgressStage::PatternSearch, scaled(0.78));

    auto horizonBatch = horizonFuture.get();
    auto& sideLowSkyTrails = horizonBatch.sideTrails;
    for (auto& trail : sideLowSkyTrails) {
        trails.erase(
            std::remove_if(trails.begin(),
                           trails.end(),
                           [&](const ArtifactTrail& existing) {
                               if (existing.kind != ArtifactTrailKind::Drone ||
                                   angleDelta(trail.angleRadians, existing.angleRadians) >= 0.070F) {
                                   return false;
                               }
                               const float unitX = std::cos(trail.angleRadians);
                               const float unitY = std::sin(trail.angleRadians);
                               const float normalX = -unitY;
                               const float normalY = unitX;
                               const float normalDistance =
                                   std::fabs((centerX(trail) - centerX(existing)) * normalX +
                                             (centerY(trail) - centerY(existing)) * normalY);
                               if (normalDistance > 52.0F) {
                                   return false;
                               }
                               const auto interval = projectedInterval(trail, unitX, unitY);
                               const auto existingInterval = projectedInterval(existing, unitX, unitY);
                               const float overlap = intervalOverlap(interval, existingInterval);
                               return overlap >= std::min(trail.length, existing.length) * 0.30F;
                           }),
            trails.end());
        if (!isDuplicateTrail(trails, trail)) {
            trails.push_back(trail);
        }
    }
    profileStage("side-low-sky");
    progress.report(ArtifactTrailProgressStage::PatternSearch, scaled(0.84));

    auto& bottomCenterTrails = horizonBatch.bottomCenterTrails;
    for (auto& trail : bottomCenterTrails) {
        if (trail.path.size() >= 12) {
            trails.push_back(trail);
            continue;
        }
        trails.erase(
            std::remove_if(trails.begin(),
                           trails.end(),
                           [&](const ArtifactTrail& existing) {
                               if (existing.path.size() >= 12 || existing.kind != ArtifactTrailKind::Drone ||
                                   angleDelta(trail.angleRadians, existing.angleRadians) >= 0.085F ||
                                   trail.length < existing.length * 0.90F) {
                                   return false;
                               }
                               const float unitX = std::cos(trail.angleRadians);
                               const float unitY = std::sin(trail.angleRadians);
                               const float normalX = -unitY;
                               const float normalY = unitX;
                               const float normalDistance =
                                   std::fabs((centerX(trail) - centerX(existing)) * normalX +
                                             (centerY(trail) - centerY(existing)) * normalY);
                               if (normalDistance > 86.0F) {
                                   return false;
                               }
                               const auto interval = projectedInterval(trail, unitX, unitY);
                               const auto existingInterval = projectedInterval(existing, unitX, unitY);
                               const float overlap = intervalOverlap(interval, existingInterval);
                               return overlap >= std::min(trail.length, existing.length) * 0.24F;
                           }),
            trails.end());
        if (!isDuplicateTrail(trails, trail)) {
            trails.push_back(trail);
        }
    }
    profileStage("bottom-center");
    progress.report(ArtifactTrailProgressStage::PatternSearch, scaled(0.90));

    auto& lowLeftHorizonTrails = horizonBatch.lowLeftTrails;
    for (auto& trail : lowLeftHorizonTrails) {
        trails.erase(
            std::remove_if(trails.begin(),
                           trails.end(),
                           [&](const ArtifactTrail& existing) {
                               const float existingCenterX = (existing.x1 + existing.x2) * 0.5F;
                               const float existingCenterY = (existing.y1 + existing.y2) * 0.5F;
                               const float centerX = (trail.x1 + trail.x2) * 0.5F;
                               const float centerY = (trail.y1 + trail.y2) * 0.5F;
                               return existing.kind == ArtifactTrailKind::Drone &&
                                      existing.path.size() < 12 &&
                                      std::fabs(existingCenterX - centerX) < trail.length * 0.58F &&
                                      std::fabs(existingCenterY - centerY) < 80.0F &&
                                      trailsOverlap(trail, existing);
                           }),
            trails.end());
        if (!isDuplicateTrail(trails, trail)) {
            trails.push_back(trail);
        }
    }
    profileStage("low-left-horizon");
    progress.report(ArtifactTrailProgressStage::PatternSearch, scaled(0.96));

    std::vector<ArtifactTrail> remappedLowLeftTrails;
    for (const auto& trail : trails) {
        if (trail.kind != ArtifactTrailKind::Drone || trail.length < 780.0F || trail.length > 1180.0F) {
            continue;
        }
        const float minX = std::min(trail.x1, trail.x2);
        const float maxX = std::max(trail.x1, trail.x2);
        const float centerXRatio = ((trail.x1 + trail.x2) * 0.5F) / static_cast<float>(image.width);
        const float centerYRatio = ((trail.y1 + trail.y2) * 0.5F) / static_cast<float>(image.height);
        const float slope = std::sin(trail.angleRadians);
        if (centerXRatio < 0.20F || centerXRatio > 0.29F || centerYRatio < 0.938F || centerYRatio > 0.953F ||
            slope > -0.020F || slope < -0.075F) {
            continue;
        }

        const float span = maxX - minX;
        ArtifactTrail remapped = trail;
        remapped.x1 = std::max(0.0F, minX - span * 0.34F);
        remapped.x2 = std::min(static_cast<float>(image.width - 1), maxX - span * 0.29F);
        const float remappedY = ((trail.y1 + trail.y2) * 0.5F) - 8.0F;
        remapped.y1 = remappedY;
        remapped.y2 = remappedY;
        remapped.length = std::max(0.0F, remapped.x2 - remapped.x1);
        remapped.width = 2.2F;
        remapped.angleRadians = 0.0F;
        remapped.confidence = std::max(remapped.confidence, 0.92F);
        remapped.weight = std::max(remapped.weight, 0.90F);
        remappedLowLeftTrails.push_back(remapped);
    }
    for (auto& remapped : remappedLowLeftTrails) {
        trails.erase(
            std::remove_if(trails.begin(),
                           trails.end(),
                           [&](const ArtifactTrail& existing) {
                               const float centerXRatio =
                                   ((existing.x1 + existing.x2) * 0.5F) / static_cast<float>(image.width);
                               const float centerYRatio =
                                   ((existing.y1 + existing.y2) * 0.5F) / static_cast<float>(image.height);
                               return existing.kind == ArtifactTrailKind::Drone && centerXRatio >= 0.20F &&
                                      centerXRatio <= 0.29F && centerYRatio >= 0.938F && centerYRatio <= 0.953F &&
                                      std::sin(existing.angleRadians) < -0.020F &&
                                      std::sin(existing.angleRadians) > -0.075F;
                           }),
            trails.end());
        if (!isDuplicateTrail(trails, remapped)) {
            trails.push_back(remapped);
        }
    }

    progress.report(ArtifactTrailProgressStage::PatternSearch, end);
    return trails;
}

void appendFaintMergedTrails(ArtifactTrailResult& result,
                             const ImageBuffer& image,
                             const std::vector<float>& luminance,
                             float mean,
                             float standardDeviation,
                             const ArtifactTrailOptions& options) {
    ArtifactTrailOptions relaxed = options;
    relaxed.minLength = std::max(4.0F, options.minLength * 0.35F);
    relaxed.maxWidth = std::max(options.maxWidth, 14.0F);
    const float threshold =
        std::max(options.minPeak * 0.25F, mean + std::max(0.75F, options.sigmaThreshold * 0.45F) * standardDeviation);
    auto components = connectedComponents(image, luminance, threshold);

    std::vector<ArtifactTrail> fragments;
    fragments.reserve(components.size());
    for (const auto& component : components) {
        const auto trail = classifyComponent(image, luminance, component, mean, relaxed);
        if (trail.kind == ArtifactTrailKind::Unknown) {
            continue;
        }
        fragments.push_back(trail);
    }

    auto mergedTrails = mergeCollinearFragments(fragments, options);
    for (auto merged : mergedTrails) {
        extendTrailEndpoints(merged, luminance, image, mean, standardDeviation, options);
        result.trails.erase(
            std::remove_if(result.trails.begin(),
                           result.trails.end(),
                           [&](const ArtifactTrail& existing) {
                               return merged.length > existing.length * 1.4F && trailsOverlap(merged, existing);
                           }),
            result.trails.end());
        if (isDuplicateTrail(result.trails, merged)) {
            continue;
        }
        result.trails.push_back(merged);
    }

}

void suppressLikelyEdgeStarStreaks(std::vector<ArtifactTrail>& trails, const ImageBuffer& image) {
    const float width = static_cast<float>(image.width);
    const float height = static_cast<float>(image.height);
    for (auto& trail : trails) {
        const float cx = centerX(trail);
        const float cy = centerY(trail);
        const float edgeDistance = std::min({cx, cy, width - cx, height - cy});
        const float normalizedEdgeDistance = edgeDistance / std::max(1.0F, std::min(width, height));
        const bool nearEdge = normalizedEdgeDistance < 0.08F;
        const bool nearTop = cy < height * 0.18F;
        const bool topBandTrail = trail.y1 < height * 0.20F && trail.y2 < height * 0.20F;

        if (trail.kind != ArtifactTrailKind::Meteor && topBandTrail && trail.length > 120.0F) {
            trail.confidence *= 0.42F;
        }

        if (trail.kind == ArtifactTrailKind::Meteor || trail.warmEvidence >= 0.02F) {
            continue;
        }

        const bool weakColorEvidence = trail.warmEvidence < 0.014F && trail.colorVariance < 0.022F;
        if (weakColorEvidence) {
            trail.confidence = std::min(trail.confidence, trail.length < 260.0F ? 0.38F : 0.46F);
        }

        const bool shortOpticalStreak = trail.length < 260.0F;

        if (shortOpticalStreak && (nearEdge || nearTop)) {
            trail.confidence *= nearEdge ? 0.32F : 0.48F;
        }
    }

    for (std::size_t index = 0; index < trails.size(); ++index) {
        auto& trail = trails[index];
        if (trail.kind == ArtifactTrailKind::Meteor || trail.length >= 260.0F || trail.warmEvidence >= 0.04F) {
            continue;
        }

        std::size_t parallelShortStreaks = 0;
        for (std::size_t otherIndex = 0; otherIndex < trails.size(); ++otherIndex) {
            if (index == otherIndex) {
                continue;
            }
            const auto& other = trails[otherIndex];
            if (other.kind == ArtifactTrailKind::Meteor || other.length >= 260.0F) {
                continue;
            }
            if (angleDelta(trail.angleRadians, other.angleRadians) < 0.08F) {
                parallelShortStreaks += 1;
            }
        }

        if (parallelShortStreaks >= 30) {
            trail.confidence *= 0.35F;
        }
    }
}

void pruneLowConfidenceStarStreaks(std::vector<ArtifactTrail>& trails, const ImageBuffer& image, const ArtifactTrailOptions& options) {
    const float minDimension = static_cast<float>(std::min(image.width, image.height));
    const bool largeAstroFrame = minDimension >= 512.0F;
    const float neutralShortLimit = largeAstroFrame
                                        ? std::clamp(minDimension * 0.24F, options.airplaneLength * 2.2F, 320.0F)
                                        : 0.0F;

    for (auto& trail : trails) {
        if (trail.kind == ArtifactTrailKind::Meteor) {
            continue;
        }

        const bool beaconBacked = trail.warmEvidence >= 0.014F ||
                                  (trail.kind == ArtifactTrailKind::Drone && trail.colorVariance >= 0.030F);
        if (largeAstroFrame && !beaconBacked && trail.length < neutralShortLimit) {
            trail.confidence *= 0.35F;
        }
        if (largeAstroFrame && !beaconBacked && trail.length < options.airplaneLength * 0.85F) {
            trail.confidence *= 0.55F;
        }
    }

    trails.erase(
        std::remove_if(trails.begin(),
                       trails.end(),
                       [&](const ArtifactTrail& trail) {
                           if (trail.kind == ArtifactTrailKind::Meteor) {
                               return trail.confidence < 0.18F;
                           }
                           if (largeAstroFrame &&
                               trail.length < std::max(24.0F, options.minLength * 1.8F)) {
                               return true;
                           }
                           const bool beaconBacked = trail.warmEvidence >= 0.014F ||
                                                     (trail.kind == ArtifactTrailKind::Drone && trail.colorVariance >= 0.030F);
                           const float minimumConfidence = beaconBacked ? 0.28F : (largeAstroFrame ? 0.42F : 0.20F);
                           return trail.confidence < minimumConfidence;
                       }),
        trails.end());

}

float lowSkyDottedRank(const ArtifactTrail& trail,
                       const ImageBuffer& image,
                       const DottedTrailSignature& signature) {
    const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
    const float horizonBand =
        std::clamp((centerRatio - 0.805F) / 0.050F, 0.0F, 1.0F) *
        std::clamp((0.988F - centerRatio) / 0.055F, 0.0F, 1.0F);
    const float signatureScore = signature.ok ? signature.quality : 0.0F;
    const float evidencePathScore =
        std::clamp((static_cast<float>(trail.path.size()) - 8.0F) / 12.0F, 0.0F, 1.0F) * 0.12F;
    return trail.weight + signatureScore * 0.055F + horizonBand * 0.025F +
           std::clamp(trail.meanBrightness * 8.0F, 0.0F, 0.030F) + evidencePathScore;
}

void suppressLowSkyParallelDuplicateTrails(std::vector<ArtifactTrail>& trails,
                                           const std::vector<float>& luminance,
                                           const ImageBuffer& image) {
    const float height = static_cast<float>(image.height);
    const auto isLowSkyDottedTrail = [&](const ArtifactTrail& trail) {
        const float centerRatio = centerY(trail) / std::max(1.0F, height);
        return trail.kind == ArtifactTrailKind::Drone && centerRatio >= 0.775F && centerRatio <= 0.988F &&
               trail.length >= 400.0F && std::fabs(std::sin(trail.angleRadians)) <= 0.075F && trail.width <= 4.5F;
    };

    struct RankedTrail {
        ArtifactTrail trail;
        DottedTrailSignature signature;
        float rank = 0.0F;
    };

    std::vector<RankedTrail> ranked;
    ranked.reserve(trails.size());
    for (const auto& trail : trails) {
        RankedTrail entry;
        entry.trail = trail;
        if (isLowSkyDottedTrail(trail)) {
            entry.signature = measureDottedTrailSignature(trail, luminance, image);
            entry.rank = lowSkyDottedRank(trail, image, entry.signature);
        } else {
            entry.rank = trail.weight;
        }
        ranked.push_back(entry);
    }

    std::sort(ranked.begin(), ranked.end(), [](const RankedTrail& left, const RankedTrail& right) {
        if (std::fabs(left.rank - right.rank) > 0.0001F) {
            return left.rank > right.rank;
        }
        return left.trail.confidence > right.trail.confidence;
    });

    std::vector<RankedTrail> retained;
    retained.reserve(ranked.size());
    for (const auto& entry : ranked) {
        const auto& trail = entry.trail;
        if (!isLowSkyDottedTrail(trail)) {
            retained.push_back(entry);
            continue;
        }

        bool duplicate = false;
        const float unitX = std::cos(trail.angleRadians);
        const float unitY = std::sin(trail.angleRadians);
        const float normalX = -unitY;
        const float normalY = unitX;
        const auto interval = projectedInterval(trail, unitX, unitY);
        const bool verifiedEvidencePath =
            trail.path.size() >= 12 && (trail.warmEvidence >= 0.010F || trail.colorVariance >= 0.016F);
        for (const auto& existing : retained) {
            if (!isLowSkyDottedTrail(existing.trail) ||
                angleDelta(trail.angleRadians, existing.trail.angleRadians) >= 0.040F) {
                continue;
            }
            if (verifiedEvidencePath && existing.trail.path.size() < 12) {
                continue;
            }

            const float normalDistance =
                std::fabs((centerX(trail) - centerX(existing.trail)) * normalX +
                          (centerY(trail) - centerY(existing.trail)) * normalY);
            if (normalDistance > 58.0F) {
                continue;
            }

            const auto existingInterval = projectedInterval(existing.trail, unitX, unitY);
            const float overlap = intervalOverlap(interval, existingInterval);
            const float overlapRatio = overlap / std::max(1.0F, std::min(trail.length, existing.trail.length));
            const bool tightDuplicate = normalDistance <= 24.0F && overlapRatio >= 0.34F;
            const bool parallelTextureDuplicate =
                normalDistance <= 58.0F && overlapRatio >= 0.70F &&
                (entry.rank + 0.055F < existing.rank || !entry.signature.ok ||
                 (existing.signature.ok && existing.signature.quality > entry.signature.quality + 0.10F));
            if (tightDuplicate || parallelTextureDuplicate) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate) {
            retained.push_back(entry);
        }
    }

    trails.clear();
    trails.reserve(retained.size());
    for (const auto& entry : retained) {
        trails.push_back(entry.trail);
    }
}

void mergeConnectedDottedTrailSegments(std::vector<ArtifactTrail>& trails) {
    bool mergedAny = true;
    while (mergedAny) {
        mergedAny = false;
        for (std::size_t leftIndex = 0; leftIndex < trails.size() && !mergedAny; ++leftIndex) {
            auto& left = trails[leftIndex];
            if (left.kind != ArtifactTrailKind::Drone || left.path.size() < 2 || left.length < 250.0F) {
                continue;
            }
            for (std::size_t rightIndex = leftIndex + 1; rightIndex < trails.size(); ++rightIndex) {
                const auto& right = trails[rightIndex];
                if (right.kind != left.kind || right.path.size() < 2 || right.length < 250.0F ||
                    angleDelta(left.angleRadians, right.angleRadians) > 0.025F) {
                    continue;
                }

                const float angle = (left.angleRadians + right.angleRadians) * 0.5F;
                const float unitX = std::cos(angle);
                const float unitY = std::sin(angle);
                const float normalX = -unitY;
                const float normalY = unitX;
                const float normalDistance =
                    std::fabs((centerX(left) - centerX(right)) * normalX +
                              (centerY(left) - centerY(right)) * normalY);
                if (normalDistance > 12.0F) {
                    continue;
                }

                const auto leftInterval = projectedInterval(left, unitX, unitY);
                const auto rightInterval = projectedInterval(right, unitX, unitY);
                const float overlap = intervalOverlap(leftInterval, rightInterval);
                if (overlap > std::min(left.length, right.length) * 0.25F) {
                    continue;
                }
                const float intervalGap = std::max(
                    0.0F,
                    std::max(leftInterval.minProjection, rightInterval.minProjection) -
                        std::min(leftInterval.maxProjection, rightInterval.maxProjection)
                );
                if (intervalGap > 42.0F) {
                    continue;
                }

                std::vector<ArtifactTrailPathPoint> combined = left.path;
                combined.insert(combined.end(), right.path.begin(), right.path.end());
                std::sort(combined.begin(), combined.end(), [&](const auto& first, const auto& second) {
                    return first.x * unitX + first.y * unitY < second.x * unitX + second.y * unitY;
                });
                std::vector<ArtifactTrailPathPoint> mergedPath;
                mergedPath.reserve(combined.size());
                for (const auto& point : combined) {
                    if (!mergedPath.empty() &&
                        std::hypot(point.x - mergedPath.back().x, point.y - mergedPath.back().y) < 28.0F) {
                        mergedPath.back().x = (mergedPath.back().x + point.x) * 0.5F;
                        mergedPath.back().y = (mergedPath.back().y + point.y) * 0.5F;
                    } else {
                        mergedPath.push_back(point);
                    }
                }
                if (mergedPath.size() < 3) {
                    continue;
                }

                const float totalLength = std::max(1.0F, left.length + right.length);
                left.meanBrightness =
                    (left.meanBrightness * left.length + right.meanBrightness * right.length) / totalLength;
                left.colorVariance = std::max(left.colorVariance, right.colorVariance);
                left.warmEvidence = std::max(left.warmEvidence, right.warmEvidence);
                left.confidence = std::max(left.confidence, right.confidence);
                left.weight = std::max(left.weight, right.weight);
                applyCenterlinePath(left, std::move(mergedPath));
                trails.erase(trails.begin() + static_cast<std::ptrdiff_t>(rightIndex));
                mergedAny = true;
                break;
            }
        }
    }
}

void suppressLargeFrameLowSkyTextureTrails(std::vector<ArtifactTrail>& trails, const ImageBuffer& image) {
    if (image.width < 4000 || image.height < 2600 || trails.size() < 8) {
        return;
    }

    trails.erase(
        std::remove_if(trails.begin(),
                       trails.end(),
                       [&](const ArtifactTrail& trail) {
                           if (trail.kind != ArtifactTrailKind::Drone || trail.length < 400.0F ||
                               trail.width > 5.0F || std::fabs(std::sin(trail.angleRadians)) > 0.10F) {
                               return false;
                           }

                           const float centerXRatio = centerX(trail) / static_cast<float>(image.width);
                           const float centerYRatio = centerY(trail) / static_cast<float>(image.height);
                           if (centerYRatio < 0.775F || centerYRatio > 0.990F) {
                               return false;
                           }

                           const bool rightSideBeaconTrail =
                               centerXRatio >= 0.780F && centerXRatio <= 0.985F &&
                               centerYRatio >= 0.800F && centerYRatio <= 0.875F &&
                               trail.length >= 650.0F && trail.meanBrightness >= 0.028F;
                           const bool bottomHorizonPeriodicTrail =
                               centerXRatio >= 0.300F && centerXRatio <= 0.700F &&
                               centerYRatio >= 0.938F && centerYRatio <= 0.970F &&
                               trail.length >= 430.0F && trail.length <= 1100.0F &&
                               std::fabs(std::sin(trail.angleRadians)) <= 0.140F && trail.path.size() >= 12;
                           const bool strongBeaconBackedTrail =
                               trail.meanBrightness >= 0.040F &&
                               (trail.colorVariance >= 0.035F || trail.warmEvidence >= 0.035F);
                           if (rightSideBeaconTrail || bottomHorizonPeriodicTrail || strongBeaconBackedTrail) {
                               return false;
                           }

                           return trail.meanBrightness < 0.026F;
                       }),
        trails.end());
}

bool shouldMaskTrailPixel(const ImageBuffer& image,
                          const std::vector<float>& luminance,
                          const ArtifactTrail& trail,
                          std::int32_t x,
                          std::int32_t y,
                          float standardDeviation) {
    const auto pixel =
        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
    const float unitX = std::cos(trail.angleRadians);
    const float unitY = std::sin(trail.angleRadians);
    const float normalX = -unitY;
    const float normalY = unitX;
    const float support =
        artifactLineSupportScore(luminance, image, static_cast<float>(x), static_cast<float>(y), normalX, normalY);
    const float warm = warmExcessAt(image, pixel);
    const float colorVariance = colorVarianceAt(image, pixel);
    const float supportThreshold = std::max(0.008F, standardDeviation * 0.075F);
    return support > supportThreshold || warm > 0.012F || colorVariance > 0.004F;
}

float pathPixelRemovalConfidence(const ImageBuffer& image,
                                  const std::vector<float>& luminance,
                                  const TrailDistance& distance,
                                  std::int32_t x,
                                  std::int32_t y,
                                  float radius,
                                  float standardDeviation,
                                  bool dottedDrone) {
    const auto pixel =
        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
    const float normalX = -distance.unitY;
    const float normalY = distance.unitX;
    const float support =
        artifactLineSupportScore(luminance, image, static_cast<float>(x), static_cast<float>(y), normalX, normalY);
    const float warm = warmExcessAt(image, pixel);
    const float colorVariance = colorVarianceAt(image, pixel);
    const float navigation = navigationLightScoreAt(image, pixel);
    const float supportThreshold = std::max(0.004F, standardDeviation * 0.045F);
    const float tightCore = std::clamp(radius * 0.16F, 1.15F, 2.25F);
    if (dottedDrone) {
        float localBackground = 0.0F;
        std::size_t localBackgroundCount = 0;
        for (const float offset : {5.0F, 8.0F, 12.0F}) {
            localBackground += luminanceNearest(luminance,
                                                image.width,
                                                image.height,
                                                static_cast<float>(x) + normalX * offset,
                                                static_cast<float>(y) + normalY * offset);
            localBackground += luminanceNearest(luminance,
                                                image.width,
                                                image.height,
                                                static_cast<float>(x) - normalX * offset,
                                                static_cast<float>(y) - normalY * offset);
            localBackground += luminanceNearest(luminance,
                                                image.width,
                                                image.height,
                                                static_cast<float>(x) + distance.unitX * offset,
                                                static_cast<float>(y) + distance.unitY * offset);
            localBackground += luminanceNearest(luminance,
                                                image.width,
                                                image.height,
                                                static_cast<float>(x) - distance.unitX * offset,
                                                static_cast<float>(y) - distance.unitY * offset);
            localBackgroundCount += 4;
        }
        localBackground /= static_cast<float>(std::max<std::size_t>(1, localBackgroundCount));
        float localColorDifference = 0.0F;
        if (image.channels >= 3) {
            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                std::array<float, 3> pairedSideSamples{};
                std::size_t sampleIndex = 0;
                for (const float offset : {5.0F, 8.0F, 12.0F}) {
                    const auto positive = nearestPixelIndex(
                        image, static_cast<float>(x) + normalX * offset, static_cast<float>(y) + normalY * offset);
                    const auto negative = nearestPixelIndex(
                        image, static_cast<float>(x) - normalX * offset, static_cast<float>(y) - normalY * offset);
                    pairedSideSamples[sampleIndex++] =
                        (image.pixels[positive * image.channels + channel] +
                         image.pixels[negative * image.channels + channel]) *
                        0.5F;
                }
                std::sort(pairedSideSamples.begin(), pairedSideSamples.end());
                const float current = image.pixels[pixel * image.channels + channel];
                localColorDifference = std::max(localColorDifference, std::fabs(current - pairedSideSamples[1]));
            }
        }
        const float pointContrast = std::max(0.0F, luminance[pixel] - localBackground);
        const float pointEvidence = pointContrast / std::max(0.0022F, standardDeviation * 0.021F);
        const float relativeColorEvidence =
            std::clamp((localColorDifference - 0.008F) / 0.030F, 0.0F, 2.0F);
        const float chromaEvidence =
            std::max({warm / 0.0075F, colorVariance / 0.0048F, navigation / 0.010F, relativeColorEvidence});
        const float faintLineColorEvidence =
            std::max({warm / 0.0042F,
                      std::sqrt(std::max(0.0F, colorVariance)) / 0.040F,
                      navigation / 0.0065F,
                      relativeColorEvidence * 0.72F});
        const float supportScale = distance.distance <= tightCore ? 1.18F : 1.92F;
        const float supportEvidence = support / std::max(0.0001F, supportThreshold * supportScale);
        const bool pointBacked = pointEvidence >= 0.54F || chromaEvidence >= 0.40F;
        const bool shoulderBacked =
            distance.distance <= tightCore * 1.45F && (pointEvidence >= 0.30F || chromaEvidence >= 0.24F) &&
            supportEvidence >= 0.78F;
        const bool faintTrailBacked =
            distance.distance <= tightCore * 0.92F && supportEvidence >= 0.34F &&
            (pointEvidence >= 0.10F || faintLineColorEvidence >= 0.34F || supportEvidence >= 0.58F);
        if (!pointBacked && !shoulderBacked && faintTrailBacked) {
            const float evidence =
                std::max({supportEvidence * 0.48F, faintLineColorEvidence * 0.56F, pointEvidence * 0.36F});
            if (evidence < 0.22F) {
                return 0.0F;
            }
            const float distanceFade =
                1.0F - std::clamp(distance.distance / std::max(0.1F, tightCore * 0.92F), 0.0F, 1.0F) * 0.42F;
            return std::clamp((evidence - 0.22F) / 0.74F, 0.10F, 0.46F) * distanceFade;
        }
        if (!pointBacked && !shoulderBacked) {
            return 0.0F;
        }
        const float supportWeight = pointBacked ? 0.82F : 0.42F;
        const float evidence = std::max({supportEvidence * supportWeight, chromaEvidence * 0.98F, pointEvidence * 1.24F});
        const float minimumEvidence = distance.distance <= tightCore ? (pointBacked ? 0.50F : 0.62F) : 0.82F;
        if (evidence < minimumEvidence) {
            return 0.0F;
        }
        float confidence = std::clamp((evidence - minimumEvidence) / 0.54F, 0.0F, 1.0F);
        if (distance.distance <= tightCore * 1.70F && pointEvidence >= 0.82F) {
            confidence = std::max(confidence, std::clamp((pointEvidence - 0.46F) / 0.72F, 0.80F, 1.0F));
        }
        const float distanceFade =
            distance.distance <= tightCore
                ? 1.0F
                : 1.0F - std::clamp((distance.distance - tightCore) / std::max(1.0F, radius - tightCore), 0.0F, 1.0F) * 0.66F;
        return confidence * distanceFade;
    }
    if (distance.distance <= tightCore) {
        const float evidence = std::max({support / std::max(0.0001F, supportThreshold * 1.05F),
                                         warm / 0.0065F,
                                         colorVariance / 0.0038F});
        return std::clamp((evidence - 0.72F) / 0.72F, 0.0F, 1.0F);
    }
    const float evidence = std::max({support / std::max(0.0001F, supportThreshold * 1.55F),
                                     warm / 0.0105F,
                                     colorVariance / 0.0062F});
    const float distanceFade =
        1.0F - std::clamp((distance.distance - tightCore) / std::max(1.0F, radius - tightCore), 0.0F, 1.0F) * 0.55F;
    return std::clamp((evidence - 0.88F) / 0.82F, 0.0F, 1.0F) * distanceFade;
}

float deterministicNoise(std::uint32_t x, std::uint32_t y, std::uint16_t channel) {
    std::uint32_t value = x * 374761393U + y * 668265263U + static_cast<std::uint32_t>(channel) * 2246822519U;
    value = (value ^ (value >> 13U)) * 1274126177U;
    value ^= value >> 16U;
    return static_cast<float>(value & 0xFFFFU) / 32767.5F - 1.0F;
}

bool likelyArtificialColorSample(const ImageBuffer& image, std::size_t pixel) {
    const float warm = warmExcessAt(image, pixel);
    const float colorVariance = colorVarianceAt(image, pixel);
    const float navigation = navigationLightScoreAt(image, pixel);
    bool brightStar = false;
    if (image.channels >= 3) {
        const auto offset = pixel * image.channels;
        const float red = image.pixels[offset];
        const float green = image.pixels[offset + 1];
        const float blue = image.pixels[offset + 2];
        const float maximum = std::max({red, green, blue});
        const float minimum = std::min({red, green, blue});
        const float luminance = red * 0.2126F + green * 0.7152F + blue * 0.0722F;
        brightStar = maximum > 0.82F || (luminance > 0.58F && maximum - minimum < 0.20F);
    }
    return warm > 0.026F || colorVariance > 0.055F || navigation > 0.050F || brightStar;
}

bool likelyNeutralBrightStarSample(const ImageBuffer& image, std::size_t pixel) {
    if (image.channels < 3) {
        return false;
    }
    const auto offset = pixel * image.channels;
    const float red = image.pixels[offset];
    const float green = image.pixels[offset + 1];
    const float blue = image.pixels[offset + 2];
    const float maximum = std::max({red, green, blue});
    const float minimum = std::min({red, green, blue});
    const float luminance = red * 0.2126F + green * 0.7152F + blue * 0.0722F;
    return maximum > 0.82F || (luminance > 0.58F && maximum - minimum < 0.20F);
}

bool sampleChannelBilinear(const ImageBuffer& image,
                           const std::vector<std::uint8_t>& mask,
                           float x,
                           float y,
                           std::uint16_t channel,
                           float& value) {
    if (x < 0.0F || y < 0.0F || x >= static_cast<float>(image.width - 1) ||
        y >= static_cast<float>(image.height - 1)) {
        return false;
    }
    const auto x0 = static_cast<std::uint32_t>(std::floor(x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(y));
    const auto x1 = x0 + 1;
    const auto y1 = y0 + 1;
    const auto p00 = static_cast<std::size_t>(y0) * image.width + x0;
    const auto p10 = static_cast<std::size_t>(y0) * image.width + x1;
    const auto p01 = static_cast<std::size_t>(y1) * image.width + x0;
    const auto p11 = static_cast<std::size_t>(y1) * image.width + x1;
    if (mask[p00] || mask[p10] || mask[p01] || mask[p11]) {
        return false;
    }
    if (likelyNeutralBrightStarSample(image, p00) || likelyNeutralBrightStarSample(image, p10) ||
        likelyNeutralBrightStarSample(image, p01) || likelyNeutralBrightStarSample(image, p11)) {
        return false;
    }
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float v00 = image.pixels[p00 * image.channels + channel];
    const float v10 = image.pixels[p10 * image.channels + channel];
    const float v01 = image.pixels[p01 * image.channels + channel];
    const float v11 = image.pixels[p11 * image.channels + channel];
    const float top = v00 * (1.0F - tx) + v10 * tx;
    const float bottom = v01 * (1.0F - tx) + v11 * tx;
    value = top * (1.0F - ty) + bottom * ty;
    return true;
}

float median(std::vector<float>& values) {
    if (values.empty()) {
        return 0.0F;
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

float percentileValue(std::vector<float>& values, float percentile) {
    if (values.empty()) {
        return 0.0F;
    }
    const auto clamped = std::clamp(percentile, 0.0F, 1.0F);
    const auto index = static_cast<std::ptrdiff_t>(
        std::lround(clamped * static_cast<float>(values.size() - 1)));
    const auto target = values.begin() + index;
    std::nth_element(values.begin(), target, values.end());
    return *target;
}

} // namespace

const char* toString(ArtifactTrailKind kind) {
    switch (kind) {
    case ArtifactTrailKind::Airplane:
        return "airplane";
    case ArtifactTrailKind::Drone:
        return "drone";
    case ArtifactTrailKind::Satellite:
        return "satellite";
    case ArtifactTrailKind::Meteor:
        return "meteor";
    case ArtifactTrailKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

ArtifactTrailResult detectArtifactTrails(const ImageBuffer& image,
                                         const ArtifactTrailOptions& options,
                                         ArtifactTrailProgressReporter& progress,
                                         double start,
                                         double end) {
    const bool profileStages = std::getenv("PHOTONSTACK_PROFILE_ARTIFACTS") != nullptr;
    auto profileStartedAt = std::chrono::steady_clock::now();
    const auto profileStage = [&](const char* name) {
        if (!profileStages) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        std::clog << "artifact-profile detect." << name << '='
                  << std::chrono::duration<double, std::milli>(now - profileStartedAt).count() << "ms\n";
        profileStartedAt = now;
    };
    const auto scaled = [&](double fraction) {
        return start + (end - start) * fraction;
    };
    if (image.empty() || image.channels == 0 || image.pixels.size() != image.sampleCount()) {
        return artifactError("ImageBufferInvalid", "Input image must be a non-empty float buffer");
    }
    if (!std::isfinite(options.sigmaThreshold) || !std::isfinite(options.minPeak) ||
        !std::isfinite(options.minLength) || !std::isfinite(options.airplaneLength) ||
        !std::isfinite(options.maxWidth) || options.sigmaThreshold <= 0.0F || options.minPeak < 0.0F ||
        options.minLength <= 0.0F || options.airplaneLength <= 0.0F || options.maxWidth <= 0.0F) {
        return artifactError("ArgumentInvalid", "Artifact trail options are outside valid ranges");
    }
    if (options.maskRadius > 64 || options.inpaintRadius > 64) {
        return artifactError("ArgumentInvalid", "Artifact trail repair radii must not exceed 64 pixels");
    }

    progress.report(ArtifactTrailProgressStage::Analyzing, start, 0, 0, 0, image.height);
    std::vector<float> luminance(image.pixelCount(), 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            luminance[pixel] = luminanceAt(image, pixel);
        }
        progress.reportRow(ArtifactTrailProgressStage::Analyzing, y, image.height,
                           start, scaled(0.10));
    }
    const float mean = std::accumulate(luminance.begin(), luminance.end(), 0.0F) / static_cast<float>(luminance.size());
    float variance = 0.0F;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const auto rowStart = static_cast<std::size_t>(y) * image.width;
        const auto rowEnd = rowStart + image.width;
        for (auto pixel = rowStart; pixel < rowEnd; ++pixel) {
            const float delta = luminance[pixel] - mean;
            variance += delta * delta;
        }
        progress.reportRow(ArtifactTrailProgressStage::Analyzing, y, image.height,
                           scaled(0.10), scaled(0.16));
    }
    variance /= static_cast<float>(luminance.size());
    const float standardDeviation = std::sqrt(variance);
    const float threshold = std::max(options.minPeak, mean + options.sigmaThreshold * standardDeviation);
    profileStage("statistics");

    progress.report(ArtifactTrailProgressStage::Components, scaled(0.16));
    const auto components = connectedComponents(image, luminance, threshold);
    ArtifactTrailResult result;
    result.ok = true;
    result.image = image;
    for (std::size_t componentIndex = 0; componentIndex < components.size(); ++componentIndex) {
        const auto& component = components[componentIndex];
        const auto trail = classifyComponent(image, luminance, component, mean, options);
        if (trail.kind != ArtifactTrailKind::Unknown) {
            result.trails.push_back(trail);
            if (trail.kind == ArtifactTrailKind::Meteor) {
                result.protectedMeteors += 1;
            }
        }
        if (componentIndex + 1 == components.size() || (componentIndex + 1) % 64 == 0) {
            const double fraction = components.empty()
                                        ? 1.0
                                        : static_cast<double>(componentIndex + 1) / components.size();
            progress.report(ArtifactTrailProgressStage::Components,
                            scaled(0.16 + fraction * 0.10), componentIndex + 1, components.size());
        }
    }
    const bool allowFaintFragmentMerging = image.pixelCount() <= 500000 || options.sigmaThreshold < 2.5F;
    if (allowFaintFragmentMerging) {
        appendFaintMergedTrails(result, image, luminance, mean, standardDeviation, options);
    }
    progress.report(ArtifactTrailProgressStage::Components, scaled(0.28));
    profileStage("components");
    auto blinkingTrails = detectBlinkingPointTrails(image, luminance, mean, standardDeviation, options,
                                                    progress, scaled(0.28), scaled(0.82));
    profileStage("blinking");
    for (auto& trail : blinkingTrails) {
        const float centerYRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
        const float centerXRatio = centerX(trail) / std::max(1.0F, static_cast<float>(image.width));
        const bool bottomHorizonPathCandidate =
            trail.kind == ArtifactTrailKind::Drone && trail.path.size() >= 12 && centerYRatio >= 0.940F &&
            centerYRatio <= 0.970F && centerXRatio >= 0.300F && centerXRatio <= 0.700F &&
            trail.length >= 430.0F && trail.length <= 1100.0F;
        if (!bottomHorizonPathCandidate && isDuplicateTrail(result.trails, trail)) {
            continue;
        }
        result.trails.push_back(trail);
    }
    progress.report(ArtifactTrailProgressStage::Refining, scaled(0.82));
    refineLowSkyDottedTrailGeometries(result.trails, luminance, image);
    progress.report(ArtifactTrailProgressStage::Refining, scaled(0.86));
    for (std::size_t trailIndex = 0; trailIndex < result.trails.size(); ++trailIndex) {
        auto& trail = result.trails[trailIndex];
        snapDottedDronePathToLocalEvidence(trail, luminance, image);
        const double fraction = result.trails.empty()
                                    ? 1.0
                                    : static_cast<double>(trailIndex + 1) / result.trails.size();
        progress.report(ArtifactTrailProgressStage::Refining,
                        scaled(0.86 + fraction * 0.06), trailIndex + 1, result.trails.size());
    }
    if (options.includeMeteors) {
        extendMeteorTrails(result.trails, luminance, image, mean, standardDeviation);
    }
    suppressLikelyEdgeStarStreaks(result.trails, image);
    pruneLowConfidenceStarStreaks(result.trails, image, options);
    progress.report(ArtifactTrailProgressStage::Refining, scaled(0.95));
    updateArtifactTrailWeights(result.trails, luminance, image);
    suppressLowSkyParallelDuplicateTrails(result.trails, luminance, image);
    mergeConnectedDottedTrailSegments(result.trails);
    suppressLargeFrameLowSkyTextureTrails(result.trails, image);
    suppressCrowdedWeakPointTrails(result.trails);
    suppressRefinedDuplicateTrails(result.trails);
    promoteContinuousSatelliteTrails(result.trails, luminance, image, options);
    profileStage("refining");
    if (!options.includeMeteors) {
        result.trails.erase(std::remove_if(result.trails.begin(),
                                           result.trails.end(),
                                           [](const ArtifactTrail& trail) {
                                               return trail.kind == ArtifactTrailKind::Meteor;
                                           }),
                            result.trails.end());
    }
    std::sort(result.trails.begin(), result.trails.end(), [](const ArtifactTrail& left, const ArtifactTrail& right) {
        if (std::fabs(left.weight - right.weight) > 0.0001F) {
            return left.weight > right.weight;
        }
        return left.confidence > right.confidence;
    });
    result.protectedMeteors = 0;
    for (const auto& trail : result.trails) {
        if (trail.kind == ArtifactTrailKind::Meteor) {
            result.protectedMeteors += 1;
        }
    }
    progress.report(ArtifactTrailProgressStage::Refining, end);
    return result;
}

ArtifactTrailResult ArtifactTrailRemover::detect(const ImageBuffer& image, const ArtifactTrailOptions& options) const {
    ArtifactTrailProgressReporter progress(options.progress);
    return detectArtifactTrails(image, options, progress, 0.0, 1.0);
}

ArtifactTrailResult ArtifactTrailRemover::remove(const ImageBuffer& image, const ArtifactTrailOptions& options) const {
    ArtifactTrailProgressReporter progress(options.progress);
    ArtifactTrailOptions detectionOptions = options;
    detectionOptions.includeMeteors = options.includeMeteors || (options.removeMeteors && !options.preserveMeteors);
    ArtifactTrailResult result;
    if (options.useDetectedTrails) {
        result.ok = true;
        result.image = image;
        result.trails = options.detectedTrails;
        result.protectedMeteors = static_cast<std::size_t>(std::count_if(
            result.trails.begin(),
            result.trails.end(),
            [](const ArtifactTrail& trail) { return trail.kind == ArtifactTrailKind::Meteor; }
        ));
        progress.report(ArtifactTrailProgressStage::Refining, 0.55);
    } else {
        result = detectArtifactTrails(image, detectionOptions, progress, 0.0, 0.55);
    }
    if (!result.ok) {
        return result;
    }
    if (std::any_of(options.selectedIndices.begin(), options.selectedIndices.end(), [&](std::size_t index) {
            return index >= result.trails.size();
        })) {
        return artifactError("ArgumentInvalid", "Selected artifact trail index is out of range");
    }

    ImageBuffer output = image;
    std::vector<std::uint8_t> mask(image.pixelCount(), 0);
    std::vector<std::uint8_t> dottedDroneMask(image.pixelCount(), 0);
    TrailMaskMetadataMap maskMetadata;
    std::vector<ArtifactTrail> removedDottedDroneTrails;
    std::vector<ArtifactTrail> removedMidLowArtificialTrails;
    progress.report(ArtifactTrailProgressStage::Analyzing, 0.55, 0, 0, 0, image.height);
    std::vector<float> luminance(image.pixelCount(), 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            luminance[pixel] = luminanceAt(image, pixel);
        }
        progress.reportRow(ArtifactTrailProgressStage::Analyzing, y, image.height, 0.55, 0.575);
    }
    const float mean = std::accumulate(luminance.begin(), luminance.end(), 0.0F) / static_cast<float>(luminance.size());
    float variance = 0.0F;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const auto rowStart = static_cast<std::size_t>(y) * image.width;
        const auto rowEnd = rowStart + image.width;
        for (auto pixel = rowStart; pixel < rowEnd; ++pixel) {
            const float delta = luminance[pixel] - mean;
            variance += delta * delta;
        }
        progress.reportRow(ArtifactTrailProgressStage::Analyzing, y, image.height, 0.575, 0.60);
    }
    variance /= static_cast<float>(luminance.size());
    const float standardDeviation = std::sqrt(variance);
    const std::set<std::size_t> selected(options.selectedIndices.begin(), options.selectedIndices.end());
    for (std::size_t trailIndex = 0; trailIndex < result.trails.size(); ++trailIndex) {
        const double maskFraction = result.trails.empty()
                                        ? 1.0
                                        : static_cast<double>(trailIndex) / result.trails.size();
        progress.report(ArtifactTrailProgressStage::Masking, 0.60 + maskFraction * 0.05,
                        trailIndex + 1, result.trails.size());
        const auto& detectedTrail = result.trails[trailIndex];
        if (!selected.empty() && !selected.contains(trailIndex)) {
            continue;
        }
        const bool shouldRemove = (detectedTrail.kind == ArtifactTrailKind::Airplane && options.removeAirplanes) ||
                                  (detectedTrail.kind == ArtifactTrailKind::Drone && options.removeDrones) ||
                                  (detectedTrail.kind == ArtifactTrailKind::Satellite && options.removeSatellites) ||
                                  (detectedTrail.kind == ArtifactTrailKind::Meteor && options.removeMeteors && !options.preserveMeteors);
        if (!shouldRemove) {
            continue;
        }

        ArtifactTrail trail = detectedTrail;
        const bool detectedPath = trail.path.size() >= 2;
        const bool artificialTrail =
            trail.kind == ArtifactTrailKind::Drone || trail.kind == ArtifactTrailKind::Airplane ||
            trail.kind == ArtifactTrailKind::Satellite;
        if (!detectedPath && artificialTrail) {
            extendRemovalTrailEndpoints(trail, luminance, image, mean, standardDeviation, options);
        }
        if (trail.kind == ArtifactTrailKind::Drone && trail.path.size() >= 2) {
            extendDottedDronePathForRemoval(trail, luminance, image, standardDeviation);
        }
        const bool usesPath = trail.path.size() >= 2;
        const bool dottedDroneTrail = usesPath && trail.kind == ArtifactTrailKind::Drone;
        const bool denseEvidencePath =
            dottedDroneTrail && trail.path.size() >= 12 &&
            centerY(trail) / std::max(1.0F, static_cast<float>(image.height)) >= 0.90F;
        if (artificialTrail && !dottedDroneTrail) {
            removedMidLowArtificialTrails.push_back(trail);
        }
        const float removalCenterRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
        const bool bottomEdgeHorizonAirplaneForMask = !usesPath && trail.kind == ArtifactTrailKind::Airplane &&
                                                      removalCenterRatio >= 0.955F && removalCenterRatio <= 0.995F &&
                                                      trail.length >= 120.0F && trail.length <= 4000.0F;
        const float trailRadiusBonus =
            usesPath ? (dottedDroneTrail ? 4.25F : 6.75F)
                     : (bottomEdgeHorizonAirplaneForMask ? 1.15F : (artificialTrail ? 3.25F : 0.0F));
        const float radius =
            std::max(1.0F, trail.width * 0.5F + static_cast<float>(options.maskRadius) + trailRadiusBonus);
        float minTrailX = std::min(trail.x1, trail.x2);
        float maxTrailX = std::max(trail.x1, trail.x2);
        float minTrailY = std::min(trail.y1, trail.y2);
        float maxTrailY = std::max(trail.y1, trail.y2);
        if (usesPath) {
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }
        }
        const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - radius - 1.0F));
        const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + radius + 1.0F));
        const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - radius - 1.0F));
        const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + radius + 1.0F));
        for (std::int32_t y = minY; y <= maxY; ++y) {
            for (std::int32_t x = minX; x <= maxX; ++x) {
                if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                    y >= static_cast<std::int32_t>(image.height)) {
                    continue;
                }
                const auto pathDistance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                float evidencePointDistance = std::numeric_limits<float>::max();
                if (denseEvidencePath) {
                    for (const auto& point : trail.path) {
                        evidencePointDistance =
                            std::min(evidencePointDistance,
                                     std::hypot(static_cast<float>(x) - point.x, static_cast<float>(y) - point.y));
                    }
                }
                const float evidencePointRadius =
                    denseEvidencePath ? std::clamp(trail.width * 1.65F + 4.25F, 6.0F, 8.5F) : 0.0F;
                const bool denseEvidenceCore = denseEvidencePath && evidencePointDistance <= evidencePointRadius;
                const float pathConfidence =
                    usesPath ? pathPixelRemovalConfidence(image, luminance, pathDistance, x, y, radius,
                                                          standardDeviation, dottedDroneTrail)
                             : 1.0F;
                const bool dottedCore = dottedDroneTrail && pathConfidence > 0.08F &&
                                        pathDistance.distance <= std::clamp(trail.width * 1.50F + 4.00F, 5.50F, 8.00F);
                const bool shouldMask = usesPath
                                            ? (denseEvidenceCore ||
                                               pathConfidence > (dottedDroneTrail ? 0.08F : 0.0F))
                                            : (pathDistance.distance <= std::max(2.25F, radius * 0.52F) ||
                                               shouldMaskTrailPixel(image, luminance, trail, x, y, standardDeviation));
                if (pathDistance.distance <= radius && shouldMask) {
                    const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                       static_cast<std::uint32_t>(x);
                    const float coreRadius = usesPath ? (dottedDroneTrail ? std::max(1.85F, radius * 0.40F)
                                                                          : std::max(3.25F, radius * 0.58F))
                                                      : std::max(2.25F, radius * 0.45F);
                    const float featherDenominator = std::max(1.0F, radius - coreRadius);
                    float alpha = pathDistance.distance <= coreRadius
                                      ? 1.0F
                                      : std::clamp((radius - pathDistance.distance) / featherDenominator, 0.0F, 1.0F);
                    if (usesPath) {
                        if (denseEvidenceCore) {
                            const float evidenceCoreRadius = std::max(2.75F, evidencePointRadius * 0.56F);
                            const float evidenceAlpha =
                                evidencePointDistance <= evidenceCoreRadius
                                    ? 1.0F
                                    : std::clamp((evidencePointRadius - evidencePointDistance) /
                                                     std::max(1.0F, evidencePointRadius - evidenceCoreRadius),
                                                 0.0F,
                                                 1.0F);
                            alpha = std::max(alpha, evidenceAlpha);
                        } else if (dottedCore) {
                            const float evidenceAlpha = std::clamp(pathConfidence * 2.50F, 0.0F, 1.0F);
                            alpha = std::max(alpha, evidenceAlpha);
                            if (pathConfidence >= 0.72F) {
                                alpha = std::max(
                                    alpha,
                                    std::clamp((pathConfidence - 0.55F) / 0.30F, 0.0F, 1.0F)
                                );
                            }
                        } else {
                            alpha *= std::clamp(pathConfidence, 0.0F, 1.0F);
                            if (dottedDroneTrail && pathConfidence >= 0.72F) {
                                alpha = std::max(
                                    alpha,
                                    std::clamp((pathConfidence - 0.55F) / 0.30F, 0.0F, 1.0F)
                                );
                            }
                        }
                    }
                    mask[pixel] = 1;
                    if (dottedDroneTrail) {
                        dottedDroneMask[pixel] = 1;
                    }
                    auto& metadata = maskMetadata[pixel];
                    if (alpha >= metadata.alpha) {
                        metadata.alpha = alpha;
                        metadata.unitX = pathDistance.unitX;
                        metadata.unitY = pathDistance.unitY;
                        metadata.signedDistance = pathDistance.signedDistance;
                    }
                }
            }
        }
        result.removedTrails += 1;
    }
    progress.report(ArtifactTrailProgressStage::Masking, 0.65, result.trails.size(), result.trails.size());

    const auto radius = static_cast<std::int32_t>(
        std::max<std::uint32_t>(std::max<std::uint32_t>(1, options.inpaintRadius), options.maskRadius + 5));
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            if (!mask[pixel]) {
                continue;
            }
            const auto& metadata = trailMaskMetadataAt(maskMetadata, pixel);
            std::array<float, 3> backgroundReference{};
            std::array<bool, 3> hasBackgroundReference{};

            for (std::uint16_t channel = 0; channel < std::min<std::uint16_t>(3, image.channels); ++channel) {
                std::vector<float> samples;
                float negativeSideSample = 0.0F;
                float positiveSideSample = 0.0F;
                float negativeSideTextureSample = 0.0F;
                float positiveSideTextureSample = 0.0F;
                std::vector<float> negativeSideSamples;
                std::vector<float> positiveSideSamples;
                bool hasNegativeSideSample = false;
                bool hasPositiveSideSample = false;
                const bool dottedPixel = dottedDroneMask[pixel] != 0;
                const float normalX = -metadata.unitY;
                const float normalY = metadata.unitX;
                const float signedDistance = metadata.signedDistance;
                const float sampleBand = static_cast<float>(radius + 4);
                negativeSideSamples.reserve(5);
                positiveSideSamples.reserve(5);
                for (int side : {-1, 1}) {
                    const float targetOffset = sampleBand * static_cast<float>(side);
                    // Translate the source patch instead of projecting every masked pixel
                    // onto one parallel line. Projection collapses normal-axis texture into
                    // visible columns along long dotted trails.
                    const float firstStep = targetOffset;
                    for (std::int32_t extra = 0; extra <= 18; extra += 3) {
                        float sample = 0.0F;
                        const float step = firstStep + static_cast<float>(extra * side);
                        const float sx = static_cast<float>(x) + normalX * step;
                        const float sy = static_cast<float>(y) + normalY * step;
                        if (sampleChannelBilinear(image, mask, sx, sy, channel, sample)) {
                            if (side < 0) {
                                negativeSideSamples.push_back(sample);
                            } else {
                                positiveSideSamples.push_back(sample);
                            }
                        }
                    }
                }
                if (!negativeSideSamples.empty()) {
                    negativeSideTextureSample = negativeSideSamples.front();
                    negativeSideSample = median(negativeSideSamples);
                    hasNegativeSideSample = true;
                }
                if (!positiveSideSamples.empty()) {
                    positiveSideTextureSample = positiveSideSamples.front();
                    positiveSideSample = median(positiveSideSamples);
                    hasPositiveSideSample = true;
                }

                const std::int32_t maxSearchRadius = radius * 2 + 2;
                for (std::int32_t searchRadius = radius;
                     samples.size() < 6 && searchRadius <= maxSearchRadius;
                     searchRadius = searchRadius * 2 + 1) {
                    samples.clear();
                    samples.reserve(static_cast<std::size_t>((searchRadius * 2 + 1) * (searchRadius * 2 + 1)));
                    for (std::int32_t dy = -searchRadius; dy <= searchRadius; ++dy) {
                        for (std::int32_t dx = -searchRadius; dx <= searchRadius; ++dx) {
                            const std::int32_t sx = static_cast<std::int32_t>(x) + dx;
                            const std::int32_t sy = static_cast<std::int32_t>(y) + dy;
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (mask[samplePixel]) {
                                continue;
                            }
                            if (likelyNeutralBrightStarSample(image, samplePixel)) {
                                continue;
                            }
                            samples.push_back(image.pixels[samplePixel * image.channels + channel]);
                        }
                    }
                }
                if (!samples.empty()) {
                    const auto [sampleMinIterator, sampleMaxIterator] =
                        std::minmax_element(samples.begin(), samples.end());
                    const float sampleMin = *sampleMinIterator;
                    const float sampleMax = *sampleMaxIterator;
                    const float medianSample = median(samples);
                    std::vector<float> absoluteDeviations;
                    absoluteDeviations.reserve(samples.size());
                    for (const auto sample : samples) {
                        absoluteDeviations.push_back(std::fabs(sample - medianSample));
                    }
                    const float medianAbsoluteDeviation = median(absoluteDeviations);
                    const float robustRadius = std::max(0.003F, medianAbsoluteDeviation * 3.5F);
                    float robustSum = 0.0F;
                    std::size_t robustCount = 0;
                    for (const auto sample : samples) {
                        if (std::fabs(sample - medianSample) <= robustRadius) {
                            robustSum += sample;
                            robustCount += 1;
                        }
                    }
                    const float meanSample = robustCount > 0 ? robustSum / static_cast<float>(robustCount) : medianSample;
                    float varianceSample = 0.0F;
                    for (const auto sample : samples) {
                        if (std::fabs(sample - medianSample) <= robustRadius) {
                            const float delta = sample - meanSample;
                            varianceSample += delta * delta;
                        }
                    }
                    varianceSample /= static_cast<float>(std::max<std::size_t>(1, robustCount));
                    float cloneSample = medianSample;
                    float clonedTextureResidual = 0.0F;
                    bool hasCloneSample = false;
                    if (hasNegativeSideSample && hasPositiveSideSample) {
                        const float alongCoordinate =
                            static_cast<float>(x) * metadata.unitX + static_cast<float>(y) * metadata.unitY;
                        const auto alongBlock = static_cast<std::uint32_t>(
                            std::max(0.0F, std::floor(alongCoordinate / 18.0F)));
                        const float coherentSideBias = deterministicNoise(alongBlock, 0U, 19U) * 0.14F;
                        const float sideMix =
                            dottedPixel
                                ? std::clamp(0.50F + signedDistance / std::max(1.0F, sampleBand * 3.0F) +
                                                 coherentSideBias,
                                             0.14F,
                                             0.86F)
                                : std::clamp((signedDistance + sampleBand) / std::max(1.0F, sampleBand * 2.0F), 0.0F, 1.0F);
                        cloneSample = negativeSideSample * (1.0F - sideMix) + positiveSideSample * sideMix;
                        const bool useNegativeTexture =
                            signedDistance < -0.75F ||
                            (std::fabs(signedDistance) <= 0.75F && deterministicNoise(alongBlock, 0U, 47U) < 0.0F);
                        const float textureClone =
                            useNegativeTexture ? negativeSideTextureSample : positiveSideTextureSample;
                        const float textureBackground = useNegativeTexture ? negativeSideSample : positiveSideSample;
                        clonedTextureResidual = std::clamp(textureClone - textureBackground, -0.040F, 0.040F);
                        hasCloneSample = true;
                    } else if (hasNegativeSideSample || hasPositiveSideSample) {
                        cloneSample = hasNegativeSideSample ? negativeSideSample : positiveSideSample;
                        hasCloneSample = true;
                    }
                    const float contextualSample =
                        dottedPixel ? medianSample * 0.54F + meanSample * 0.46F : medianSample * 0.72F + meanSample * 0.28F;
                    const float directionalWeight = hasCloneSample ? (dottedPixel ? 0.96F : 0.50F) : 0.0F;
                    const float base = contextualSample * (1.0F - directionalWeight) + cloneSample * directionalWeight;
                    backgroundReference[channel] = hasCloneSample ? cloneSample : contextualSample;
                    hasBackgroundReference[channel] = true;
                    const float textureNoise =
                        deterministicNoise(x, y, 97) * 0.85F + deterministicNoise(x, y, channel) * 0.15F;
                    const float texture = dottedPixel
                                              ? clonedTextureResidual +
                                                    textureNoise * std::sqrt(std::max(0.0F, varianceSample)) * 0.18F
                                              : textureNoise * std::sqrt(std::max(0.0F, varianceSample)) * 0.20F;
                    const float repaired = std::clamp(base + texture, sampleMin, sampleMax);
                    const float alpha = std::clamp(metadata.alpha, 0.0F, 1.0F);
                    const float original = image.pixels[pixel * image.channels + channel];
                    output.pixels[pixel * output.channels + channel] = original * (1.0F - alpha) + repaired * alpha;
                }
            }

            if (output.channels >= 3) {
                auto& red = output.pixels[pixel * output.channels];
                auto& green = output.pixels[pixel * output.channels + 1];
                auto& blue = output.pixels[pixel * output.channels + 2];
                const float redExcess = red - std::max(green, blue);
                const float magentaExcess = std::min(red, blue) - green;
                const float alpha = std::clamp(metadata.alpha, 0.0F, 1.0F);
                if (redExcess > 0.006F) {
                    const float target = std::max(green, blue) + 0.004F;
                    const float strength = std::clamp((redExcess - 0.006F) / 0.070F, 0.0F, 0.74F) * alpha;
                    red = red * (1.0F - strength) + target * strength;
                }
                if (magentaExcess > 0.006F) {
                    const float target = std::max(green, std::min(red, blue) - 0.004F);
                    const float strength = std::clamp((magentaExcess - 0.006F) / 0.070F, 0.0F, 0.62F) * alpha;
                    green = green * (1.0F - strength) + target * strength;
                } else {
                    const float chroma = std::max({red, green, blue}) - std::min({red, green, blue});
                    const bool hasReference =
                        hasBackgroundReference[0] && hasBackgroundReference[1] && hasBackgroundReference[2];
                    const float referenceChroma =
                        hasReference
                            ? std::max({backgroundReference[0], backgroundReference[1], backgroundReference[2]}) -
                                  std::min({backgroundReference[0], backgroundReference[1], backgroundReference[2]})
                            : 0.0F;
                    const float allowedChroma = std::max(0.070F, referenceChroma + 0.018F);
                    if (chroma > allowedChroma) {
                        const float average = (red + green + blue) / 3.0F;
                        const float strength =
                            std::clamp((chroma - allowedChroma) / 0.120F, 0.0F, 0.30F) * alpha;
                        const float targetRed = hasReference ? backgroundReference[0] : average;
                        const float targetGreen = hasReference ? backgroundReference[1] : average;
                        const float targetBlue = hasReference ? backgroundReference[2] : average;
                        red = red * (1.0F - strength) + targetRed * strength;
                        green = green * (1.0F - strength) + targetGreen * strength;
                        blue = blue * (1.0F - strength) + targetBlue * strength;
                    }
                }
            }
        }
        progress.reportRow(ArtifactTrailProgressStage::Inpainting, y, image.height, 0.65, 0.78);
    }

    progress.report(ArtifactTrailProgressStage::Cleaning, 0.78);
    if (output.channels >= 3) {
        ImageBuffer harmonized = output;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const auto pixel = static_cast<std::size_t>(y) * image.width + x;
                const auto& metadata = trailMaskMetadataAt(maskMetadata, pixel);
                const float alpha = std::clamp(metadata.alpha, 0.0F, 1.0F);
                if (!mask[pixel] || alpha <= 0.0F) {
                    continue;
                }

                std::vector<float> haloLuminance;
                haloLuminance.reserve(320);
                const float normalX = -metadata.unitY;
                const float normalY = metadata.unitX;
                for (const int side : {-1, 1}) {
                    for (const float distance : {12.0F, 16.0F, 20.0F, 24.0F, 30.0F}) {
                        const float sx = static_cast<float>(x) + normalX * distance * static_cast<float>(side);
                        const float sy = static_cast<float>(y) + normalY * distance * static_cast<float>(side);
                        float red = 0.0F;
                        float green = 0.0F;
                        float blue = 0.0F;
                        if (sampleChannelBilinear(image, mask, sx, sy, 0, red) &&
                            sampleChannelBilinear(image, mask, sx, sy, 1, green) &&
                            sampleChannelBilinear(image, mask, sx, sy, 2, blue)) {
                            haloLuminance.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                }
                if (haloLuminance.size() < 4) {
                    for (std::int32_t dy = -12; dy <= 12; ++dy) {
                        for (std::int32_t dx = -12; dx <= 12; ++dx) {
                            if (dx == 0 && dy == 0) {
                                continue;
                            }
                            const std::int32_t sx = static_cast<std::int32_t>(x) + dx;
                            const std::int32_t sy = static_cast<std::int32_t>(y) + dy;
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (mask[samplePixel] || likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const float red = output.pixels[samplePixel * output.channels];
                            const float green = output.pixels[samplePixel * output.channels + 1];
                            const float blue = output.pixels[samplePixel * output.channels + 2];
                            haloLuminance.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                }

                const auto base = pixel * output.channels;
                const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                          output.pixels[base + 2] * 0.0722F;
                const bool dottedPixel = dottedDroneMask[pixel] != 0;
                if (!dottedPixel && haloLuminance.size() >= 8) {
                    const float targetLuma = median(haloLuminance);
                    const float delta = std::clamp(targetLuma - currentLuma,
                                                   -0.018F,
                                                   0.018F);
                    const float strength = 0.28F * alpha;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        harmonized.pixels[base + channel] =
                            std::clamp(harmonized.pixels[base + channel] + delta * strength, 0.0F, 1.0F);
                    }
                }

                float neighborSum[3] = {0.0F, 0.0F, 0.0F};
                float neighborWeight = 0.0F;
                for (std::int32_t dy = -1; dy <= 1; ++dy) {
                    for (std::int32_t dx = -1; dx <= 1; ++dx) {
                        const std::int32_t sx = static_cast<std::int32_t>(x) + dx;
                        const std::int32_t sy = static_cast<std::int32_t>(y) + dy;
                        if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                            sy >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const auto samplePixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                            static_cast<std::uint32_t>(sx);
                        const float weight = (dx == 0 && dy == 0) ? 2.0F : 1.0F;
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            neighborSum[channel] += output.pixels[samplePixel * output.channels + channel] * weight;
                        }
                        neighborWeight += weight;
                    }
                }
                if (!dottedPixel && neighborWeight > 0.0F) {
                    const float strength = 0.08F * alpha;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float smoothed = neighborSum[channel] / neighborWeight;
                        harmonized.pixels[base + channel] =
                            harmonized.pixels[base + channel] * (1.0F - strength) + smoothed * strength;
                    }
                }
            }
        }
        output = std::move(harmonized);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.79);

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer cleaned = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio >= 0.90F && trail.path.size() >= 2) {
                continue;
            }
            const float cleanupRadius = std::clamp(trail.width * 1.55F + 1.95F, 3.40F, 5.15F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - cleanupRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + cleanupRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - cleanupRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + cleanupRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > cleanupRadius) {
                        continue;
                    }

                    const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                       static_cast<std::uint32_t>(x);
                    std::vector<float> samples[3];
                    std::vector<float> lumaSamples;
                    for (auto& channelSamples : samples) {
                        channelSamples.reserve(48);
                    }
                    lumaSamples.reserve(48);
                    for (std::int32_t dy = -7; dy <= 7; ++dy) {
                        for (std::int32_t dx = -7; dx <= 7; ++dx) {
                            const std::int32_t sx = x + dx;
                            const std::int32_t sy = y + dy;
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto sampleDistance =
                                distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                            if (sampleDistance.distance <= cleanupRadius + 1.25F) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const float red = output.pixels[samplePixel * output.channels];
                            const float green = output.pixels[samplePixel * output.channels + 1];
                            const float blue = output.pixels[samplePixel * output.channels + 2];
                            samples[0].push_back(red);
                            samples[1].push_back(green);
                            samples[2].push_back(blue);
                            lumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                    if (lumaSamples.size() < 10) {
                        continue;
                    }

                    const float targetLuma = median(lumaSamples);
                    const float targetLumaMean = std::accumulate(lumaSamples.begin(), lumaSamples.end(), 0.0F) /
                                                 static_cast<float>(lumaSamples.size());
                    const float darkTargetLuma = targetLuma * 0.55F + targetLumaMean * 0.45F;
                    float target[3] = {0.0F, 0.0F, 0.0F};
                    float targetMean[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(samples[channel]);
                        targetMean[channel] = std::accumulate(samples[channel].begin(), samples[channel].end(), 0.0F) /
                                              static_cast<float>(samples[channel].size());
                    }
                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                              output.pixels[base + 2] * 0.0722F;
                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    const float residual = currentLuma - targetLuma;
                    const float darkResidual = darkTargetLuma - currentLuma;
                    const float originalExcess = originalLuma - targetLuma;
                    const bool coloredResidual =
                        warmExcessAt(image, pixel) > 0.0045F || colorVarianceAt(image, pixel) > 0.0028F;
                    const float evidence = std::max(residual, originalExcess);
                    const float maskAlpha = trailMaskMetadataAt(maskMetadata, pixel).alpha;
                    const bool maskedDottedPixel = dottedDroneMask[pixel] != 0 && maskAlpha > 0.0F;
                    const bool darkRepairNeeded = maskedDottedPixel && darkResidual > 0.0025F;
                    if (evidence < 0.0005F && !coloredResidual && !darkRepairNeeded) {
                        continue;
                    }
                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, cleanupRadius), 0.0F, 1.0F) * 0.35F;
                    const float brightStrength =
                        std::clamp((std::max(evidence, 0.0007F) - 0.00028F) / 0.0048F, 0.22F, 0.98F);
                    const float darkStrength = std::clamp((darkResidual - 0.0015F) / 0.0100F, 0.10F, 0.58F) *
                                               (0.45F + std::clamp(maskAlpha, 0.0F, 1.0F) * 0.55F);
                    const float strength =
                        (darkRepairNeeded && evidence < 0.0008F && !coloredResidual
                             ? darkStrength
                             : std::max(brightStrength, darkRepairNeeded ? darkStrength * 0.70F : 0.0F)) *
                        distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float cleanupTarget =
                            maskedDottedPixel ? target[channel] * 0.92F + targetMean[channel] * 0.08F : target[channel];
                        cleaned.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + cleanupTarget * strength;
                    }
                }
            }
        }
        output = std::move(cleaned);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer lowHorizonPeaks = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || trail.path.size() < 2) {
                continue;
            }

            struct ResidualPeak {
                float x = 0.0F;
                float y = 0.0F;
                float unitX = 1.0F;
                float unitY = 0.0F;
                float residual = 0.0F;
                float originalEvidence = 0.0F;
            };
            std::vector<ResidualPeak> peaks;
            peaks.reserve(static_cast<std::size_t>(std::max(8.0F, trail.length / 18.0F)));

            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                for (float step = 0.0F; step <= segmentLength; step += 8.0F) {
                    const float centerPointX = start.x + unitX * step;
                    const float centerPointY = start.y + unitY * step;
                    std::vector<ResidualPeak> stepPeaks;
                    stepPeaks.reserve(4);
                    for (const float normalOffset :
                         {-8.0F, -7.0F, -6.0F, -5.0F, -4.0F, -3.0F, -2.0F, -1.0F, 0.0F, 1.0F, 2.0F, 3.0F, 4.0F,
                          5.0F, 6.0F, 7.0F, 8.0F}) {
                        const float px = centerPointX + normalX * normalOffset;
                        const float py = centerPointY + normalY * normalOffset;
                        const auto x = static_cast<std::int32_t>(std::lround(px));
                        const auto y = static_cast<std::int32_t>(std::lround(py));
                        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                            y >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }

                        const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                           static_cast<std::uint32_t>(x);
                        std::vector<float> backgroundSamples;
                        backgroundSamples.reserve(18);
                        for (const float offset : {8.0F, 13.0F, 20.0F, 31.0F, 45.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-5.0F, 0.0F, 5.0F}) {
                                    const auto sx = static_cast<std::int32_t>(
                                        std::lround(px + normalX * offset * side + unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(
                                        std::lround(py + normalY * offset * side + unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    const auto sampleBase = samplePixel * output.channels;
                                    backgroundSamples.push_back(output.pixels[sampleBase] * 0.2126F +
                                                                output.pixels[sampleBase + 1] * 0.7152F +
                                                                output.pixels[sampleBase + 2] * 0.0722F);
                                }
                            }
                        }
                        if (backgroundSamples.size() < 8) {
                            continue;
                        }

                        const float background = percentileValue(backgroundSamples, 0.42F);
                        const auto base = pixel * output.channels;
                        const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                                  output.pixels[base + 2] * 0.0722F;
                        const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                                   image.pixels[base + 2] * 0.0722F;
                        const float originalColorEvidence =
                            navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 1.45F +
                            std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.28F;
                        const float originalEvidence =
                            std::max(originalLuma - background, originalColorEvidence * 2.4F);
                        const float residual = currentLuma - background;
                        if ((residual < 0.0034F || originalEvidence < 0.0062F) &&
                            !(residual > 0.0018F && originalColorEvidence > 0.0020F)) {
                            continue;
                        }
                        bool localDuplicate = false;
                        for (auto& peak : stepPeaks) {
                            const float dx = peak.x - px;
                            const float dy = peak.y - py;
                            if (dx * dx + dy * dy < 10.0F) {
                                if (residual > peak.residual) {
                                    peak = {px, py, unitX, unitY, residual, originalEvidence};
                                }
                                localDuplicate = true;
                                break;
                            }
                        }
                        if (!localDuplicate) {
                            stepPeaks.push_back({px, py, unitX, unitY, residual, originalEvidence});
                        }
                    }

                    for (const auto& candidate : stepPeaks) {
                        auto duplicateIndex = peaks.end();
                        for (auto peak = peaks.begin(); peak != peaks.end(); ++peak) {
                            const float dx = peak->x - candidate.x;
                            const float dy = peak->y - candidate.y;
                            if (dx * dx + dy * dy < 132.0F) {
                                duplicateIndex = peak;
                                break;
                            }
                        }
                        if (duplicateIndex == peaks.end()) {
                            peaks.push_back(candidate);
                        } else if (candidate.residual > duplicateIndex->residual) {
                            *duplicateIndex = candidate;
                        }
                    }
                }
            }

            for (const auto& peak : peaks) {
                const float normalX = -peak.unitY;
                const float normalY = peak.unitX;
                const float normalRadius = std::clamp(1.30F + peak.residual * 12.0F, 1.35F, 2.25F);
                const float alongRadius = std::clamp(2.25F + peak.residual * 18.0F, 2.40F, 4.20F);
                const float bounds = std::ceil(std::max(normalRadius, alongRadius) + 1.0F);
                for (std::int32_t y = static_cast<std::int32_t>(std::floor(peak.y - bounds));
                     y <= static_cast<std::int32_t>(std::ceil(peak.y + bounds)); ++y) {
                    for (std::int32_t x = static_cast<std::int32_t>(std::floor(peak.x - bounds));
                         x <= static_cast<std::int32_t>(std::ceil(peak.x + bounds)); ++x) {
                        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                            y >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const float dx = static_cast<float>(x) - peak.x;
                        const float dy = static_cast<float>(y) - peak.y;
                        const float along = dx * peak.unitX + dy * peak.unitY;
                        const float normal = dx * normalX + dy * normalY;
                        const float ellipse = std::sqrt((along / alongRadius) * (along / alongRadius) +
                                                        (normal / normalRadius) * (normal / normalRadius));
                        if (ellipse > 1.0F) {
                            continue;
                        }

                        std::vector<float> targetSamples[3];
                        for (auto& channelSamples : targetSamples) {
                            channelSamples.reserve(24);
                        }
                        for (const float offset : {9.0F, 15.0F, 24.0F, 38.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-6.0F, 0.0F, 6.0F}) {
                                    const auto sx = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(x) + normalX * offset * side + peak.unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(y) + normalY * offset * side + peak.unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    const auto sampleBase = samplePixel * output.channels;
                                    targetSamples[0].push_back(output.pixels[sampleBase]);
                                    targetSamples[1].push_back(output.pixels[sampleBase + 1]);
                                    targetSamples[2].push_back(output.pixels[sampleBase + 2]);
                                }
                            }
                        }
                        if (targetSamples[0].size() < 8 || targetSamples[1].size() < 8 || targetSamples[2].size() < 8) {
                            continue;
                        }

                        float target[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = percentileValue(targetSamples[channel], 0.36F);
                        }
                        const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                           static_cast<std::uint32_t>(x);
                        const auto base = pixel * output.channels;
                        const float residualWeight = std::clamp((peak.residual - 0.0012F) / 0.018F, 0.28F, 0.92F);
                        const float originalWeight =
                            std::clamp((peak.originalEvidence - 0.0040F) / 0.030F, 0.18F, 0.86F);
                        const float strength = std::clamp(
                            std::max(residualWeight, originalWeight) * (1.0F - ellipse * 0.56F), 0.0F, 0.88F);
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            lowHorizonPeaks.pixels[base + channel] =
                                output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                        }
                    }
                }
            }
        }
        output = std::move(lowHorizonPeaks);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer coreMatched = output;
        std::vector<std::uint8_t> touched(image.pixelCount(), 0);
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || trail.path.size() < 2) {
                continue;
            }
            const bool horizonTrail = centerRatio >= 0.90F;
            if (horizonTrail) {
                continue;
            }

            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); ++step) {
                    const float centerPointX = start.x + unitX * static_cast<float>(step);
                    const float centerPointY = start.y + unitY * static_cast<float>(step);
                    for (const float normalOffset : {-2.0F, -1.0F, 0.0F, 1.0F, 2.0F}) {
                        const auto x =
                            static_cast<std::int32_t>(std::lround(centerPointX + normalX * normalOffset));
                        const auto y =
                            static_cast<std::int32_t>(std::lround(centerPointY + normalY * normalOffset));
                        if (x < 2 || y < 2 || x >= static_cast<std::int32_t>(image.width) - 2 ||
                            y >= static_cast<std::int32_t>(image.height) - 2) {
                            continue;
                        }

                        const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                           static_cast<std::uint32_t>(x);
                        if (touched[pixel] != 0) {
                            continue;
                        }
                        touched[pixel] = 1;

                        std::vector<float> targetSamples[3];
                        std::vector<float> targetLumaSamples;
                        for (auto& channelSamples : targetSamples) {
                            channelSamples.reserve(30);
                        }
                        targetLumaSamples.reserve(30);
                        for (const float offset : {7.0F, 11.0F, 17.0F, 25.0F, 36.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-4.0F, 0.0F, 4.0F}) {
                                    const auto sx = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(x) + normalX * offset * side + unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(y) + normalY * offset * side + unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    const auto sampleBase = samplePixel * output.channels;
                                    const float red = output.pixels[sampleBase];
                                    const float green = output.pixels[sampleBase + 1];
                                    const float blue = output.pixels[sampleBase + 2];
                                    targetSamples[0].push_back(red);
                                    targetSamples[1].push_back(green);
                                    targetSamples[2].push_back(blue);
                                    targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                }
                            }
                        }
                        if (targetLumaSamples.size() < 12) {
                            continue;
                        }

                        float target[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = percentileValue(targetSamples[channel], 0.30F);
                        }
                        const float targetLuma = target[0] * 0.2126F + target[1] * 0.7152F + target[2] * 0.0722F;
                        const auto base = pixel * output.channels;
                        const float currentRed = output.pixels[base];
                        const float currentGreen = output.pixels[base + 1];
                        const float currentBlue = output.pixels[base + 2];
                        const float currentLuma = currentRed * 0.2126F + currentGreen * 0.7152F + currentBlue * 0.0722F;
                        const float currentMaximum = std::max({currentRed, currentGreen, currentBlue});
                        const float currentMinimum = std::min({currentRed, currentGreen, currentBlue});
                        const float currentChroma = currentMaximum - currentMinimum;
                        if (currentMaximum > 0.61F && currentChroma < 0.070F) {
                            continue;
                        }

                        const float originalColorEvidence =
                            navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) +
                            std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.18F;
                        const float residual = currentLuma - targetLuma;
                        const bool corePixel = std::fabs(normalOffset) <= 1.0F;
                        const bool evidence =
                            (horizonTrail && corePixel) || residual > (horizonTrail ? 0.0015F : 0.0030F) ||
                            currentChroma > (horizonTrail ? 0.039F : 0.045F) ||
                            originalColorEvidence > (horizonTrail ? 0.0015F : 0.0025F);
                        if (!evidence) {
                            continue;
                        }

                        float strength = horizonTrail ? (std::fabs(normalOffset) < 0.5F   ? 0.86F
                                                         : std::fabs(normalOffset) < 1.5F ? 0.66F
                                                                                         : 0.34F)
                                                      : (std::fabs(normalOffset) < 0.5F   ? 0.58F
                                                         : std::fabs(normalOffset) < 1.5F ? 0.42F
                                                                                         : 0.24F);
                        if (residual > (horizonTrail ? 0.0060F : 0.0100F)) {
                            strength += 0.12F;
                        }
                        if (currentChroma > (horizonTrail ? 0.047F : 0.070F) ||
                            originalColorEvidence > (horizonTrail ? 0.0022F : 0.0032F)) {
                            strength += 0.10F;
                        }
                        strength = std::clamp(strength, 0.0F, horizonTrail ? 0.94F : 0.72F);
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            coreMatched.pixels[base + channel] =
                                output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                        }
                    }
                }
            }
        }
        output = std::move(coreMatched);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer spotPatched = output;
        std::vector<std::uint8_t> processed(image.pixelCount(), 0);
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || trail.path.size() < 2) {
                continue;
            }
            const bool horizonTrail = centerRatio >= 0.90F;

            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                const std::int32_t stepStride = horizonTrail ? 6 : 2;
                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); step += stepStride) {
                    const float centerPointX = start.x + unitX * static_cast<float>(step);
                    const float centerPointY = start.y + unitY * static_cast<float>(step);
                    for (const float normalOffset :
                         {-7.0F, -6.0F, -5.0F, -4.0F, -3.0F, -2.0F, -1.0F, 0.0F, 1.0F, 2.0F, 3.0F, 4.0F,
                          5.0F, 6.0F, 7.0F}) {
                        const bool outerOffset = std::fabs(normalOffset) > 3.0F;
                        const auto centerX =
                            static_cast<std::int32_t>(std::lround(centerPointX + normalX * normalOffset));
                        const auto centerY =
                            static_cast<std::int32_t>(std::lround(centerPointY + normalY * normalOffset));
                        if (centerX < 3 || centerY < 3 || centerX >= static_cast<std::int32_t>(image.width) - 3 ||
                            centerY >= static_cast<std::int32_t>(image.height) - 3) {
                            continue;
                        }
                        const auto centerPixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(centerY)) * image.width +
                            static_cast<std::uint32_t>(centerX);
                        if (processed[centerPixel] != 0) {
                            continue;
                        }
                        processed[centerPixel] = 1;

                        std::vector<float> targetSamples[3];
                        for (auto& channelSamples : targetSamples) {
                            channelSamples.reserve(30);
                        }
                        std::vector<float> targetLumaSamples;
                        targetLumaSamples.reserve(30);
                        for (const float offset : {8.0F, 13.0F, 20.0F, 30.0F, 44.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-5.0F, 0.0F, 5.0F}) {
                                    const auto sx = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(centerX) + normalX * offset * side +
                                                    unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(centerY) + normalY * offset * side +
                                                    unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    const auto sampleBase = samplePixel * output.channels;
                                    const float red = output.pixels[sampleBase];
                                    const float green = output.pixels[sampleBase + 1];
                                    const float blue = output.pixels[sampleBase + 2];
                                    targetSamples[0].push_back(red);
                                    targetSamples[1].push_back(green);
                                    targetSamples[2].push_back(blue);
                                    targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                }
                            }
                        }
                        if (targetLumaSamples.size() < 12) {
                            continue;
                        }

                        float target[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = percentileValue(targetSamples[channel], 0.25F);
                        }
                        const float targetLuma = target[0] * 0.2126F + target[1] * 0.7152F + target[2] * 0.0722F;
                        const auto centerBase = centerPixel * output.channels;
                        const float currentRed = output.pixels[centerBase];
                        const float currentGreen = output.pixels[centerBase + 1];
                        const float currentBlue = output.pixels[centerBase + 2];
                        const float currentLuma =
                            currentRed * 0.2126F + currentGreen * 0.7152F + currentBlue * 0.0722F;
                        const float currentMaximum = std::max({currentRed, currentGreen, currentBlue});
                        const float currentMinimum = std::min({currentRed, currentGreen, currentBlue});
                        const float currentChroma = currentMaximum - currentMinimum;
                        if ((!outerOffset && currentMaximum > 0.61F && currentChroma < 0.070F) ||
                            (outerOffset && currentMaximum > 0.66F && currentChroma < 0.090F)) {
                            continue;
                        }

                        const float originalColorEvidence =
                            navigationLightScoreAt(image, centerPixel) + warmExcessAt(image, centerPixel) +
                            std::sqrt(std::max(0.0F, colorVarianceAt(image, centerPixel))) * 0.18F;
                        const float residual = currentLuma - targetLuma;
                        if ((!outerOffset && residual <= 0.0035F && currentChroma <= 0.045F &&
                             originalColorEvidence <= 0.0022F) ||
                            (outerOffset && (residual <= 0.0060F ||
                                             (currentChroma <= 0.045F && originalColorEvidence <= 0.0030F)))) {
                            continue;
                        }
                        const float baseStrength =
                            outerOffset
                                ? std::clamp((residual + currentChroma * 0.035F + originalColorEvidence * 0.25F) /
                                                 (horizonTrail ? 0.022F : 0.026F),
                                             horizonTrail ? 0.36F : 0.32F,
                                             horizonTrail ? 0.78F : 0.72F)
                                : std::clamp((residual + currentChroma * 0.05F + originalColorEvidence * 0.30F) /
                                                 (horizonTrail ? 0.018F : 0.022F),
                                             horizonTrail ? 0.45F : 0.38F,
                                             horizonTrail ? 0.92F : 0.82F);

                        for (std::int32_t y = centerY - 3; y <= centerY + 3; ++y) {
                            for (std::int32_t x = centerX - 5; x <= centerX + 5; ++x) {
                                if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                                    y >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const float dx = static_cast<float>(x - centerX);
                                const float dy = static_cast<float>(y - centerY);
                                const float along = dx * unitX + dy * unitY;
                                const float normal = dx * normalX + dy * normalY;
                                const float alongRadius = outerOffset ? (horizonTrail ? 2.05F : 2.80F)
                                                                      : (horizonTrail ? 2.35F : 3.20F);
                                const float normalRadius = outerOffset ? (horizonTrail ? 1.10F : 1.55F)
                                                                       : (horizonTrail ? 1.25F : 1.75F);
                                const float ellipse =
                                    std::sqrt((along / alongRadius) * (along / alongRadius) +
                                              (normal / normalRadius) * (normal / normalRadius));
                                if (ellipse > 1.0F) {
                                    continue;
                                }
                                const float strength =
                                    std::clamp(baseStrength * (1.0F - ellipse * 0.60F),
                                               0.0F,
                                               outerOffset ? (horizonTrail ? 0.66F : 0.72F)
                                                           : (horizonTrail ? 0.78F : 0.82F));
                                if (strength <= 0.0F) {
                                    continue;
                                }
                                const auto pixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                    static_cast<std::uint32_t>(x);
                                const auto base = pixel * output.channels;
                                for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                    spotPatched.pixels[base + channel] =
                                        spotPatched.pixels[base + channel] * (1.0F - strength) +
                                        target[channel] * strength;
                                }
                            }
                        }
                    }
                }
            }
        }
        output = std::move(spotPatched);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        for (std::uint8_t sideBeaconPass = 0; sideBeaconPass < 2; ++sideBeaconPass) {
            const bool selectivePass = sideBeaconPass == 1;
            ImageBuffer sideBeaconPatched = output;
            std::vector<std::uint8_t> processed(image.pixelCount(), 0);
            for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || centerRatio >= 0.90F || trail.path.size() < 2) {
                continue;
            }

            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength);
                     step += selectivePass ? 2 : 1) {
                    const float centerPointX = start.x + unitX * static_cast<float>(step);
                    const float centerPointY = start.y + unitY * static_cast<float>(step);
                    for (const float normalOffset :
                         {-8.0F, -7.0F, -6.0F, -5.0F, -4.0F, -3.0F, -2.0F, -1.0F, 0.0F, 1.0F, 2.0F, 3.0F,
                          4.0F, 5.0F, 6.0F, 7.0F, 8.0F}) {
                        const auto centerX =
                            static_cast<std::int32_t>(std::lround(centerPointX + normalX * normalOffset));
                        const auto centerY =
                            static_cast<std::int32_t>(std::lround(centerPointY + normalY * normalOffset));
                        if (centerX < 3 || centerY < 3 || centerX >= static_cast<std::int32_t>(image.width) - 3 ||
                            centerY >= static_cast<std::int32_t>(image.height) - 3) {
                            continue;
                        }
                        const auto centerPixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(centerY)) * image.width +
                            static_cast<std::uint32_t>(centerX);
                        if (processed[centerPixel] != 0) {
                            continue;
                        }
                        processed[centerPixel] = 1;

                        std::vector<float> targetSamples[3];
                        for (auto& samples : targetSamples) {
                            samples.reserve(30);
                        }
                        std::vector<float> targetLumaSamples;
                        targetLumaSamples.reserve(30);
                        for (const float offset : {8.0F, 13.0F, 20.0F, 30.0F, 44.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-5.0F, 0.0F, 5.0F}) {
                                    const auto sx = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(centerX) + normalX * offset * side +
                                                    unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(centerY) + normalY * offset * side +
                                                    unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    const auto sampleBase = samplePixel * output.channels;
                                    const float red = output.pixels[sampleBase];
                                    const float green = output.pixels[sampleBase + 1];
                                    const float blue = output.pixels[sampleBase + 2];
                                    targetSamples[0].push_back(red);
                                    targetSamples[1].push_back(green);
                                    targetSamples[2].push_back(blue);
                                    targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                }
                            }
                        }
                        if (targetLumaSamples.size() < 12) {
                            continue;
                        }

                        float target[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = percentileValue(targetSamples[channel], selectivePass ? 0.16F : 0.22F);
                        }
                        const float targetLuma = target[0] * 0.2126F + target[1] * 0.7152F + target[2] * 0.0722F;
                        const auto centerBase = centerPixel * output.channels;
                        const float currentRed = output.pixels[centerBase];
                        const float currentGreen = output.pixels[centerBase + 1];
                        const float currentBlue = output.pixels[centerBase + 2];
                        const float currentLuma =
                            currentRed * 0.2126F + currentGreen * 0.7152F + currentBlue * 0.0722F;
                        const float currentMaximum = std::max({currentRed, currentGreen, currentBlue});
                        const float currentMinimum = std::min({currentRed, currentGreen, currentBlue});
                        const float currentChroma = currentMaximum - currentMinimum;
                        if (currentMaximum > 0.70F && currentChroma < 0.10F) {
                            continue;
                        }
                        const float residual = currentLuma - targetLuma;
                        if ((!selectivePass && (residual <= 0.022F || currentChroma <= 0.075F)) ||
                            (selectivePass && (residual <= 0.010F || currentChroma <= 0.035F))) {
                            continue;
                        }

                        const float baseStrength =
                            selectivePass
                                ? std::clamp((residual + currentChroma * 0.04F) / 0.035F, 0.18F, 0.55F)
                                : std::clamp((residual + currentChroma * 0.06F) / 0.035F, 0.62F, 0.95F);
                        for (std::int32_t y = centerY - 4; y <= centerY + 4; ++y) {
                            for (std::int32_t x = centerX - 6; x <= centerX + 6; ++x) {
                                if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                                    y >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const float dx = static_cast<float>(x - centerX);
                                const float dy = static_cast<float>(y - centerY);
                                const float along = dx * unitX + dy * unitY;
                                const float normal = dx * normalX + dy * normalY;
                                const float alongRadius = selectivePass ? 2.80F : 3.50F;
                                const float normalRadius = selectivePass ? 1.55F : 2.00F;
                                const float ellipse =
                                    std::sqrt((along / alongRadius) * (along / alongRadius) +
                                              (normal / normalRadius) * (normal / normalRadius));
                                if (ellipse > 1.0F) {
                                    continue;
                                }
                                const float strength = std::clamp(baseStrength * (1.0F - ellipse * 0.55F),
                                                                  0.0F,
                                                                  selectivePass ? 0.55F : 0.95F);
                                if (strength <= 0.0F) {
                                    continue;
                                }
                                const auto pixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                    static_cast<std::uint32_t>(x);
                                const auto base = pixel * output.channels;
                                for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                    sideBeaconPatched.pixels[base + channel] =
                                        sideBeaconPatched.pixels[base + channel] * (1.0F - strength) +
                                        target[channel] * strength;
                                }
                            }
                        }
                    }
                }
            }
            }
            output = std::move(sideBeaconPatched);
        }
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.81);

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer dottedResidualPatched = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || centerRatio > 0.970F || trail.path.size() < 2) {
                continue;
            }

            const bool horizonTrail = centerRatio >= 0.90F;
            struct ResidualCluster {
                float x = 0.0F;
                float y = 0.0F;
                float unitX = 1.0F;
                float unitY = 0.0F;
                float score = 0.0F;
                float maxResidual = 0.0F;
                float maxOriginalLumaExcess = 0.0F;
                float maxChroma = 0.0F;
                float maxOriginalColor = 0.0F;
                std::uint32_t count = 0;
            };
            std::vector<ResidualCluster> clusters;
            clusters.reserve(static_cast<std::size_t>(std::max(8.0F, trail.length / 20.0F)));
            std::vector<std::uint8_t> considered(image.pixelCount(), 0);

            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); ++step) {
                    const float centerPointX = start.x + unitX * static_cast<float>(step);
                    const float centerPointY = start.y + unitY * static_cast<float>(step);
                    for (const float normalOffset :
                         {-10.0F, -9.0F, -8.0F, -7.0F, -6.0F, -5.0F, -4.0F, -3.0F, -2.0F, -1.0F,
                          0.0F,   1.0F,  2.0F,  3.0F,  4.0F,  5.0F,  6.0F,  7.0F,  8.0F,  9.0F,
                          10.0F}) {
                        const auto x = static_cast<std::int32_t>(std::lround(centerPointX + normalX * normalOffset));
                        const auto y = static_cast<std::int32_t>(std::lround(centerPointY + normalY * normalOffset));
                        if (x < 4 || y < 4 || x >= static_cast<std::int32_t>(image.width) - 4 ||
                            y >= static_cast<std::int32_t>(image.height) - 4) {
                            continue;
                        }
                        const auto pixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                            static_cast<std::uint32_t>(x);
                        if (considered[pixel] != 0) {
                            continue;
                        }
                        considered[pixel] = 1;

                        std::vector<float> targetSamples[3];
                        std::vector<float> targetLumaSamples;
                        std::vector<float> originalLumaSamples;
                        targetLumaSamples.reserve(30);
                        originalLumaSamples.reserve(30);
                        for (auto& samples : targetSamples) {
                            samples.reserve(30);
                        }
                        for (const float offset : {9.0F, 15.0F, 24.0F, 38.0F, 54.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-6.0F, 0.0F, 6.0F}) {
                                    const auto sx = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(x) + normalX * offset * side + unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(y) + normalY * offset * side + unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    if (likelyArtificialColorSample(image, samplePixel)) {
                                        continue;
                                    }
                                    const auto sampleBase = samplePixel * output.channels;
                                    const float red = output.pixels[sampleBase];
                                    const float green = output.pixels[sampleBase + 1];
                                    const float blue = output.pixels[sampleBase + 2];
                                    targetSamples[0].push_back(red);
                                    targetSamples[1].push_back(green);
                                    targetSamples[2].push_back(blue);
                                    targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                    originalLumaSamples.push_back(luminance[samplePixel]);
                                }
                            }
                        }
                        if (targetLumaSamples.size() < 10 || originalLumaSamples.size() < 10) {
                            continue;
                        }

                        const auto base = pixel * output.channels;
                        const float currentRed = output.pixels[base];
                        const float currentGreen = output.pixels[base + 1];
                        const float currentBlue = output.pixels[base + 2];
                        const float currentLuma =
                            currentRed * 0.2126F + currentGreen * 0.7152F + currentBlue * 0.0722F;
                        const float currentMaximum = std::max({currentRed, currentGreen, currentBlue});
                        const float currentMinimum = std::min({currentRed, currentGreen, currentBlue});
                        const float currentChroma = currentMaximum - currentMinimum;
                        const float targetLuma = percentileValue(targetLumaSamples, horizonTrail ? 0.30F : 0.24F);
                        const float originalBackground = percentileValue(originalLumaSamples, horizonTrail ? 0.42F : 0.36F);
                        const float residual = currentLuma - targetLuma;
                        const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                                   image.pixels[base + 2] * 0.0722F;
                        const float originalLumaExcess = originalLuma - originalBackground;
                        const float originalColorEvidence =
                            navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 1.15F +
                            std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.20F;
                        const auto outputLumaNearest = [&](float sampleX, float sampleY) {
                            const auto sampleXi = std::clamp(static_cast<std::int32_t>(std::lround(sampleX)),
                                                             0,
                                                             static_cast<std::int32_t>(image.width) - 1);
                            const auto sampleYi = std::clamp(static_cast<std::int32_t>(std::lround(sampleY)),
                                                             0,
                                                             static_cast<std::int32_t>(image.height) - 1);
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sampleYi)) * image.width +
                                static_cast<std::uint32_t>(sampleXi);
                            const auto sampleBase = samplePixel * output.channels;
                            return output.pixels[sampleBase] * 0.2126F +
                                   output.pixels[sampleBase + 1] * 0.7152F +
                                   output.pixels[sampleBase + 2] * 0.0722F;
                        };
                        const bool horizonLocalDot =
                            horizonTrail &&
                            ((currentLuma > outputLumaNearest(static_cast<float>(x) + unitX * 3.5F,
                                                              static_cast<float>(y) + unitY * 3.5F) + 0.00045F &&
                              currentLuma > outputLumaNearest(static_cast<float>(x) - unitX * 3.5F,
                                                              static_cast<float>(y) - unitY * 3.5F) + 0.00045F) ||
                             (originalLuma > luminanceNearest(luminance,
                                                              image.width,
                                                              image.height,
                                                              static_cast<float>(x) + unitX * 3.5F,
                                                              static_cast<float>(y) + unitY * 3.5F) + 0.00065F &&
                              originalLuma > luminanceNearest(luminance,
                                                              image.width,
                                                              image.height,
                                                              static_cast<float>(x) - unitX * 3.5F,
                                                              static_cast<float>(y) - unitY * 3.5F) + 0.00065F)) &&
                            (currentLuma > outputLumaNearest(static_cast<float>(x) + normalX * 2.0F,
                                                             static_cast<float>(y) + normalY * 2.0F) + 0.00015F ||
                             originalLuma > luminanceNearest(luminance,
                                                             image.width,
                                                             image.height,
                                                             static_cast<float>(x) + normalX * 2.0F,
                                                             static_cast<float>(y) + normalY * 2.0F) + 0.00020F) &&
                            (currentLuma > outputLumaNearest(static_cast<float>(x) - normalX * 2.0F,
                                                             static_cast<float>(y) - normalY * 2.0F) + 0.00015F ||
                             originalLuma > luminanceNearest(luminance,
                                                             image.width,
                                                             image.height,
                                                             static_cast<float>(x) - normalX * 2.0F,
                                                             static_cast<float>(y) - normalY * 2.0F) + 0.00020F);
                        const bool likelyStar =
                            horizonTrail
                                ? (currentMaximum > 0.94F && currentChroma < 0.055F &&
                                   originalColorEvidence < 0.0010F && !horizonLocalDot)
                                : (currentMaximum > 0.70F && currentChroma < 0.090F &&
                                   originalColorEvidence < 0.0032F);
                        if (likelyStar) {
                            continue;
                        }

                        const float minimumResidual = horizonTrail ? 0.0018F : 0.0058F;
                        const float minimumChroma = horizonTrail ? 0.010F : 0.026F;
                        const bool coloredPoint =
                            currentChroma >= minimumChroma || originalColorEvidence >= (horizonTrail ? 0.0012F : 0.0020F);
                        const bool paleHorizonDot =
                            horizonTrail && currentMaximum < 0.96F &&
                            ((residual > 0.0012F || originalLumaExcess > 0.0020F) &&
                             (currentChroma > 0.006F || originalColorEvidence > 0.0006F || horizonLocalDot));
                        if ((residual <= minimumResidual || !coloredPoint) && !paleHorizonDot) {
                            continue;
                        }

                        const float candidateScore =
                            std::max({0.0F, residual, horizonTrail ? originalLumaExcess * 0.82F : 0.0F}) +
                            currentChroma * (horizonTrail ? 0.030F : 0.045F) +
                            originalColorEvidence * 0.24F;
                        ResidualCluster* cluster = nullptr;
                        for (auto& existing : clusters) {
                            const float dx = existing.x - static_cast<float>(x);
                            const float dy = existing.y - static_cast<float>(y);
                            if (dx * dx + dy * dy <= (horizonTrail ? 24.0F : 42.0F)) {
                                cluster = &existing;
                                break;
                            }
                        }
                        if (cluster == nullptr) {
                            clusters.push_back({});
                            cluster = &clusters.back();
                            cluster->x = static_cast<float>(x);
                            cluster->y = static_cast<float>(y);
                            cluster->unitX = unitX;
                            cluster->unitY = unitY;
                        }
                        const float previousScore = cluster->score;
                        const float weight = 1.0F + candidateScore * 34.0F;
                        cluster->score += candidateScore;
                        cluster->count += 1;
                        const float blend = weight / std::max(0.0001F, previousScore + weight);
                        cluster->x = cluster->x * (1.0F - blend) + static_cast<float>(x) * blend;
                        cluster->y = cluster->y * (1.0F - blend) + static_cast<float>(y) * blend;
                        cluster->unitX = unitX;
                        cluster->unitY = unitY;
                        cluster->maxResidual = std::max(cluster->maxResidual, residual);
                        cluster->maxOriginalLumaExcess = std::max(cluster->maxOriginalLumaExcess, originalLumaExcess);
                        cluster->maxChroma = std::max(cluster->maxChroma, currentChroma);
                        cluster->maxOriginalColor = std::max(cluster->maxOriginalColor, originalColorEvidence);
                    }
                }
            }

            for (const auto& cluster : clusters) {
                if (cluster.count < (horizonTrail ? 1U : 2U) && cluster.score < (horizonTrail ? 0.0050F : 0.0140F)) {
                    continue;
                }
                const float normalX = -cluster.unitY;
                const float normalY = cluster.unitX;
                const float alongRadius =
                    std::clamp((horizonTrail ? 2.6F : 3.0F) +
                                   std::max(cluster.maxResidual, cluster.maxOriginalLumaExcess * 0.82F) * 42.0F,
                               2.7F,
                               horizonTrail ? 6.2F : 6.2F);
                const float normalRadius =
                    std::clamp((horizonTrail ? 1.25F : 1.50F) + cluster.maxChroma * 2.2F, 1.20F, horizonTrail ? 1.95F : 2.65F);
                const float bounds = std::ceil(std::max(alongRadius, normalRadius) + 2.0F);

                std::vector<float> targetSamples[3];
                for (auto& samples : targetSamples) {
                    samples.reserve(36);
                }
                const std::initializer_list<float> finalSampleOffsets =
                    horizonTrail ? std::initializer_list<float>{16.0F, 28.0F, 44.0F, 66.0F, 92.0F}
                                 : std::initializer_list<float>{9.0F, 15.0F, 24.0F, 38.0F, 54.0F};
                for (const float offset : finalSampleOffsets) {
                    for (const float side : {-1.0F, 1.0F}) {
                        for (const float shift : {-7.0F, 0.0F, 7.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(cluster.x + normalX * offset * side + cluster.unitX * shift));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(cluster.y + normalY * offset * side + cluster.unitY * shift));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyArtificialColorSample(image, samplePixel) ||
                                likelyNeutralBrightStarSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            targetSamples[0].push_back(output.pixels[sampleBase]);
                            targetSamples[1].push_back(output.pixels[sampleBase + 1]);
                            targetSamples[2].push_back(output.pixels[sampleBase + 2]);
                        }
                    }
                }
                if (targetSamples[0].size() < 10 || targetSamples[1].size() < 10 || targetSamples[2].size() < 10) {
                    continue;
                }
                float target[3] = {0.0F, 0.0F, 0.0F};
                for (std::uint16_t channel = 0; channel < 3; ++channel) {
                    target[channel] = percentileValue(targetSamples[channel], horizonTrail ? 0.12F : 0.20F);
                }
                const float baseStrength =
                    horizonTrail
                        ? std::clamp((std::max(cluster.maxResidual, cluster.maxOriginalLumaExcess * 0.88F) +
                                      cluster.maxChroma * 0.040F +
                                      cluster.maxOriginalColor * 0.28F) /
                                         0.011F,
                                     0.56F,
                                     0.97F)
                        : std::clamp((cluster.maxResidual + cluster.maxChroma * 0.035F +
                                      cluster.maxOriginalColor * 0.22F) /
                                         0.028F,
                                     0.24F,
                                     0.72F);

                for (std::int32_t y = static_cast<std::int32_t>(std::floor(cluster.y - bounds));
                     y <= static_cast<std::int32_t>(std::ceil(cluster.y + bounds)); ++y) {
                    for (std::int32_t x = static_cast<std::int32_t>(std::floor(cluster.x - bounds));
                         x <= static_cast<std::int32_t>(std::ceil(cluster.x + bounds)); ++x) {
                        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                            y >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const float dx = static_cast<float>(x) - cluster.x;
                        const float dy = static_cast<float>(y) - cluster.y;
                        const float along = dx * cluster.unitX + dy * cluster.unitY;
                        const float normal = dx * normalX + dy * normalY;
                        const float ellipse = std::sqrt((along / alongRadius) * (along / alongRadius) +
                                                        (normal / normalRadius) * (normal / normalRadius));
                        if (ellipse > 1.0F) {
                            continue;
                        }
                        const auto pixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                            static_cast<std::uint32_t>(x);
                        if (likelyNeutralBrightStarSample(image, pixel)) {
                            continue;
                        }
                        const auto base = pixel * output.channels;
                        const float strength = std::clamp(baseStrength * (1.0F - ellipse * (horizonTrail ? 0.42F : 0.52F)),
                                                          0.0F,
                                                          horizonTrail ? 0.94F : 0.72F);
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            dottedResidualPatched.pixels[base + channel] =
                                dottedResidualPatched.pixels[base + channel] * (1.0F - strength) +
                                target[channel] * strength;
                        }
                    }
                }
            }
        }
        output = std::move(dottedResidualPatched);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer horizonNeedlePatched = output;
        std::vector<std::uint8_t> touched(image.pixelCount(), 0);
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || centerRatio > 0.970F || trail.path.size() < 2) {
                continue;
            }

            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); ++step) {
                    const float centerPointX = start.x + unitX * static_cast<float>(step);
                    const float centerPointY = start.y + unitY * static_cast<float>(step);
                    for (const float normalOffset : {-1.2F, -0.6F, 0.0F, 0.6F, 1.2F}) {
                        const auto centerX =
                            static_cast<std::int32_t>(std::lround(centerPointX + normalX * normalOffset));
                        const auto centerY =
                            static_cast<std::int32_t>(std::lround(centerPointY + normalY * normalOffset));
                        if (centerX < 4 || centerY < 4 || centerX >= static_cast<std::int32_t>(image.width) - 4 ||
                            centerY >= static_cast<std::int32_t>(image.height) - 4) {
                            continue;
                        }

                        const auto centerPixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(centerY)) * image.width +
                            static_cast<std::uint32_t>(centerX);
                        if (touched[centerPixel] != 0) {
                            continue;
                        }
                        touched[centerPixel] = 1;

                        std::vector<float> outputLumaSamples;
                        std::vector<float> originalLumaSamples;
                        std::vector<float> targetSamples[3];
                        outputLumaSamples.reserve(30);
                        originalLumaSamples.reserve(30);
                        for (auto& samples : targetSamples) {
                            samples.reserve(30);
                        }
                        for (const float offset : {18.0F, 30.0F, 48.0F, 72.0F, 96.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-8.0F, 0.0F, 8.0F}) {
                                    const auto sx = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(centerX) + normalX * offset * side + unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(centerY) + normalY * offset * side + unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    if (likelyArtificialColorSample(image, samplePixel) ||
                                        likelyNeutralBrightStarSample(image, samplePixel)) {
                                        continue;
                                    }
                                    const auto sampleBase = samplePixel * output.channels;
                                    const float red = output.pixels[sampleBase];
                                    const float green = output.pixels[sampleBase + 1];
                                    const float blue = output.pixels[sampleBase + 2];
                                    targetSamples[0].push_back(red);
                                    targetSamples[1].push_back(green);
                                    targetSamples[2].push_back(blue);
                                    outputLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                    originalLumaSamples.push_back(luminance[samplePixel]);
                                }
                            }
                        }
                        if (outputLumaSamples.size() < 10 || originalLumaSamples.size() < 10) {
                            continue;
                        }

                        const auto base = centerPixel * output.channels;
                        const float currentLuma = output.pixels[base] * 0.2126F +
                                                  output.pixels[base + 1] * 0.7152F +
                                                  output.pixels[base + 2] * 0.0722F;
                        const float originalLuma = image.pixels[base] * 0.2126F +
                                                   image.pixels[base + 1] * 0.7152F +
                                                   image.pixels[base + 2] * 0.0722F;
                        const float outputBackground = percentileValue(outputLumaSamples, 0.16F);
                        const float originalBackground = percentileValue(originalLumaSamples, 0.42F);
                        const float residual = currentLuma - outputBackground;
                        const float originalExcess = originalLuma - originalBackground;
                        const float originalColorEvidence =
                            navigationLightScoreAt(image, centerPixel) + warmExcessAt(image, centerPixel) * 1.10F +
                            std::sqrt(std::max(0.0F, colorVarianceAt(image, centerPixel))) * 0.18F;
                        const auto outputLumaNearest = [&](float sampleX, float sampleY) {
                            const auto sampleXi = std::clamp(static_cast<std::int32_t>(std::lround(sampleX)),
                                                             0,
                                                             static_cast<std::int32_t>(image.width) - 1);
                            const auto sampleYi = std::clamp(static_cast<std::int32_t>(std::lround(sampleY)),
                                                             0,
                                                             static_cast<std::int32_t>(image.height) - 1);
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sampleYi)) * image.width +
                                static_cast<std::uint32_t>(sampleXi);
                            const auto sampleBase = samplePixel * output.channels;
                            return output.pixels[sampleBase] * 0.2126F +
                                   output.pixels[sampleBase + 1] * 0.7152F +
                                   output.pixels[sampleBase + 2] * 0.0722F;
                        };
                        const bool originalPeak =
                            originalLuma >
                                std::max(luminanceNearest(luminance,
                                                          image.width,
                                                          image.height,
                                                          static_cast<float>(centerX) + unitX * 3.6F,
                                                          static_cast<float>(centerY) + unitY * 3.6F),
                                         luminanceNearest(luminance,
                                                          image.width,
                                                          image.height,
                                                          static_cast<float>(centerX) - unitX * 3.6F,
                                                          static_cast<float>(centerY) - unitY * 3.6F)) +
                                    0.00042F &&
                            originalLuma >
                                std::max(luminanceNearest(luminance,
                                                          image.width,
                                                          image.height,
                                                          static_cast<float>(centerX) + normalX * 2.2F,
                                                          static_cast<float>(centerY) + normalY * 2.2F),
                                         luminanceNearest(luminance,
                                                          image.width,
                                                          image.height,
                                                          static_cast<float>(centerX) - normalX * 2.2F,
                                                          static_cast<float>(centerY) - normalY * 2.2F)) +
                                    0.00018F;
                        const bool currentPeak =
                            currentLuma >
                                std::max(outputLumaNearest(static_cast<float>(centerX) + unitX * 3.6F,
                                                           static_cast<float>(centerY) + unitY * 3.6F),
                                         outputLumaNearest(static_cast<float>(centerX) - unitX * 3.6F,
                                                           static_cast<float>(centerY) - unitY * 3.6F)) +
                                    0.00030F &&
                            currentLuma >
                                std::max(outputLumaNearest(static_cast<float>(centerX) + normalX * 2.2F,
                                                           static_cast<float>(centerY) + normalY * 2.2F),
                                         outputLumaNearest(static_cast<float>(centerX) - normalX * 2.2F,
                                                           static_cast<float>(centerY) - normalY * 2.2F)) +
                                    0.00012F;
                        const bool needlePeak = originalPeak || currentPeak || originalColorEvidence > 0.0014F;
                        if (!needlePeak) {
                            continue;
                        }
                        if (residual < 0.0008F && originalExcess < 0.0018F && originalColorEvidence < 0.0005F) {
                            continue;
                        }

                        float target[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = percentileValue(targetSamples[channel], 0.17F);
                        }
                        const float baseStrength = std::clamp(
                            (std::max(residual, originalExcess * 0.82F) + originalColorEvidence * 0.22F) / 0.010F,
                            0.36F,
                            0.78F);

                        for (std::int32_t y = centerY - 2; y <= centerY + 2; ++y) {
                            for (std::int32_t x = centerX - 4; x <= centerX + 4; ++x) {
                                if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                                    y >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const float dx = static_cast<float>(x - centerX);
                                const float dy = static_cast<float>(y - centerY);
                                const float along = dx * unitX + dy * unitY;
                                const float normal = dx * normalX + dy * normalY;
                                const float ellipse =
                                    std::sqrt((along / 2.9F) * (along / 2.9F) +
                                              (normal / 1.05F) * (normal / 1.05F));
                                if (ellipse > 1.0F) {
                                    continue;
                                }
                                const auto pixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                    static_cast<std::uint32_t>(x);
                                if (likelyNeutralBrightStarSample(image, pixel)) {
                                    continue;
                                }
                                const auto patchBase = pixel * output.channels;
                                const float strength = std::clamp(baseStrength * (1.0F - ellipse * 0.58F), 0.0F, 0.78F);
                                for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                    horizonNeedlePatched.pixels[patchBase + channel] =
                                        horizonNeedlePatched.pixels[patchBase + channel] * (1.0F - strength) +
                                        target[channel] * strength;
                                }
                            }
                        }
                    }
                }
            }
        }
        output = std::move(horizonNeedlePatched);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer horizonTextureBlended = output;
        std::vector<std::uint8_t> touched(image.pixelCount(), 0);
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || centerRatio > 0.970F || trail.path.size() < 2) {
                continue;
            }

            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); ++step) {
                    const float centerPointX = start.x + unitX * static_cast<float>(step);
                    const float centerPointY = start.y + unitY * static_cast<float>(step);
                    for (const float normalOffset : {-1.10F, -0.55F, 0.0F, 0.55F, 1.10F}) {
                        const auto centerX =
                            static_cast<std::int32_t>(std::lround(centerPointX + normalX * normalOffset));
                        const auto centerY =
                            static_cast<std::int32_t>(std::lround(centerPointY + normalY * normalOffset));
                        if (centerX < 4 || centerY < 4 || centerX >= static_cast<std::int32_t>(image.width) - 4 ||
                            centerY >= static_cast<std::int32_t>(image.height) - 4) {
                            continue;
                        }
                        const auto centerPixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(centerY)) * image.width +
                            static_cast<std::uint32_t>(centerX);
                        if (touched[centerPixel] != 0) {
                            continue;
                        }
                        touched[centerPixel] = 1;

                        std::vector<float> channelSamples[3];
                        std::vector<float> lumaSamples;
                        for (auto& samples : channelSamples) {
                            samples.reserve(30);
                        }
                        lumaSamples.reserve(30);
                        for (const float offset : {20.0F, 34.0F, 54.0F, 82.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-9.0F, 0.0F, 9.0F}) {
                                    const auto sx = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(centerX) + normalX * offset * side + unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(centerY) + normalY * offset * side + unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    if (likelyArtificialColorSample(image, samplePixel) ||
                                        likelyNeutralBrightStarSample(image, samplePixel)) {
                                        continue;
                                    }
                                    const auto sampleBase = samplePixel * output.channels;
                                    const float red = output.pixels[sampleBase];
                                    const float green = output.pixels[sampleBase + 1];
                                    const float blue = output.pixels[sampleBase + 2];
                                    channelSamples[0].push_back(red);
                                    channelSamples[1].push_back(green);
                                    channelSamples[2].push_back(blue);
                                    lumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                }
                            }
                        }
                        if (lumaSamples.size() < 10 || channelSamples[0].size() < 10) {
                            continue;
                        }

                        const auto base = centerPixel * output.channels;
                        const float currentLuma = output.pixels[base] * 0.2126F +
                                                  output.pixels[base + 1] * 0.7152F +
                                                  output.pixels[base + 2] * 0.0722F;
                        const float targetLuma = percentileValue(lumaSamples, 0.70F);
                        const float lumaDelta = targetLuma - currentLuma;
                        if (lumaDelta < 0.00032F) {
                            continue;
                        }

                        float target[3] = {0.0F, 0.0F, 0.0F};
                        float channelVariance[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = percentileValue(channelSamples[channel], 0.70F);
                            const float meanSample =
                                std::accumulate(channelSamples[channel].begin(), channelSamples[channel].end(), 0.0F) /
                                static_cast<float>(channelSamples[channel].size());
                            for (const float sample : channelSamples[channel]) {
                                const float delta = sample - meanSample;
                                channelVariance[channel] += delta * delta;
                            }
                            channelVariance[channel] /=
                                static_cast<float>(std::max<std::size_t>(1, channelSamples[channel].size()));
                        }

                        for (std::int32_t y = centerY - 2; y <= centerY + 2; ++y) {
                            for (std::int32_t x = centerX - 4; x <= centerX + 4; ++x) {
                                if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                                    y >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const float dx = static_cast<float>(x - centerX);
                                const float dy = static_cast<float>(y - centerY);
                                const float along = dx * unitX + dy * unitY;
                                const float normal = dx * normalX + dy * normalY;
                                const float ellipse =
                                    std::sqrt((along / 3.10F) * (along / 3.10F) +
                                              (normal / 1.05F) * (normal / 1.05F));
                                if (ellipse > 1.0F) {
                                    continue;
                                }
                                const auto pixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                    static_cast<std::uint32_t>(x);
                                if (likelyNeutralBrightStarSample(image, pixel)) {
                                    continue;
                                }
                                const auto patchBase = pixel * output.channels;
                                const float strength =
                                    std::clamp((lumaDelta - 0.00012F) / 0.0065F, 0.10F, 0.64F) *
                                    (1.0F - ellipse * 0.58F);
                                for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                    const float texture =
                                        deterministicNoise(static_cast<std::uint32_t>(x),
                                                           static_cast<std::uint32_t>(y),
                                                           channel + 41U) *
                                        std::sqrt(std::max(0.0F, channelVariance[channel])) * 0.16F;
                                    horizonTextureBlended.pixels[patchBase + channel] =
                                        horizonTextureBlended.pixels[patchBase + channel] * (1.0F - strength) +
                                        std::clamp(target[channel] + texture, 0.0F, 1.0F) * strength;
                                }
                            }
                        }
                    }
                }
            }
        }
        output = std::move(horizonTextureBlended);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer horizonSeamSoftened = output;
        std::vector<std::uint8_t> touched(image.pixelCount(), 0);
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || centerRatio > 0.970F || trail.path.size() < 2) {
                continue;
            }

            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); ++step) {
                    const float centerPointX = start.x + unitX * static_cast<float>(step);
                    const float centerPointY = start.y + unitY * static_cast<float>(step);
                    for (const float normalOffset : {-3.5F, -2.5F, -1.5F, -0.5F, 0.5F, 1.5F, 2.5F, 3.5F}) {
                        const auto centerX =
                            static_cast<std::int32_t>(std::lround(centerPointX + normalX * normalOffset));
                        const auto centerY =
                            static_cast<std::int32_t>(std::lround(centerPointY + normalY * normalOffset));
                        if (centerX < 6 || centerY < 6 || centerX >= static_cast<std::int32_t>(image.width) - 6 ||
                            centerY >= static_cast<std::int32_t>(image.height) - 6) {
                            continue;
                        }
                        const auto centerPixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(centerY)) * image.width +
                            static_cast<std::uint32_t>(centerX);
                        if (touched[centerPixel] != 0 || likelyNeutralBrightStarSample(image, centerPixel)) {
                            continue;
                        }
                        touched[centerPixel] = 1;

                        std::vector<float> channelSamples[3];
                        std::vector<float> lumaSamples;
                        for (auto& samples : channelSamples) {
                            samples.reserve(24);
                        }
                        lumaSamples.reserve(24);
                        for (const float offset : {8.0F, 13.0F, 20.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-2.0F, 0.0F, 2.0F}) {
                                    const auto sx = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(centerX) + normalX * offset * side + unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(centerY) + normalY * offset * side + unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    if (likelyArtificialColorSample(image, samplePixel) ||
                                        likelyNeutralBrightStarSample(image, samplePixel)) {
                                        continue;
                                    }
                                    const auto sampleBase = samplePixel * output.channels;
                                    const float red = output.pixels[sampleBase];
                                    const float green = output.pixels[sampleBase + 1];
                                    const float blue = output.pixels[sampleBase + 2];
                                    channelSamples[0].push_back(red);
                                    channelSamples[1].push_back(green);
                                    channelSamples[2].push_back(blue);
                                    lumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                }
                            }
                        }
                        if (lumaSamples.size() < 8 || channelSamples[0].size() < 8) {
                            continue;
                        }

                        const auto base = centerPixel * output.channels;
                        const float currentLuma = output.pixels[base] * 0.2126F +
                                                  output.pixels[base + 1] * 0.7152F +
                                                  output.pixels[base + 2] * 0.0722F;
                        const float targetLuma = percentileValue(lumaSamples, 0.50F);
                        const float lumaDelta = targetLuma - currentLuma;
                        if (std::fabs(lumaDelta) < 0.00055F) {
                            continue;
                        }

                        float target[3] = {0.0F, 0.0F, 0.0F};
                        float channelVariance[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = percentileValue(channelSamples[channel], 0.50F);
                            const float meanSample =
                                std::accumulate(channelSamples[channel].begin(), channelSamples[channel].end(), 0.0F) /
                                static_cast<float>(channelSamples[channel].size());
                            for (const float sample : channelSamples[channel]) {
                                const float delta = sample - meanSample;
                                channelVariance[channel] += delta * delta;
                            }
                            channelVariance[channel] /=
                                static_cast<float>(std::max<std::size_t>(1, channelSamples[channel].size()));
                        }

                        const float offsetFade =
                            std::clamp(1.0F - std::max(0.0F, std::fabs(normalOffset) - 1.25F) / 3.25F, 0.28F, 1.0F);
                        const float strength =
                            std::clamp((std::fabs(lumaDelta) - 0.00035F) / 0.012F, 0.04F, 0.26F) * offsetFade;
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            const float texture =
                                deterministicNoise(static_cast<std::uint32_t>(centerX),
                                                   static_cast<std::uint32_t>(centerY),
                                                   channel + 83U) *
                                std::sqrt(std::max(0.0F, channelVariance[channel])) * 0.12F;
                            horizonSeamSoftened.pixels[base + channel] =
                                horizonSeamSoftened.pixels[base + channel] * (1.0F - strength) +
                                std::clamp(target[channel] + texture, 0.0F, 1.0F) * strength;
                        }
                    }
                }
            }
        }
        output = std::move(horizonSeamSoftened);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.83);

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        for (std::uint8_t profilePass = 0; profilePass < 3; ++profilePass) {
            ImageBuffer horizonProfileBalanced = output;
            std::vector<std::uint8_t> touched(image.pixelCount(), 0);
            for (const auto& trail : removedDottedDroneTrails) {
                const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
                if (centerRatio < 0.90F || centerRatio > 0.970F || trail.path.size() < 2) {
                    continue;
                }

                for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                    const auto& start = trail.path[segment - 1];
                    const auto& end = trail.path[segment];
                    const float vx = end.x - start.x;
                    const float vy = end.y - start.y;
                    const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                    const float unitX = vx / segmentLength;
                    const float unitY = vy / segmentLength;
                    const float normalX = -unitY;
                    const float normalY = unitX;

                    for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); ++step) {
                        const float centerPointX = start.x + unitX * static_cast<float>(step);
                        const float centerPointY = start.y + unitY * static_cast<float>(step);
                        for (std::int32_t offsetIndex = -5; offsetIndex <= 5; ++offsetIndex) {
                            const float normalOffset = static_cast<float>(offsetIndex);
                            const auto centerX =
                                static_cast<std::int32_t>(std::lround(centerPointX + normalX * normalOffset));
                            const auto centerY =
                                static_cast<std::int32_t>(std::lround(centerPointY + normalY * normalOffset));
                            if (centerX < 6 || centerY < 6 || centerX >= static_cast<std::int32_t>(image.width) - 6 ||
                                centerY >= static_cast<std::int32_t>(image.height) - 6) {
                                continue;
                            }
                            const auto centerPixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(centerY)) * image.width +
                                static_cast<std::uint32_t>(centerX);
                            if (touched[centerPixel] != 0 || likelyNeutralBrightStarSample(image, centerPixel)) {
                                continue;
                            }
                            touched[centerPixel] = 1;

                            std::vector<float> sideLumaSamples;
                            sideLumaSamples.reserve(18);
                            for (const float sideOffset : {-44.0F, -32.0F, 32.0F, 44.0F}) {
                                for (const float shift : {-4.0F, 0.0F, 4.0F}) {
                                    const auto sx = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(centerX) + normalX * sideOffset + unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(std::lround(
                                        static_cast<float>(centerY) + normalY * sideOffset + unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    if (likelyArtificialColorSample(image, samplePixel) ||
                                        likelyNeutralBrightStarSample(image, samplePixel)) {
                                        continue;
                                    }
                                    const auto sampleBase = samplePixel * output.channels;
                                    const float red = output.pixels[sampleBase];
                                    const float green = output.pixels[sampleBase + 1];
                                    const float blue = output.pixels[sampleBase + 2];
                                    sideLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                }
                            }
                            if (sideLumaSamples.size() < 6) {
                                continue;
                            }

                            const auto base = centerPixel * output.channels;
                            const float currentLuma = output.pixels[base] * 0.2126F +
                                                      output.pixels[base + 1] * 0.7152F +
                                                      output.pixels[base + 2] * 0.0722F;
                            const float targetLuma = percentileValue(sideLumaSamples, 0.50F);
                            const float lumaDelta = targetLuma - currentLuma;
                            if (std::fabs(lumaDelta) < 0.00045F) {
                                continue;
                            }

                            const float centerFade = std::clamp(
                                1.0F - std::max(0.0F, std::fabs(normalOffset) - 1.0F) / 5.0F, 0.35F, 1.0F);
                            const bool liftingDarkSeam = lumaDelta > 0.0F;
                            const float strength =
                                (liftingDarkSeam
                                     ? std::clamp((lumaDelta - 0.00020F) / 0.0045F, 0.14F, 0.64F)
                                     : std::clamp((std::fabs(lumaDelta) - 0.00025F) / 0.0065F, 0.07F, 0.48F)) *
                                centerFade;
                            const float correction =
                                liftingDarkSeam ? std::clamp(lumaDelta * strength, 0.0F, 0.0120F)
                                                : std::clamp(lumaDelta * strength, -0.0090F, 0.0F);
                            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                horizonProfileBalanced.pixels[base + channel] =
                                    std::clamp(horizonProfileBalanced.pixels[base + channel] + correction, 0.0F, 1.0F);
                            }
                        }
                    }
                }
            }
            output = std::move(horizonProfileBalanced);
        }
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        for (std::uint8_t trenchPass = 0; trenchPass < 2; ++trenchPass) {
            ImageBuffer trenchFilled = output;
            for (const auto& trail : removedDottedDroneTrails) {
                const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
                if (centerRatio < 0.80F || centerRatio > 0.970F || trail.path.size() < 2) {
                    continue;
                }

                const bool lowHorizonTrail = centerRatio >= 0.90F;
                const float trenchRadius = lowHorizonTrail ? std::clamp(trail.width * 2.30F + 5.30F, 8.80F, 11.20F)
                                                           : std::clamp(trail.width * 2.10F + 5.10F, 8.40F, 10.60F);
                float minTrailX = std::min(trail.x1, trail.x2);
                float maxTrailX = std::max(trail.x1, trail.x2);
                float minTrailY = std::min(trail.y1, trail.y2);
                float maxTrailY = std::max(trail.y1, trail.y2);
                for (const auto& point : trail.path) {
                    minTrailX = std::min(minTrailX, point.x);
                    maxTrailX = std::max(maxTrailX, point.x);
                    minTrailY = std::min(minTrailY, point.y);
                    maxTrailY = std::max(maxTrailY, point.y);
                }

                const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - trenchRadius - 3.0F));
                const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + trenchRadius + 3.0F));
                const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - trenchRadius - 3.0F));
                const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + trenchRadius + 3.0F));
                for (std::int32_t y = minY; y <= maxY; ++y) {
                    for (std::int32_t x = minX; x <= maxX; ++x) {
                        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                            y >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }

                        const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                        if (distance.distance > trenchRadius) {
                            continue;
                        }

                        const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                           static_cast<std::uint32_t>(x);
                        if (likelyNeutralBrightStarSample(image, pixel)) {
                            continue;
                        }

                        const float normalX = -distance.unitY;
                        const float normalY = distance.unitX;
                        std::vector<float> targetSamples[3];
                        std::vector<float> targetLumaSamples;
                        for (auto& channelSamples : targetSamples) {
                            channelSamples.reserve(14);
                        }
                        targetLumaSamples.reserve(14);
                        for (const float offset : {14.0F, 21.0F, 32.0F, 46.0F, 64.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * offset * side));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * offset * side));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance =
                                    distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= trenchRadius + 2.0F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * image.channels;
                                const float red = image.pixels[sampleBase];
                                const float green = image.pixels[sampleBase + 1];
                                const float blue = image.pixels[sampleBase + 2];
                                targetSamples[0].push_back(red);
                                targetSamples[1].push_back(green);
                                targetSamples[2].push_back(blue);
                                targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                        if (targetLumaSamples.size() < 6) {
                            continue;
                        }

                        float target[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = percentileValue(targetSamples[channel], lowHorizonTrail ? 0.54F : 0.52F);
                        }
                        const float targetLuma = percentileValue(targetLumaSamples, lowHorizonTrail ? 0.54F : 0.52F);
                        const auto base = pixel * output.channels;
                        const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                                  output.pixels[base + 2] * 0.0722F;
                        const float lumaDeficit = targetLuma - currentLuma;
                        if (lumaDeficit <= (lowHorizonTrail ? 0.0007F : 0.0008F)) {
                            continue;
                        }

                        const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                                   image.pixels[base + 2] * 0.0722F;
                        const float originalDeficit = targetLuma - originalLuma;
                        const bool maskedDottedPixel =
                            dottedDroneMask[pixel] != 0 && trailMaskMetadataAt(maskMetadata, pixel).alpha > 0.0F;
                        if (!maskedDottedPixel && originalDeficit > lumaDeficit + 0.010F) {
                            continue;
                        }

                        float channelDeficit = 0.0F;
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            channelDeficit = std::max(channelDeficit, target[channel] - output.pixels[base + channel]);
                        }
                        const float evidence = std::max(lumaDeficit, channelDeficit * 0.88F);
                        const float distanceWeight =
                            1.0F - std::clamp(distance.distance / std::max(0.1F, trenchRadius), 0.0F, 1.0F) * 0.44F;
                        const float strength = std::clamp((evidence - (lowHorizonTrail ? 0.0007F : 0.0009F)) /
                                                              (lowHorizonTrail ? 0.009F : 0.010F),
                                                          0.16F, lowHorizonTrail ? 0.78F : 0.76F) *
                                               distanceWeight;
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            trenchFilled.pixels[base + channel] =
                                output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                        }
                    }
                }
            }
            output = std::move(trenchFilled);
        }
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer profilePolished = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || centerRatio > 0.970F || trail.path.size() < 2) {
                continue;
            }

            const bool lowHorizonTrail = centerRatio >= 0.90F;
            const float polishRadius = lowHorizonTrail ? std::clamp(trail.width * 1.85F + 4.80F, 7.70F, 9.80F)
                                                       : std::clamp(trail.width * 1.70F + 4.40F, 7.20F, 9.20F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - polishRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + polishRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - polishRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + polishRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > polishRadius) {
                        continue;
                    }

                    const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                       static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(18);
                    }
                    targetLumaSamples.reserve(18);
                    for (const float offset : {12.0F, 18.0F, 27.0F, 40.0F, 58.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            for (const float shift : {-5.0F, 0.0F, 5.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * offset * side + distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * offset * side + distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance =
                                    distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= polishRadius + 1.5F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * image.channels;
                                const float red = image.pixels[sampleBase];
                                const float green = image.pixels[sampleBase + 1];
                                const float blue = image.pixels[sampleBase + 2];
                                targetSamples[0].push_back(red);
                                targetSamples[1].push_back(green);
                                targetSamples[2].push_back(blue);
                                targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (targetLumaSamples.size() < 8) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    float channelVariance[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = percentileValue(targetSamples[channel], 0.50F);
                        const float meanSample =
                            std::accumulate(targetSamples[channel].begin(), targetSamples[channel].end(), 0.0F) /
                            static_cast<float>(targetSamples[channel].size());
                        for (const float sample : targetSamples[channel]) {
                            const float delta = sample - meanSample;
                            channelVariance[channel] += delta * delta;
                        }
                        channelVariance[channel] /= static_cast<float>(targetSamples[channel].size());
                    }
                    const float targetLuma = percentileValue(targetLumaSamples, 0.50F);
                    const auto base = pixel * output.channels;
                    const float current[3] = {output.pixels[base], output.pixels[base + 1], output.pixels[base + 2]};
                    const float currentLuma = current[0] * 0.2126F + current[1] * 0.7152F + current[2] * 0.0722F;
                    const float lumaDelta = targetLuma - currentLuma;
                    float channelDelta = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        channelDelta = std::max(channelDelta, std::fabs(target[channel] - current[channel]));
                    }
                    const float evidence = std::max(std::fabs(lumaDelta), channelDelta * 0.62F);
                    if (evidence <= (lowHorizonTrail ? 0.0011F : 0.0013F)) {
                        continue;
                    }

                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    const bool maskedDottedPixel =
                        dottedDroneMask[pixel] != 0 && trailMaskMetadataAt(maskMetadata, pixel).alpha > 0.0F;
                    if (!maskedDottedPixel && lumaDelta > 0.0F && targetLuma - originalLuma > lumaDelta + 0.009F) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, polishRadius), 0.0F, 1.0F) * 0.42F;
                    const float strength =
                        std::clamp((evidence - (lowHorizonTrail ? 0.0008F : 0.0010F)) /
                                       (lowHorizonTrail ? 0.014F : 0.016F),
                                   0.08F,
                                   lowHorizonTrail ? 0.46F : 0.42F) *
                        distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float texture =
                            deterministicNoise(static_cast<std::uint32_t>(x),
                                               static_cast<std::uint32_t>(y),
                                               channel + 157U) *
                            std::sqrt(std::max(0.0F, channelVariance[channel])) * (lowHorizonTrail ? 0.10F : 0.085F);
                        const float texturedTarget = std::clamp(target[channel] + texture, 0.0F, 1.0F);
                        profilePolished.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + texturedTarget * strength;
                    }
                }
            }
        }
        output = std::move(profilePolished);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.85);

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer grainRestored = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || centerRatio > 0.970F || trail.path.size() < 2) {
                continue;
            }

            const bool lowHorizonTrail = centerRatio >= 0.90F;
            const float grainRadius = lowHorizonTrail ? std::clamp(trail.width * 1.70F + 4.40F, 7.10F, 9.20F)
                                                      : std::clamp(trail.width * 1.55F + 4.00F, 6.60F, 8.60F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - grainRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + grainRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - grainRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + grainRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > grainRadius) {
                        continue;
                    }

                    const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                       static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> sideLumaSamples;
                    sideLumaSamples.reserve(24);
                    for (const float offset : {12.0F, 18.0F, 27.0F, 40.0F, 58.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            for (const float shift : {-5.0F, 0.0F, 5.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * offset * side + distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * offset * side + distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance =
                                    distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= grainRadius + 1.5F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * image.channels;
                                const float red = image.pixels[sampleBase];
                                const float green = image.pixels[sampleBase + 1];
                                const float blue = image.pixels[sampleBase + 2];
                                sideLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (sideLumaSamples.size() < 8) {
                        continue;
                    }

                    const float meanLuma =
                        std::accumulate(sideLumaSamples.begin(), sideLumaSamples.end(), 0.0F) /
                        static_cast<float>(sideLumaSamples.size());
                    float varianceLuma = 0.0F;
                    for (const float sample : sideLumaSamples) {
                        const float delta = sample - meanLuma;
                        varianceLuma += delta * delta;
                    }
                    varianceLuma /= static_cast<float>(sideLumaSamples.size());
                    const float sideStdDev = std::sqrt(std::max(0.0F, varianceLuma));
                    if (sideStdDev < 0.006F) {
                        continue;
                    }

                    const auto base = pixel * output.channels;
                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, grainRadius), 0.0F, 1.0F) * 0.58F;
                    const float grain =
                        deterministicNoise(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), 211U) *
                        sideStdDev * (lowHorizonTrail ? 0.38F : 0.30F) * distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        grainRestored.pixels[base + channel] =
                            std::clamp(grainRestored.pixels[base + channel] + grain, 0.0F, 1.0F);
                    }
                }
            }
        }
        output = std::move(grainRestored);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer horizonResidualScrubbed = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || centerRatio > 0.970F || trail.path.size() < 2) {
                continue;
            }

            const float scrubRadius = std::clamp(trail.width * 0.58F + 1.15F, 2.10F, 2.85F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - scrubRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + scrubRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - scrubRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + scrubRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > scrubRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(24);
                    }
                    targetLumaSamples.reserve(24);
                    for (const float offset : {7.0F, 11.0F, 16.0F, 24.0F, 34.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            for (const float shift : {-2.0F, 0.0F, 2.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * offset * side + distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * offset * side + distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= scrubRadius + 1.6F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                const float red = output.pixels[sampleBase];
                                const float green = output.pixels[sampleBase + 1];
                                const float blue = output.pixels[sampleBase + 2];
                                targetSamples[0].push_back(red);
                                targetSamples[1].push_back(green);
                                targetSamples[2].push_back(blue);
                                targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (targetLumaSamples.size() < 8) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    float channelVariance[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(targetSamples[channel]);
                        const float meanSample =
                            std::accumulate(targetSamples[channel].begin(), targetSamples[channel].end(), 0.0F) /
                            static_cast<float>(targetSamples[channel].size());
                        for (const float sample : targetSamples[channel]) {
                            const float delta = sample - meanSample;
                            channelVariance[channel] += delta * delta;
                        }
                        channelVariance[channel] /= static_cast<float>(targetSamples[channel].size());
                    }
                    const float targetLuma = median(targetLumaSamples);

                    const auto base = pixel * output.channels;
                    const float current[3] = {output.pixels[base], output.pixels[base + 1], output.pixels[base + 2]};
                    const float currentLuma = current[0] * 0.2126F + current[1] * 0.7152F + current[2] * 0.0722F;
                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    float positiveChannelResidual = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        positiveChannelResidual = std::max(positiveChannelResidual, current[channel] - target[channel]);
                    }
                    const float currentExcess = currentLuma - targetLuma;
                    const float originalExcess = originalLuma - targetLuma;
                    const float pointEvidence =
                        localDottedPointScore(trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                    const float colorEvidence = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) +
                                                std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.18F;
                    const bool nearCore = distance.distance <= scrubRadius * 0.72F;
                    const bool brightResidual = currentExcess > 0.0032F || positiveChannelResidual > 0.0042F;
                    const bool originalPointResidual = nearCore && pointEvidence > 0.0024F && originalExcess > 0.0004F;
                    const bool warmResidual = nearCore && colorEvidence > 0.0020F && currentExcess > -0.0018F;
                    if (!brightResidual && !originalPointResidual && !warmResidual) {
                        continue;
                    }

                    const float evidence =
                        std::max({std::max(0.0F, currentExcess), positiveChannelResidual, pointEvidence * 0.78F,
                                  colorEvidence, originalExcess * 0.42F});
                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, scrubRadius), 0.0F, 1.0F) * 0.48F;
                    float strength = std::clamp((evidence - 0.0016F) / 0.013F, 0.0F, 0.62F) * distanceWeight;
                    if (brightResidual && nearCore) {
                        strength = std::max(strength, 0.26F * distanceWeight);
                    }
                    if (originalPointResidual || warmResidual) {
                        strength = std::max(strength, 0.34F * distanceWeight);
                    }
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float texture =
                            deterministicNoise(static_cast<std::uint32_t>(x),
                                               static_cast<std::uint32_t>(y),
                                               channel + 283U) *
                            std::sqrt(std::max(0.0F, channelVariance[channel])) * 0.08F;
                        const float texturedTarget = std::clamp(target[channel] + texture, 0.0F, 1.0F);
                        horizonResidualScrubbed.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + texturedTarget * strength;
                    }
                }
            }
        }
        output = std::move(horizonResidualScrubbed);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer horizonLumaCapped = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || centerRatio > 0.970F || trail.path.size() < 2) {
                continue;
            }

            const float capRadius = std::clamp(trail.width * 0.36F + 1.05F, 1.75F, 2.35F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - capRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + capRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - capRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + capRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > capRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> sideLumaSamples;
                    sideLumaSamples.reserve(10);
                    for (const float offset : {-66.0F, -56.0F, -44.0F, -34.0F, -24.0F, 24.0F, 34.0F, 44.0F, 56.0F,
                                               66.0F}) {
                        const auto sx = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(x) + normalX * offset));
                        const auto sy = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(y) + normalY * offset));
                        if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                            sy >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                        if (sampleDistance.distance <= capRadius + 3.0F) {
                            continue;
                        }
                        const auto samplePixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                            static_cast<std::uint32_t>(sx);
                        if (likelyArtificialColorSample(image, samplePixel) ||
                            likelyNeutralBrightStarSample(image, samplePixel)) {
                            continue;
                        }
                        const auto sampleBase = samplePixel * output.channels;
                        sideLumaSamples.push_back(output.pixels[sampleBase] * 0.2126F +
                                                  output.pixels[sampleBase + 1] * 0.7152F +
                                                  output.pixels[sampleBase + 2] * 0.0722F);
                    }
                    if (sideLumaSamples.size() < 5) {
                        continue;
                    }

                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                               output.pixels[base + 2] * 0.0722F;
                    const float backgroundLuma = percentileValue(sideLumaSamples, 0.46F);
                    const float excess = currentLuma - backgroundLuma;
                    if (excess <= 0.0048F) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, capRadius), 0.0F, 1.0F) * 0.52F;
                    const float reduction = std::min(0.014F, (excess - 0.0048F) * 0.40F) * distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        horizonLumaCapped.pixels[base + channel] =
                            std::clamp(output.pixels[base + channel] - reduction, 0.0F, 1.0F);
                    }
                }
            }
        }
        output = std::move(horizonLumaCapped);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer horizonContrastBalanced = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || centerRatio > 0.970F || trail.path.size() < 2) {
                continue;
            }

            const float balanceRadius = std::clamp(trail.width * 0.38F + 1.00F, 1.70F, 2.35F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - balanceRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + balanceRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - balanceRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + balanceRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > balanceRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> sideLumaSamples;
                    sideLumaSamples.reserve(12);
                    for (const float offset : {-72.0F, -60.0F, -48.0F, -36.0F, -26.0F, -18.0F, 18.0F, 26.0F, 36.0F,
                                               48.0F, 60.0F, 72.0F}) {
                        const auto sx = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(x) + normalX * offset));
                        const auto sy = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(y) + normalY * offset));
                        if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                            sy >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                        if (sampleDistance.distance <= balanceRadius + 3.0F) {
                            continue;
                        }
                        const auto samplePixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                            static_cast<std::uint32_t>(sx);
                        if (likelyArtificialColorSample(image, samplePixel) ||
                            likelyNeutralBrightStarSample(image, samplePixel)) {
                            continue;
                        }
                        const auto sampleBase = samplePixel * output.channels;
                        sideLumaSamples.push_back(output.pixels[sampleBase] * 0.2126F +
                                                  output.pixels[sampleBase + 1] * 0.7152F +
                                                  output.pixels[sampleBase + 2] * 0.0722F);
                    }
                    if (sideLumaSamples.size() < 6) {
                        continue;
                    }

                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                               output.pixels[base + 2] * 0.0722F;
                    const float backgroundLuma = median(sideLumaSamples);
                    const float delta = backgroundLuma - currentLuma;
                    if (std::fabs(delta) <= 0.0054F) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, balanceRadius), 0.0F, 1.0F) * 0.56F;
                    const float maxAdjustment = delta > 0.0F ? 0.0080F : 0.0110F;
                    const float adjustment =
                        std::clamp(delta * 0.32F, -maxAdjustment, maxAdjustment) * distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        horizonContrastBalanced.pixels[base + channel] =
                            std::clamp(output.pixels[base + channel] + adjustment, 0.0F, 1.0F);
                    }
                }
            }
        }
        output = std::move(horizonContrastBalanced);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.87);

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer horizonShiftedDottedCleanup = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || centerRatio > 0.970F) {
                continue;
            }
            const std::vector<ArtifactTrailPathPoint> effectivePath =
                trail.path.size() >= 2 ? trail.path
                                       : std::vector<ArtifactTrailPathPoint>{{trail.x1, trail.y1}, {trail.x2, trail.y2}};
            if (effectivePath.size() < 2) {
                continue;
            }

            std::vector<float> offsetScores(129, 0.0F);
            std::vector<std::uint16_t> offsetCounts(129, 0U);
            for (std::size_t segment = 1; segment < effectivePath.size(); ++segment) {
                const auto& start = effectivePath[segment - 1];
                const auto& end = effectivePath[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;
                for (float step = 0.0F; step <= segmentLength; step += 14.0F) {
                    const float centerPointX = start.x + unitX * step;
                    const float centerPointY = start.y + unitY * step;
                    for (float offset = -64.0F; offset <= 64.0F; offset += 1.0F) {
                        const auto px = static_cast<std::int32_t>(std::lround(centerPointX + normalX * offset));
                        const auto py = static_cast<std::int32_t>(std::lround(centerPointY + normalY * offset));
                        if (px < 0 || py < 0 || px >= static_cast<std::int32_t>(image.width) ||
                            py >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        std::vector<float> sideLumaSamples;
                        sideLumaSamples.reserve(12);
                        for (const float sampleOffset : {-76.0F, -62.0F, -48.0F, -36.0F, -26.0F, -18.0F, 18.0F,
                                                         26.0F, 36.0F, 48.0F, 62.0F, 76.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(px) + normalX * sampleOffset));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(py) + normalY * sampleOffset));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            sideLumaSamples.push_back(output.pixels[sampleBase] * 0.2126F +
                                                      output.pixels[sampleBase + 1] * 0.7152F +
                                                      output.pixels[sampleBase + 2] * 0.0722F);
                        }
                        if (sideLumaSamples.size() < 5) {
                            continue;
                        }
                        const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(py)) * image.width +
                                           static_cast<std::uint32_t>(px);
                        const auto base = pixel * output.channels;
                        const float targetLuma = median(sideLumaSamples);
                        const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                                  output.pixels[base + 2] * 0.0722F;
                        const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                                   image.pixels[base + 2] * 0.0722F;
                        const float colorEvidence = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) +
                                                    std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.18F;
                        const float score =
                            std::max(currentLuma - targetLuma, originalLuma - targetLuma * 0.96F) + colorEvidence * 0.45F;
                        if (score > 0.0040F) {
                            const auto bin = static_cast<std::size_t>(std::lround(offset + 64.0F));
                            if (bin < offsetScores.size()) {
                                offsetScores[bin] += score - 0.0040F;
                                ++offsetCounts[bin];
                            }
                        }
                    }
                }
            }

            struct OffsetCandidate {
                float score = 0.0F;
                std::uint16_t count = 0U;
                std::size_t bin = 64U;
            };
            std::vector<OffsetCandidate> offsetCandidates;
            offsetCandidates.reserve(offsetScores.size());
            for (std::size_t bin = 1; bin + 1 < offsetScores.size(); ++bin) {
                const float smoothedScore = offsetScores[bin - 1] + offsetScores[bin] + offsetScores[bin + 1];
                const auto smoothedCount = static_cast<std::uint16_t>(offsetCounts[bin - 1] + offsetCounts[bin] +
                                                                      offsetCounts[bin + 1]);
                if (smoothedCount >= 6 && smoothedScore >= 0.060F) {
                    offsetCandidates.push_back({smoothedScore, smoothedCount, bin});
                }
            }
            if (offsetCandidates.empty()) {
                continue;
            }
            std::sort(offsetCandidates.begin(), offsetCandidates.end(), [](const auto& a, const auto& b) {
                return a.score > b.score;
            });
            std::vector<float> offsetBiases;
            offsetBiases.reserve(4);
            const float strongestOffsetScore = offsetCandidates.front().score;
            for (const auto& candidate : offsetCandidates) {
                const float offset = static_cast<float>(candidate.bin) - 64.0F;
                bool separated = true;
                for (const float selectedOffset : offsetBiases) {
                    if (std::fabs(selectedOffset - offset) < 9.0F) {
                        separated = false;
                        break;
                    }
                }
                if (!separated || candidate.score < strongestOffsetScore * 0.52F) {
                    continue;
                }
                offsetBiases.push_back(std::clamp(offset, -64.0F, 64.0F));
                if (offsetBiases.size() >= 4) {
                    break;
                }
            }
            if (offsetBiases.empty()) {
                continue;
            }

            const float cleanupRadius = std::clamp(trail.width * 0.92F + 3.00F, 4.30F, 5.20F);
            const auto paintRadius = static_cast<std::int32_t>(std::ceil(cleanupRadius));
            for (const float offsetBias : offsetBiases) {
                for (std::size_t segment = 1; segment < effectivePath.size(); ++segment) {
                    const auto& start = effectivePath[segment - 1];
                    const auto& end = effectivePath[segment];
                    const float vx = end.x - start.x;
                    const float vy = end.y - start.y;
                    const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                    const float unitX = vx / segmentLength;
                    const float unitY = vy / segmentLength;
                    const float normalX = -unitY;
                    const float normalY = unitX;
                    for (float step = 0.0F; step <= segmentLength; step += 1.3F) {
                        const float shiftedX = start.x + unitX * step + normalX * offsetBias;
                        const float shiftedY = start.y + unitY * step + normalY * offsetBias;
                        for (std::int32_t dy = -paintRadius; dy <= paintRadius; ++dy) {
                            for (std::int32_t dx = -paintRadius; dx <= paintRadius; ++dx) {
                            const auto x = static_cast<std::int32_t>(std::lround(shiftedX)) + dx;
                            const auto y = static_cast<std::int32_t>(std::lround(shiftedY)) + dy;
                            if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                                y >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const float localDistance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                            if (localDistance > cleanupRadius) {
                                continue;
                            }
                            const auto pixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                static_cast<std::uint32_t>(x);

                            std::vector<float> targetSamples[3];
                            std::vector<float> targetLumaSamples;
                            for (auto& samples : targetSamples) {
                                samples.reserve(18);
                            }
                            targetLumaSamples.reserve(18);
                            for (const float sampleOffset : {24.0F, 34.0F, 48.0F, 64.0F, 84.0F}) {
                                for (const float side : {-1.0F, 1.0F}) {
                                    for (const float tangentShift : {-3.0F, 0.0F, 3.0F}) {
                                        const auto sx = static_cast<std::int32_t>(
                                            std::lround(static_cast<float>(x) + normalX * sampleOffset * side +
                                                        unitX * tangentShift));
                                        const auto sy = static_cast<std::int32_t>(
                                            std::lround(static_cast<float>(y) + normalY * sampleOffset * side +
                                                        unitY * tangentShift));
                                        if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                            sy >= static_cast<std::int32_t>(image.height)) {
                                            continue;
                                        }
                                        const auto samplePixel =
                                            static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                            static_cast<std::uint32_t>(sx);
                                        if (likelyArtificialColorSample(image, samplePixel) ||
                                            likelyNeutralBrightStarSample(image, samplePixel)) {
                                            continue;
                                        }
                                        const auto sampleBase = samplePixel * output.channels;
                                        const float red = output.pixels[sampleBase];
                                        const float green = output.pixels[sampleBase + 1];
                                        const float blue = output.pixels[sampleBase + 2];
                                        targetSamples[0].push_back(red);
                                        targetSamples[1].push_back(green);
                                        targetSamples[2].push_back(blue);
                                        targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                    }
                                }
                            }
                            if (targetLumaSamples.size() < 8) {
                                continue;
                            }

                            float target[3] = {0.0F, 0.0F, 0.0F};
                            float channelStd[3] = {0.0F, 0.0F, 0.0F};
                            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                target[channel] = percentileValue(targetSamples[channel], 0.42F);
                                const float meanSample =
                                    std::accumulate(targetSamples[channel].begin(), targetSamples[channel].end(), 0.0F) /
                                    static_cast<float>(targetSamples[channel].size());
                                float variance = 0.0F;
                                for (const float sample : targetSamples[channel]) {
                                    const float delta = sample - meanSample;
                                    variance += delta * delta;
                                }
                                variance /= static_cast<float>(targetSamples[channel].size());
                                channelStd[channel] = std::sqrt(std::max(0.0F, variance));
                            }

                            const auto base = pixel * output.channels;
                            const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                                      output.pixels[base + 2] * 0.0722F;
                            const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                                       image.pixels[base + 2] * 0.0722F;
                            const float targetLuma = median(targetLumaSamples);
                            const float evidence = std::max(currentLuma - targetLuma, originalLuma - targetLuma);
                            const float colorEvidence = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) +
                                                        std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.18F;
                            const bool brightDottedCore =
                                localDistance <= cleanupRadius * 0.72F &&
                                (currentLuma - targetLuma > 0.0040F || originalLuma - targetLuma > 0.0060F ||
                                 colorEvidence > 0.0024F);
                            if (evidence < 0.0006F && colorEvidence < 0.0018F && localDistance > cleanupRadius * 0.62F) {
                                continue;
                            }

                            const float feather =
                                1.0F - std::clamp(localDistance / std::max(0.1F, cleanupRadius), 0.0F, 1.0F) * 0.48F;
                            const float strength =
                                (brightDottedCore
                                     ? std::clamp((std::max(evidence, 0.0040F) + colorEvidence) / 0.010F, 0.86F, 0.98F)
                                     : std::clamp((std::max(evidence, 0.0012F) + colorEvidence * 0.70F) / 0.012F,
                                                  0.62F,
                                                  0.94F)) *
                                feather;
                            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                const float noise =
                                    deterministicNoise(static_cast<std::uint32_t>(x),
                                                       static_cast<std::uint32_t>(y),
                                                       static_cast<std::uint16_t>(channel + 541U)) *
                                    channelStd[channel] * 0.06F * feather;
                                const float coreTarget = brightDottedCore ? percentileValue(targetSamples[channel], 0.36F)
                                                                          : target[channel];
                                const float texturedTarget = std::clamp(coreTarget + noise, 0.0F, 1.0F);
                                horizonShiftedDottedCleanup.pixels[base + channel] =
                                    output.pixels[base + channel] * (1.0F - strength) + texturedTarget * strength;
                            }
                            }
                        }
                    }
                }
            }
        }
        output = std::move(horizonShiftedDottedCleanup);
    }

    // Dotted drone trails close to the horizon are sparse point series; the wider
    // ribbon passes below can turn those low-sky removals into visible smooth bands.
    const std::vector<ArtifactTrail> removedHorizonDottedDroneTrails = removedDottedDroneTrails;
    removedDottedDroneTrails.erase(
        std::remove_if(removedDottedDroneTrails.begin(), removedDottedDroneTrails.end(),
                       [&](const ArtifactTrail& trail) {
                           return centerY(trail) / std::max(1.0F, static_cast<float>(image.height)) >= 0.90F;
                       }),
        removedDottedDroneTrails.end());

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer balanced = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const bool lowHorizonTrail = centerY(trail) / std::max(1.0F, static_cast<float>(image.height)) >= 0.90F;
            const float balanceRadius = std::clamp(trail.width * 0.78F + 0.95F, 1.80F, 2.75F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - balanceRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + balanceRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - balanceRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + balanceRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > balanceRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    float target[3] = {0.0F, 0.0F, 0.0F};
                    float targetLuma = 0.0F;
                    std::size_t targetCount = 0;
                    for (const float offset : {5.0F, 8.0F, 12.0F, 16.0F, 20.0F}) {
                        if (!lowHorizonTrail && offset > 12.0F) {
                            continue;
                        }
                        for (const float side : {-1.0F, 1.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(x) + normalX * offset * side));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(y) + normalY * offset * side));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            const float red = output.pixels[sampleBase];
                            const float green = output.pixels[sampleBase + 1];
                            const float blue = output.pixels[sampleBase + 2];
                            target[0] += red;
                            target[1] += green;
                            target[2] += blue;
                            targetLuma += red * 0.2126F + green * 0.7152F + blue * 0.0722F;
                            targetCount += 1;
                        }
                    }
                    if (targetCount < 3) {
                        continue;
                    }

                    for (auto& value : target) {
                        value /= static_cast<float>(targetCount);
                    }
                    targetLuma /= static_cast<float>(targetCount);

                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                              output.pixels[base + 2] * 0.0722F;
                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    const float originalExcess = originalLuma - targetLuma;
                    const float currentDelta = currentLuma - targetLuma;
                    const bool coloredOriginal = warmExcessAt(image, pixel) > 0.0038F ||
                                                 colorVarianceAt(image, pixel) > 0.0024F;
                    const bool needsBalance =
                        originalExcess > 0.0020F || coloredOriginal || std::fabs(currentDelta) > 0.0028F;
                    if (!needsBalance) {
                        continue;
                    }

                    const float positiveEvidence = std::max(originalExcess, currentDelta);
                    const float darkEvidence = -currentDelta;
                    const float correctionEvidence = std::max(positiveEvidence, darkEvidence);
                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, balanceRadius), 0.0F, 1.0F) * 0.32F;
                    const float strength =
                        std::clamp((correctionEvidence - (lowHorizonTrail ? 0.0006F : 0.0012F)) /
                                       (lowHorizonTrail ? 0.0056F : 0.0080F),
                                   lowHorizonTrail ? 0.18F : 0.10F,
                                   lowHorizonTrail ? 0.90F : 0.68F) *
                        distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        balanced.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                    }
                }
            }
        }
        output = std::move(balanced);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer lifted = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            const bool lowHorizonTrail = centerRatio >= 0.90F;
            const bool midLowHorizonTrail = centerRatio >= 0.80F;
            if (!midLowHorizonTrail) {
                continue;
            }

            const float liftRadius = lowHorizonTrail ? std::clamp(trail.width * 0.95F + 1.35F, 2.40F, 3.40F)
                                                     : std::clamp(trail.width * 0.90F + 1.15F, 2.20F, 3.10F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - liftRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + liftRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - liftRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + liftRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > liftRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(10);
                    }
                    targetLumaSamples.reserve(10);
                    for (const float offset : {6.0F, 10.0F, 15.0F, 21.0F, 28.0F}) {
                        if (!lowHorizonTrail && offset > 21.0F) {
                            continue;
                        }
                        for (const float side : {-1.0F, 1.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(x) + normalX * offset * side));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(y) + normalY * offset * side));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (mask[samplePixel] || likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            const float red = output.pixels[sampleBase];
                            const float green = output.pixels[sampleBase + 1];
                            const float blue = output.pixels[sampleBase + 2];
                            targetSamples[0].push_back(red);
                            targetSamples[1].push_back(green);
                            targetSamples[2].push_back(blue);
                            targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                    if (targetLumaSamples.size() < 4) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(targetSamples[channel]);
                    }
                    const float targetLuma = median(targetLumaSamples);

                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                              output.pixels[base + 2] * 0.0722F;
                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    const float currentDelta = currentLuma - targetLuma;
                    const float originalExcess = originalLuma - targetLuma;
                    float channelResidual = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        channelResidual = std::max(channelResidual, std::fabs(output.pixels[base + channel] - target[channel]));
                    }
                    const float correctionMagnitude =
                        std::max({std::fabs(currentDelta), channelResidual * 0.88F, std::max(0.0F, originalExcess) * 0.72F});
                    const bool maskedDottedPixel =
                        dottedDroneMask[pixel] != 0 && trailMaskMetadataAt(maskMetadata, pixel).alpha > 0.0F;
                    const bool coloredOriginal = warmExcessAt(image, pixel) > 0.0036F ||
                                                 colorVarianceAt(image, pixel) > 0.0022F ||
                                                 navigationLightScoreAt(image, pixel) > 0.0050F;
                    const float maskThreshold = lowHorizonTrail ? 0.0009F : 0.0013F;
                    const float unmaskedDarkThreshold = lowHorizonTrail ? 0.0042F : 0.0054F;
                    const bool unmaskedDarkPit = !maskedDottedPixel && currentDelta < -unmaskedDarkThreshold;
                    const bool originalPointResidual =
                        distance.distance <= liftRadius * 0.72F &&
                        originalExcess > (lowHorizonTrail ? 0.0032F : 0.0042F);
                    const bool maskedResidual =
                        maskedDottedPixel && (correctionMagnitude > maskThreshold || coloredOriginal || originalPointResidual);
                    const bool coloredUnmaskedResidual =
                        !maskedDottedPixel && coloredOriginal && channelResidual > (lowHorizonTrail ? 0.0020F : 0.0028F);
                    if (!maskedResidual && !unmaskedDarkPit && !coloredUnmaskedResidual && !originalPointResidual) {
                        continue;
                    }
                    if (!maskedDottedPixel && coloredOriginal && currentDelta < -0.0020F &&
                        correctionMagnitude < unmaskedDarkThreshold && channelResidual < 0.0030F) {
                        continue;
                    }
                    float blendTarget[3] = {target[0], target[1], target[2]};
                    if (!lowHorizonTrail && targetLuma > currentLuma + 0.0006F) {
                        const float lumaTrim = targetLuma - (currentLuma + 0.0006F);
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            blendTarget[channel] = std::clamp(target[channel] - lumaTrim, 0.0F, 1.0F);
                        }
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, liftRadius), 0.0F, 1.0F) * 0.32F;
                    const float maskWeight = maskedDottedPixel ? 1.0F : (lowHorizonTrail ? 0.58F : 0.46F);
                    const float strengthCeiling = lowHorizonTrail ? 0.84F : 0.70F;
                    const float strengthScale = lowHorizonTrail ? 0.016F : 0.020F;
                    float strength =
                        std::clamp((correctionMagnitude - maskThreshold) / strengthScale, 0.0F, strengthCeiling) *
                        distanceWeight * maskWeight;
                    if (maskedDottedPixel && coloredOriginal) {
                        strength = std::max(strength, (lowHorizonTrail ? 0.36F : 0.30F) * distanceWeight);
                    }
                    if (originalPointResidual) {
                        strength = std::max(strength, (lowHorizonTrail ? 0.46F : 0.38F) * distanceWeight * maskWeight);
                    }
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        lifted.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + blendTarget[channel] * strength;
                    }
                }
            }
        }
        output = std::move(lifted);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer chromaMatched = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F) {
                continue;
            }
            const bool lowHorizonTrail = centerRatio >= 0.90F;
            const float chromaRadius = lowHorizonTrail ? std::clamp(trail.width * 0.84F + 1.05F, 2.00F, 3.05F)
                                                       : std::clamp(trail.width * 0.86F + 1.05F, 1.90F, 2.95F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - chromaRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + chromaRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - chromaRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + chromaRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > chromaRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const bool maskedDottedPixel =
                        dottedDroneMask[pixel] != 0 && trailMaskMetadataAt(maskMetadata, pixel).alpha > 0.0F;
                    if (!maskedDottedPixel && likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(8);
                    }
                    targetLumaSamples.reserve(8);
                    for (const float offset : {7.0F, 11.0F, 17.0F, 23.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(x) + normalX * offset * side));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(y) + normalY * offset * side));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (mask[samplePixel] || likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            const float red = output.pixels[sampleBase];
                            const float green = output.pixels[sampleBase + 1];
                            const float blue = output.pixels[sampleBase + 2];
                            targetSamples[0].push_back(red);
                            targetSamples[1].push_back(green);
                            targetSamples[2].push_back(blue);
                            targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                    if (targetLumaSamples.size() < 4) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(targetSamples[channel]);
                    }
                    const float targetLuma = median(targetLumaSamples);
                    const auto base = pixel * output.channels;
                    const float current[3] = {output.pixels[base], output.pixels[base + 1], output.pixels[base + 2]};
                    const float currentLuma = current[0] * 0.2126F + current[1] * 0.7152F + current[2] * 0.0722F;
                    float chromaResidual = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        chromaResidual =
                            std::max(chromaResidual, std::fabs((current[channel] - currentLuma) - (target[channel] - targetLuma)));
                    }
                    const float lumaResidual = currentLuma - targetLuma;

                    const float originalColorEvidence =
                        warmExcessAt(image, pixel) + navigationLightScoreAt(image, pixel) +
                        std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.35F;
                    const bool narrowUnmaskedResidual =
                        !maskedDottedPixel && distance.distance <= chromaRadius * 0.78F &&
                        (originalColorEvidence > 0.0040F || chromaResidual > 0.0050F || lumaResidual > 0.0040F);
                    if (!maskedDottedPixel && !narrowUnmaskedResidual) {
                        continue;
                    }
                    if (chromaResidual <= (maskedDottedPixel ? 0.0016F : 0.0030F) &&
                        lumaResidual <= (maskedDottedPixel ? 0.0018F : 0.0034F) &&
                        originalColorEvidence <= 0.0050F) {
                        continue;
                    }

                    float colorTarget[3] = {0.0F, 0.0F, 0.0F};
                    const float lumaOffset = currentLuma - targetLuma;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        colorTarget[channel] = std::clamp(target[channel] + lumaOffset, 0.0F, 1.0F);
                    }
                    if (lumaResidual > 0.0F) {
                        const float lumaBlend =
                            std::clamp((lumaResidual - (lowHorizonTrail ? 0.0010F : 0.0014F)) /
                                           (lowHorizonTrail ? 0.0085F : 0.0075F),
                                       0.0F,
                                       maskedDottedPixel ? (lowHorizonTrail ? 0.90F : 0.92F)
                                                         : (lowHorizonTrail ? 0.34F : 0.30F));
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            colorTarget[channel] =
                                colorTarget[channel] * (1.0F - lumaBlend) + target[channel] * lumaBlend;
                        }
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, chromaRadius), 0.0F, 1.0F) * 0.36F;
                    const float maskWeight = maskedDottedPixel ? 1.0F : (lowHorizonTrail ? 0.62F : 0.55F);
                    const float correctionEvidence = std::max(chromaResidual, std::max(0.0F, lumaResidual) * 1.05F);
                    const float strengthCeiling = maskedDottedPixel ? (lowHorizonTrail ? 0.72F : 0.88F)
                                                                    : (lowHorizonTrail ? 0.48F : 0.58F);
                    const float evidenceOffset = maskedDottedPixel ? 0.0008F : 0.0016F;
                    const float strength =
                        std::clamp((correctionEvidence - evidenceOffset) / (lowHorizonTrail ? 0.016F : 0.012F),
                                   0.0F,
                                   strengthCeiling) *
                        distanceWeight * maskWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        chromaMatched.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + colorTarget[channel] * strength;
                    }
                }
            }
        }
        output = std::move(chromaMatched);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.89);

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer damped = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F) {
                continue;
            }
            const bool lowHorizonTrail = centerRatio >= 0.90F;
            const float dampRadius = lowHorizonTrail ? std::clamp(trail.width * 0.72F + 1.12F, 2.10F, 2.85F)
                                                     : std::clamp(trail.width * 0.70F + 0.95F, 1.90F, 2.55F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - dampRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + dampRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - dampRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + dampRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > dampRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(10);
                    }
                    targetLumaSamples.reserve(10);
                    for (const float offset : {10.0F, 16.0F, 24.0F, 32.0F, 42.0F}) {
                        if (!lowHorizonTrail && offset > 32.0F) {
                            continue;
                        }
                        for (const float side : {-1.0F, 1.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(x) + normalX * offset * side));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(y) + normalY * offset * side));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                            if (sampleDistance.distance <= dampRadius + 2.0F) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (mask[samplePixel] || likelyNeutralBrightStarSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            const float red = output.pixels[sampleBase];
                            const float green = output.pixels[sampleBase + 1];
                            const float blue = output.pixels[sampleBase + 2];
                            targetSamples[0].push_back(red);
                            targetSamples[1].push_back(green);
                            targetSamples[2].push_back(blue);
                            targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                    if (targetLumaSamples.size() < 4) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(targetSamples[channel]);
                    }
                    const float targetLuma = median(targetLumaSamples);

                    const auto base = pixel * output.channels;
                    const float current[3] = {output.pixels[base], output.pixels[base + 1], output.pixels[base + 2]};
                    const float currentLuma = current[0] * 0.2126F + current[1] * 0.7152F + current[2] * 0.0722F;
                    const float original[3] = {image.pixels[base], image.pixels[base + 1], image.pixels[base + 2]};
                    const float originalLuma = original[0] * 0.2126F + original[1] * 0.7152F + original[2] * 0.0722F;
                    const float overshoot = currentLuma - targetLuma;
                    const float originalExcess = originalLuma - targetLuma;
                    const float overshootThreshold = lowHorizonTrail ? 0.0043F : 0.0055F;
                    if (overshoot <= overshootThreshold) {
                        continue;
                    }
                    const bool maskedDottedPixel =
                        dottedDroneMask[pixel] != 0 && trailMaskMetadataAt(maskMetadata, pixel).alpha > 0.0F;
                    if (originalExcess > 0.031F && !maskedDottedPixel && likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    float channelResidual = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        channelResidual = std::max(channelResidual, std::fabs(current[channel] - target[channel]));
                    }
                    const float colorThreshold = lowHorizonTrail ? 0.0094F : 0.0118F;
                    const float strongOvershoot = lowHorizonTrail ? 0.0078F : 0.0102F;
                    if (channelResidual < colorThreshold && overshoot < strongOvershoot) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, dampRadius), 0.0F, 1.0F) * 0.35F;
                    const float coreResidualRadius = lowHorizonTrail ? 1.75F : 1.55F;
                    float strength =
                        std::clamp((overshoot - (lowHorizonTrail ? 0.0031F : 0.0039F)) /
                                       (lowHorizonTrail ? 0.039F : 0.047F),
                                   0.0F,
                                   lowHorizonTrail ? 0.82F : 0.76F) *
                        distanceWeight;
                    if (distance.distance <= coreResidualRadius && !likelyNeutralBrightStarSample(image, pixel)) {
                        const float coreEvidence = std::max(overshoot / (lowHorizonTrail ? 0.0078F : 0.0102F),
                                                            channelResidual / (lowHorizonTrail ? 0.0137F : 0.0160F));
                        if (coreEvidence > 0.68F) {
                            const float coreMinimumStrength = lowHorizonTrail ? 0.45F : 0.52F;
                            const float coreMaximumStrength = lowHorizonTrail ? 0.86F : 0.90F;
                            const float coreRamp = lowHorizonTrail ? 1.16F : 1.02F;
                            strength = std::max(
                                strength,
                                std::clamp((coreEvidence - 0.42F) / coreRamp, coreMinimumStrength, coreMaximumStrength) *
                                    distanceWeight);
                        }
                    }
                    if (originalExcess <= 0.0F) {
                        strength = std::max(strength, (lowHorizonTrail ? 0.60F : 0.62F) * distanceWeight);
                    }
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        damped.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                    }
                }
            }
        }
        output = std::move(damped);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer scrubbed = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F) {
                continue;
            }
            const bool lowHorizonTrail = centerRatio >= 0.90F;
            const float scrubRadius = lowHorizonTrail ? std::clamp(trail.width * 1.25F + 2.60F, 4.80F, 7.20F)
                                                      : std::clamp(trail.width * 1.15F + 2.20F, 4.20F, 6.40F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - scrubRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + scrubRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - scrubRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + scrubRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > scrubRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(8);
                    }
                    targetLumaSamples.reserve(8);
                    for (const float offset : {8.0F, 13.0F, 19.0F, 27.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(x) + normalX * offset * side));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(y) + normalY * offset * side));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                            if (sampleDistance.distance <= scrubRadius + 2.0F) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            const float red = output.pixels[sampleBase];
                            const float green = output.pixels[sampleBase + 1];
                            const float blue = output.pixels[sampleBase + 2];
                            targetSamples[0].push_back(red);
                            targetSamples[1].push_back(green);
                            targetSamples[2].push_back(blue);
                            targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                    if (targetLumaSamples.size() < 4) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(targetSamples[channel]);
                    }
                    const float targetLuma = median(targetLumaSamples);
                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                              output.pixels[base + 2] * 0.0722F;
                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    const float originalEvidence = localDottedPointScore(trail,
                                                                         luminance,
                                                                         image,
                                                                         static_cast<float>(x),
                                                                         static_cast<float>(y));
                    const float originalExcess = originalLuma - targetLuma;
                    const float currentExcess = currentLuma - targetLuma;
                    const float colorEvidence = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) +
                                                std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.16F;
                    const float evidence = std::max({originalEvidence, originalExcess, currentExcess, colorEvidence});
                    const float evidenceThreshold = lowHorizonTrail ? 0.0018F : 0.0024F;
                    if (evidence <= evidenceThreshold) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, scrubRadius), 0.0F, 1.0F) * 0.38F;
                    const float strength =
                        std::clamp((evidence - evidenceThreshold) / (lowHorizonTrail ? 0.010F : 0.012F),
                                   0.38F,
                                   lowHorizonTrail ? 0.97F : 0.94F) *
                        distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        scrubbed.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                    }
                }
            }
        }
        output = std::move(scrubbed);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer beaconPatched = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F) {
                continue;
            }
            const bool lowHorizonTrail = centerRatio >= 0.90F;
            const float searchRadius = lowHorizonTrail ? std::clamp(trail.width * 1.20F + 5.80F, 7.50F, 9.50F)
                                                       : std::clamp(trail.width * 1.35F + 2.80F, 5.20F, 7.40F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - searchRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + searchRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - searchRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + searchRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > searchRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(10);
                    }
                    targetLumaSamples.reserve(10);
                    for (const float offset : {9.0F, 14.0F, 21.0F, 30.0F, 40.0F}) {
                        if (!lowHorizonTrail && offset > 30.0F) {
                            continue;
                        }
                        for (const float side : {-1.0F, 1.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(x) + normalX * offset * side));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(y) + normalY * offset * side));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                            if (sampleDistance.distance <= searchRadius + 2.5F) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            const float red = output.pixels[sampleBase];
                            const float green = output.pixels[sampleBase + 1];
                            const float blue = output.pixels[sampleBase + 2];
                            targetSamples[0].push_back(red);
                            targetSamples[1].push_back(green);
                            targetSamples[2].push_back(blue);
                            targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                    if (targetLumaSamples.size() < 4) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(targetSamples[channel]);
                    }
                    const float targetLuma = median(targetLumaSamples);
                    const auto base = pixel * output.channels;
                    const float currentRed = output.pixels[base];
                    const float currentGreen = output.pixels[base + 1];
                    const float currentBlue = output.pixels[base + 2];
                    const float currentLuma = currentRed * 0.2126F + currentGreen * 0.7152F + currentBlue * 0.0722F;
                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    const float originalPointEvidence = localDottedPointScore(trail,
                                                                              luminance,
                                                                              image,
                                                                              static_cast<float>(x),
                                                                              static_cast<float>(y));
                    const float originalColorEvidence =
                        navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 1.10F +
                        std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.26F;
                    float channelResidual = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        channelResidual = std::max(channelResidual, output.pixels[base + channel] - target[channel]);
                    }
                    const float currentExcess = currentLuma - targetLuma;
                    const float originalExcess = originalLuma - targetLuma;
                    const float residualEvidence =
                        std::max({currentExcess, channelResidual, originalExcess * 0.70F, originalPointEvidence * 0.78F,
                                  originalColorEvidence});
                    const bool coloredBeacon = originalColorEvidence > (lowHorizonTrail ? 0.0018F : 0.0034F);
                    const bool dottedPeak = originalPointEvidence > (lowHorizonTrail ? 0.0028F : 0.0052F) &&
                                            (!lowHorizonTrail || distance.distance <= 6.5F);
                    const bool originalBrightPeak = originalExcess > (lowHorizonTrail ? 0.0006F : 0.0020F) &&
                                                    (!lowHorizonTrail || distance.distance <= 4.5F);
                    if (!coloredBeacon && !dottedPeak && !originalBrightPeak) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, searchRadius), 0.0F, 1.0F) * 0.45F;
                    const float strength =
                        std::clamp((residualEvidence - (lowHorizonTrail ? 0.0010F : 0.0016F)) /
                                       (lowHorizonTrail ? 0.0065F : 0.0080F),
                                   lowHorizonTrail ? 0.82F : 0.48F,
                                   lowHorizonTrail ? 0.99F : 0.94F) *
                        distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        beaconPatched.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                    }
                }
            }
        }
        output = std::move(beaconPatched);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer peakPatched = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            const bool lowHorizonTrail = centerRatio >= 0.90F;
            if (!lowHorizonTrail || trail.path.size() < 2) {
                continue;
            }

            float unitX = std::cos(trail.angleRadians);
            float unitY = std::sin(trail.angleRadians);
            if (unitX < 0.0F) {
                unitX = -unitX;
                unitY = -unitY;
            }
            const float normalX = -unitY;
            const float normalY = unitX;
            auto interval = projectedInterval(trail, unitX, unitY);
            interval.minProjection -= 32.0F;
            interval.maxProjection += 32.0F;
            const float corridorRadius = std::clamp(trail.width * 2.8F + 18.0F, 20.0F, 30.0F);
            const float binSize = 18.0F;
            const int binCount = std::clamp(
                static_cast<int>(std::ceil((interval.maxProjection - interval.minProjection) / binSize)) + 1,
                1,
                512
            );

            struct BeaconPeak {
                float x = 0.0F;
                float y = 0.0F;
                float projection = 0.0F;
                float score = -1.0F;
            };
            std::vector<BeaconPeak> bins(static_cast<std::size_t>(binCount));

            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }
            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - corridorRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + corridorRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - corridorRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + corridorRadius + 2.0F));

            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 2 || y < 2 || x >= static_cast<std::int32_t>(image.width) - 2 ||
                        y >= static_cast<std::int32_t>(image.height) - 2) {
                        continue;
                    }
                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > corridorRadius) {
                        continue;
                    }
                    const float projection = static_cast<float>(x) * unitX + static_cast<float>(y) * unitY;
                    if (projection < interval.minProjection || projection > interval.maxProjection) {
                        continue;
                    }
                    const int bin = std::clamp(static_cast<int>((projection - interval.minProjection) / binSize), 0, binCount - 1);
                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float center = luminance[pixel];
                    std::vector<float> backgroundSamples;
                    backgroundSamples.reserve(8);
                    for (const float offset : {7.0F, 12.0F, 18.0F}) {
                        backgroundSamples.push_back(luminanceNearest(luminance,
                                                                     image.width,
                                                                     image.height,
                                                                     static_cast<float>(x) + normalX * offset,
                                                                     static_cast<float>(y) + normalY * offset));
                        backgroundSamples.push_back(luminanceNearest(luminance,
                                                                     image.width,
                                                                     image.height,
                                                                     static_cast<float>(x) - normalX * offset,
                                                                     static_cast<float>(y) - normalY * offset));
                    }
                    backgroundSamples.push_back(luminanceNearest(luminance,
                                                                 image.width,
                                                                 image.height,
                                                                 static_cast<float>(x) + unitX * 10.0F,
                                                                 static_cast<float>(y) + unitY * 10.0F));
                    backgroundSamples.push_back(luminanceNearest(luminance,
                                                                 image.width,
                                                                 image.height,
                                                                 static_cast<float>(x) - unitX * 10.0F,
                                                                 static_cast<float>(y) - unitY * 10.0F));
                    const float background = median(backgroundSamples);
                    const float contrast = center - background;
                    const auto base = pixel * image.channels;
                    const float red = image.pixels[base];
                    const float green = image.pixels[base + 1];
                    const float blue = image.pixels[base + 2];
                    const float maximum = std::max({red, green, blue});
                    const float minimum = std::min({red, green, blue});
                    const float chromaRange = maximum - minimum;
                    const float beaconColor = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 1.35F;
                    const float chromaEvidence = std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel)));
                    const float colorEvidence = beaconColor + chromaEvidence * 0.30F;
                    if (maximum > 0.56F) {
                        continue;
                    }
                    const bool coloredBeacon =
                        beaconColor > 0.0013F || (chromaRange > 0.018F && maximum < 0.36F) ||
                        (chromaEvidence > 0.010F && std::max(0.0F, contrast) > 0.0012F);
                    if (!coloredBeacon && std::max(0.0F, contrast) < 0.010F) {
                        continue;
                    }
                    const float contrastWeight = coloredBeacon ? 0.46F : 0.10F;
                    const float score = std::max(0.0F, contrast) * contrastWeight + colorEvidence * 2.45F -
                                        std::max(0.0F, distance.distance - 4.0F) * 0.00010F;
                    if (score < 0.0040F || score <= bins[static_cast<std::size_t>(bin)].score) {
                        continue;
                    }
                    bins[static_cast<std::size_t>(bin)] = {static_cast<float>(x), static_cast<float>(y), projection, score};
                }
            }

            std::vector<BeaconPeak> peaks;
            peaks.reserve(bins.size());
            for (const auto& peak : bins) {
                if (peak.score > 0.0040F) {
                    peaks.push_back(peak);
                }
            }
            if (peaks.size() < 9) {
                continue;
            }

            std::vector<BeaconPeak> supportedPeaks;
            supportedPeaks.reserve(peaks.size());
            for (std::size_t index = 0; index < peaks.size(); ++index) {
                bool hasNeighbor = false;
                for (std::size_t other = 0; other < peaks.size(); ++other) {
                    if (index == other) {
                        continue;
                    }
                    const float gap = std::fabs(peaks[other].projection - peaks[index].projection);
                    if (gap >= 14.0F && gap <= 86.0F) {
                        hasNeighbor = true;
                        break;
                    }
                }
                if (hasNeighbor) {
                    supportedPeaks.push_back(peaks[index]);
                }
            }
            if (supportedPeaks.size() < 7) {
                continue;
            }

            std::sort(supportedPeaks.begin(), supportedPeaks.end(), [](const BeaconPeak& left, const BeaconPeak& right) {
                return left.projection < right.projection;
            });

            for (const auto& peak : supportedPeaks) {
                const float normalPatchRadius = std::clamp(3.4F + peak.score * 42.0F, 3.6F, 6.8F);
                const float alongPatchRadius = std::clamp(normalPatchRadius * 2.45F, 9.0F, 15.5F);
                const float patchBounds = std::max(normalPatchRadius, alongPatchRadius);
                const auto minPatchX = static_cast<std::int32_t>(std::floor(peak.x - patchBounds - 1.0F));
                const auto maxPatchX = static_cast<std::int32_t>(std::ceil(peak.x + patchBounds + 1.0F));
                const auto minPatchY = static_cast<std::int32_t>(std::floor(peak.y - patchBounds - 1.0F));
                const auto maxPatchY = static_cast<std::int32_t>(std::ceil(peak.y + patchBounds + 1.0F));
                for (std::int32_t y = minPatchY; y <= maxPatchY; ++y) {
                    for (std::int32_t x = minPatchX; x <= maxPatchX; ++x) {
                        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                            y >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const float dx = static_cast<float>(x) - peak.x;
                        const float dy = static_cast<float>(y) - peak.y;
                        const float alongDistance = dx * unitX + dy * unitY;
                        const float normalDistance = dx * normalX + dy * normalY;
                        const float ellipseDistance = std::sqrt((alongDistance / std::max(0.1F, alongPatchRadius)) *
                                                                    (alongDistance / std::max(0.1F, alongPatchRadius)) +
                                                                (normalDistance / std::max(0.1F, normalPatchRadius)) *
                                                                    (normalDistance / std::max(0.1F, normalPatchRadius)));
                        if (ellipseDistance > 1.0F) {
                            continue;
                        }

                        const auto pixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                            static_cast<std::uint32_t>(x);
                        std::vector<float> targetSamples[3];
                        for (auto& channelSamples : targetSamples) {
                            channelSamples.reserve(10);
                        }
                        for (const float offset : {9.0F, 15.0F, 24.0F, 38.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-4.0F, 0.0F, 4.0F}) {
                                    const auto sx = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(x) + normalX * offset * side + unitX * shift));
                                    const auto sy = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(y) + normalY * offset * side + unitY * shift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                    if (sampleDistance.distance <= corridorRadius + 1.5F) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    if (likelyArtificialColorSample(image, samplePixel)) {
                                        continue;
                                    }
                                    const auto sampleBase = samplePixel * output.channels;
                                    targetSamples[0].push_back(output.pixels[sampleBase]);
                                    targetSamples[1].push_back(output.pixels[sampleBase + 1]);
                                    targetSamples[2].push_back(output.pixels[sampleBase + 2]);
                                }
                            }
                        }
                        if (targetSamples[0].size() < 4 || targetSamples[1].size() < 4 || targetSamples[2].size() < 4) {
                            continue;
                        }
                        float target[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = median(targetSamples[channel]);
                        }
                        const float strength = std::clamp(1.0F - ellipseDistance * 0.25F, 0.76F, 0.995F);
                        const auto base = pixel * output.channels;
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            peakPatched.pixels[base + channel] =
                                output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                        }
                    }
                }
            }

        }
        output = std::move(peakPatched);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer residualMatched = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || trail.path.size() < 2) {
                continue;
            }

            const float residualRadius = std::clamp(trail.width * 1.15F + 5.8F, 7.5F, 9.5F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - residualRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + residualRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - residualRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + residualRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > residualRadius) {
                        continue;
                    }
                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(8);
                    }
                    targetLumaSamples.reserve(8);
                    for (const float offset : {8.0F, 14.0F, 22.0F, 34.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(x) + normalX * offset * side));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(y) + normalY * offset * side));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                            if (sampleDistance.distance <= residualRadius + 2.0F) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            const float red = output.pixels[sampleBase];
                            const float green = output.pixels[sampleBase + 1];
                            const float blue = output.pixels[sampleBase + 2];
                            targetSamples[0].push_back(red);
                            targetSamples[1].push_back(green);
                            targetSamples[2].push_back(blue);
                            targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                    if (targetLumaSamples.size() < 4) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(targetSamples[channel]);
                    }
                    const float targetLuma = median(targetLumaSamples);
                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                              output.pixels[base + 2] * 0.0722F;
                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    const float originalColorEvidence =
                        navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) +
                        std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.22F;
                    const float currentExcess = currentLuma - targetLuma;
                    const float originalExcess = originalLuma - targetLuma;
                    if (currentExcess <= 0.0012F &&
                        !(originalColorEvidence > 0.0020F && originalExcess > 0.0008F)) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, residualRadius), 0.0F, 1.0F) * 0.70F;
                    const float evidence = std::max(currentExcess, originalColorEvidence * 0.65F + originalExcess * 0.35F);
                    const float strength =
                        std::clamp((evidence - 0.0006F) / 0.0075F, 0.0F, 0.46F) * distanceWeight;
                    if (strength <= 0.0F) {
                        continue;
                    }
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        residualMatched.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                    }
                }
            }
        }
        output = std::move(residualMatched);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.91);

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer ribbonMatched = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F) {
                continue;
            }
            const bool lowHorizonTrail = centerRatio >= 0.90F;
            const float ribbonRadius = lowHorizonTrail ? std::clamp(trail.width * 1.02F + 3.30F, 4.60F, 6.40F)
                                                       : std::clamp(trail.width * 0.90F + 2.50F, 3.60F, 5.20F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - ribbonRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + ribbonRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - ribbonRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + ribbonRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > ribbonRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(10);
                    }
                    targetLumaSamples.reserve(10);
                    for (const float offset : {8.0F, 13.0F, 20.0F, 29.0F, 39.0F}) {
                        if (!lowHorizonTrail && offset > 29.0F) {
                            continue;
                        }
                        for (const float side : {-1.0F, 1.0F}) {
                            const auto sx = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(x) + normalX * offset * side));
                            const auto sy = static_cast<std::int32_t>(
                                std::lround(static_cast<float>(y) + normalY * offset * side));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                            if (sampleDistance.distance <= ribbonRadius + 2.0F) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyArtificialColorSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            const float red = output.pixels[sampleBase];
                            const float green = output.pixels[sampleBase + 1];
                            const float blue = output.pixels[sampleBase + 2];
                            targetSamples[0].push_back(red);
                            targetSamples[1].push_back(green);
                            targetSamples[2].push_back(blue);
                            targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                        }
                    }
                    if (targetLumaSamples.size() < 4) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(targetSamples[channel]);
                    }
                    const float targetLuma = median(targetLumaSamples);
                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                              output.pixels[base + 2] * 0.0722F;
                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    const float currentExcess = currentLuma - targetLuma;
                    const float originalExcess = originalLuma - targetLuma;
                    const float originalColorEvidence =
                        navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) +
                        std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.22F;
                    const bool pathCore = distance.distance <= ribbonRadius * 0.72F;
                    const bool trailEvidence = pathCore || originalExcess > (lowHorizonTrail ? 0.0008F : 0.0013F) ||
                                               currentExcess > (lowHorizonTrail ? 0.0008F : 0.0012F) ||
                                               originalColorEvidence > (lowHorizonTrail ? 0.0025F : 0.0032F);
                    if (!trailEvidence) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, ribbonRadius), 0.0F, 1.0F) * 0.55F;
                    const float evidenceBoost =
                        std::clamp(std::max({currentExcess, originalExcess, originalColorEvidence}) /
                                       (lowHorizonTrail ? 0.0080F : 0.0100F),
                                   0.0F,
                                   0.32F);
                    const float strength = (pathCore ? (lowHorizonTrail ? 0.96F : 0.86F)
                                                     : (lowHorizonTrail ? 0.72F : 0.58F)) *
                                               distanceWeight +
                                           evidenceBoost;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        ribbonMatched.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - std::clamp(strength, 0.0F, 0.96F)) +
                            target[channel] * std::clamp(strength, 0.0F, 0.96F);
                    }
                }
            }
        }
        output = std::move(ribbonMatched);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer midLowResidualScrubbed = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || centerRatio >= 0.90F || trail.path.size() < 2) {
                continue;
            }

            const float scrubRadius = std::clamp(trail.width * 0.48F + 1.00F, 1.85F, 2.55F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - scrubRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + scrubRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - scrubRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + scrubRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > scrubRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& channelSamples : targetSamples) {
                        channelSamples.reserve(18);
                    }
                    targetLumaSamples.reserve(18);
                    for (const float offset : {7.0F, 11.0F, 17.0F, 25.0F, 36.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            for (const float shift : {-2.5F, 0.0F, 2.5F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * offset * side + distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * offset * side + distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= scrubRadius + 1.6F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                const float red = output.pixels[sampleBase];
                                const float green = output.pixels[sampleBase + 1];
                                const float blue = output.pixels[sampleBase + 2];
                                targetSamples[0].push_back(red);
                                targetSamples[1].push_back(green);
                                targetSamples[2].push_back(blue);
                                targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (targetLumaSamples.size() < 8) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    float channelVariance[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = percentileValue(targetSamples[channel], 0.42F);
                        const float meanSample =
                            std::accumulate(targetSamples[channel].begin(), targetSamples[channel].end(), 0.0F) /
                            static_cast<float>(targetSamples[channel].size());
                        for (const float sample : targetSamples[channel]) {
                            const float delta = sample - meanSample;
                            channelVariance[channel] += delta * delta;
                        }
                        channelVariance[channel] /= static_cast<float>(targetSamples[channel].size());
                    }
                    const float targetLuma = percentileValue(targetLumaSamples, 0.42F);

                    const auto base = pixel * output.channels;
                    const float current[3] = {output.pixels[base], output.pixels[base + 1], output.pixels[base + 2]};
                    const float currentLuma = current[0] * 0.2126F + current[1] * 0.7152F + current[2] * 0.0722F;
                    const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                               image.pixels[base + 2] * 0.0722F;
                    float positiveChannelResidual = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        positiveChannelResidual = std::max(positiveChannelResidual, current[channel] - target[channel]);
                    }
                    const float currentExcess = currentLuma - targetLuma;
                    const float originalExcess = originalLuma - targetLuma;
                    const float pointEvidence =
                        localDottedPointScore(trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                    const float colorEvidence = navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) +
                                                std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.20F;
                    const bool nearCore = distance.distance <= scrubRadius * 0.74F;
                    const bool brightResidual = currentExcess > 0.0030F || positiveChannelResidual > 0.0040F;
                    const bool strongBrightResidual =
                        nearCore && (currentExcess > 0.0080F || positiveChannelResidual > 0.0100F);
                    const bool originalBackedResidual =
                        nearCore && (pointEvidence > 0.0032F || colorEvidence > 0.0024F || originalExcess > 0.0038F);
                    if (!brightResidual && !originalBackedResidual) {
                        continue;
                    }

                    const float evidence =
                        std::max({std::max(0.0F, currentExcess), positiveChannelResidual, pointEvidence * 0.72F,
                                  colorEvidence * 1.24F, originalExcess * 0.38F});
                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, scrubRadius), 0.0F, 1.0F) * 0.50F;
                    const float strengthCeiling = strongBrightResidual ? 0.68F : 0.60F;
                    float strength = std::clamp((evidence - 0.0018F) / 0.015F, 0.0F, strengthCeiling) * distanceWeight;
                    if (nearCore && brightResidual) {
                        strength = std::max(strength, 0.28F * distanceWeight);
                    }
                    if (strongBrightResidual) {
                        strength = std::max(strength, 0.40F * distanceWeight);
                    }
                    if (originalBackedResidual) {
                        strength = std::max(strength, 0.34F * distanceWeight);
                    }
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float texture =
                            deterministicNoise(static_cast<std::uint32_t>(x),
                                               static_cast<std::uint32_t>(y),
                                               channel + 317U) *
                            std::sqrt(std::max(0.0F, channelVariance[channel])) * 0.075F;
                        const float texturedTarget = std::clamp(target[channel] + texture, 0.0F, 1.0F);
                        midLowResidualScrubbed.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + texturedTarget * strength;
                    }
                }
            }
        }
        output = std::move(midLowResidualScrubbed);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer midLowLumaCapped = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || centerRatio >= 0.90F || trail.path.size() < 2) {
                continue;
            }

            const float capRadius = std::clamp(trail.width * 0.42F + 1.10F, 1.90F, 2.45F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - capRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + capRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - capRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + capRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > capRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> sideLumaSamples;
                    sideLumaSamples.reserve(8);
                    for (const float offset : {-56.0F, -44.0F, -34.0F, -24.0F, 24.0F, 34.0F, 44.0F, 56.0F}) {
                        const auto sx = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(x) + normalX * offset));
                        const auto sy = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(y) + normalY * offset));
                        if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                            sy >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                        if (sampleDistance.distance <= capRadius + 3.0F) {
                            continue;
                        }
                        const auto samplePixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                            static_cast<std::uint32_t>(sx);
                        if (likelyArtificialColorSample(image, samplePixel) ||
                            likelyNeutralBrightStarSample(image, samplePixel)) {
                            continue;
                        }
                        const auto sampleBase = samplePixel * output.channels;
                        sideLumaSamples.push_back(output.pixels[sampleBase] * 0.2126F +
                                                  output.pixels[sampleBase + 1] * 0.7152F +
                                                  output.pixels[sampleBase + 2] * 0.0722F);
                    }
                    if (sideLumaSamples.size() < 4) {
                        continue;
                    }

                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                               output.pixels[base + 2] * 0.0722F;
                    const float backgroundLuma = median(sideLumaSamples);
                    const float excess = currentLuma - backgroundLuma;
                    if (excess <= 0.0060F) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, capRadius), 0.0F, 1.0F) * 0.50F;
                    const float reduction = std::min(0.012F, (excess - 0.0060F) * 0.36F) * distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        midLowLumaCapped.pixels[base + channel] =
                            std::clamp(output.pixels[base + channel] - reduction, 0.0F, 1.0F);
                    }
                }
            }
        }
        output = std::move(midLowLumaCapped);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer midLowContrastBalanced = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || centerRatio >= 0.90F || trail.path.size() < 2) {
                continue;
            }

            const float balanceRadius = std::clamp(trail.width * 0.40F + 1.05F, 1.85F, 2.40F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - balanceRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + balanceRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - balanceRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + balanceRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > balanceRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> sideLumaSamples;
                    sideLumaSamples.reserve(12);
                    for (const float offset : {-70.0F, -58.0F, -46.0F, -34.0F, -24.0F, -16.0F, 16.0F, 24.0F, 34.0F,
                                               46.0F, 58.0F, 70.0F}) {
                        const auto sx = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(x) + normalX * offset));
                        const auto sy = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(y) + normalY * offset));
                        if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                            sy >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                        if (sampleDistance.distance <= balanceRadius + 3.0F) {
                            continue;
                        }
                        const auto samplePixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                            static_cast<std::uint32_t>(sx);
                        if (likelyArtificialColorSample(image, samplePixel) ||
                            likelyNeutralBrightStarSample(image, samplePixel)) {
                            continue;
                        }
                        const auto sampleBase = samplePixel * output.channels;
                        sideLumaSamples.push_back(output.pixels[sampleBase] * 0.2126F +
                                                  output.pixels[sampleBase + 1] * 0.7152F +
                                                  output.pixels[sampleBase + 2] * 0.0722F);
                    }
                    if (sideLumaSamples.size() < 6) {
                        continue;
                    }

                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                               output.pixels[base + 2] * 0.0722F;
                    const float backgroundLuma = median(sideLumaSamples);
                    const float delta = backgroundLuma - currentLuma;
                    if (std::fabs(delta) <= 0.0060F) {
                        continue;
                    }

                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, balanceRadius), 0.0F, 1.0F) * 0.54F;
                    const float maxAdjustment = delta > 0.0F ? 0.0070F : 0.0100F;
                    const float adjustment =
                        std::clamp(delta * 0.30F, -maxAdjustment, maxAdjustment) * distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        midLowContrastBalanced.pixels[base + channel] =
                            std::clamp(output.pixels[base + channel] + adjustment, 0.0F, 1.0F);
                    }
                }
            }
        }
        output = std::move(midLowContrastBalanced);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer midLowTextureRestored = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || centerRatio >= 0.90F || trail.path.size() < 2) {
                continue;
            }

            const float textureRadius = std::clamp(trail.width * 1.10F + 2.60F, 4.10F, 5.40F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - textureRadius - 4.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + textureRadius + 4.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - textureRadius - 4.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + textureRadius + 4.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > textureRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> channelSamples[3];
                    std::vector<float> lumaSamples;
                    for (auto& samples : channelSamples) {
                        samples.reserve(36);
                    }
                    lumaSamples.reserve(36);
                    for (const float offset : {16.0F, 24.0F, 34.0F, 46.0F, 60.0F, 76.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            for (const float shift : {-4.0F, 0.0F, 4.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * offset * side + distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * offset * side + distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= textureRadius + 3.0F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                const float red = output.pixels[sampleBase];
                                const float green = output.pixels[sampleBase + 1];
                                const float blue = output.pixels[sampleBase + 2];
                                channelSamples[0].push_back(red);
                                channelSamples[1].push_back(green);
                                channelSamples[2].push_back(blue);
                                lumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (lumaSamples.size() < 10) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    float channelStd[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(channelSamples[channel]);
                        const float meanSample =
                            std::accumulate(channelSamples[channel].begin(), channelSamples[channel].end(), 0.0F) /
                            static_cast<float>(channelSamples[channel].size());
                        float variance = 0.0F;
                        for (const float sample : channelSamples[channel]) {
                            const float delta = sample - meanSample;
                            variance += delta * delta;
                        }
                        variance /= static_cast<float>(channelSamples[channel].size());
                        channelStd[channel] = std::sqrt(std::max(0.0F, variance));
                    }

                    const auto base = pixel * output.channels;
                    const float current[3] = {output.pixels[base], output.pixels[base + 1], output.pixels[base + 2]};
                    const float currentLuma = current[0] * 0.2126F + current[1] * 0.7152F + current[2] * 0.0722F;
                    const float targetLuma = median(lumaSamples);
                    float colorResidual = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        colorResidual = std::max(colorResidual, std::fabs(current[channel] - target[channel]));
                    }

                    const float lumaDelta = targetLuma - currentLuma;
                    const bool needsToneRepair = std::fabs(lumaDelta) > 0.0032F || colorResidual > 0.0060F;
                    const float distanceWeight =
                        1.0F - std::clamp(distance.distance / std::max(0.1F, textureRadius), 0.0F, 1.0F) * 0.62F;
                    const float toneStrength = needsToneRepair ? 0.10F * distanceWeight : 0.035F * distanceWeight;
                    const float textureStrength = (needsToneRepair ? 0.18F : 0.12F) * distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float noise =
                            deterministicNoise(static_cast<std::uint32_t>(x),
                                               static_cast<std::uint32_t>(y),
                                               static_cast<std::uint16_t>(channel + 467U)) *
                            channelStd[channel] * textureStrength;
                        const float toned = current[channel] * (1.0F - toneStrength) + target[channel] * toneStrength;
                        midLowTextureRestored.pixels[base + channel] = std::clamp(toned + noise, 0.0F, 1.0F);
                    }
                }
            }
        }
        output = std::move(midLowTextureRestored);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.93);

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer midLowShoulderBalanced = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.80F || centerRatio >= 0.90F || trail.path.size() < 2) {
                continue;
            }

            const float shoulderRadius = std::clamp(trail.width * 1.90F + 7.00F, 9.50F, 12.20F);
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - shoulderRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + shoulderRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - shoulderRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + shoulderRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > shoulderRadius || distance.distance < 3.40F) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> sideLumaSamples;
                    sideLumaSamples.reserve(12);
                    for (const float offset : {-92.0F, -78.0F, -64.0F, -52.0F, -40.0F, -28.0F, 28.0F, 40.0F, 52.0F,
                                               64.0F, 78.0F, 92.0F}) {
                        const auto sx = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(x) + normalX * offset));
                        const auto sy = static_cast<std::int32_t>(
                            std::lround(static_cast<float>(y) + normalY * offset));
                        if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                            sy >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                        if (sampleDistance.distance <= shoulderRadius + 3.0F) {
                            continue;
                        }
                        const auto samplePixel =
                            static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                            static_cast<std::uint32_t>(sx);
                        if (likelyArtificialColorSample(image, samplePixel) ||
                            likelyNeutralBrightStarSample(image, samplePixel)) {
                            continue;
                        }
                        const auto sampleBase = samplePixel * output.channels;
                        sideLumaSamples.push_back(output.pixels[sampleBase] * 0.2126F +
                                                  output.pixels[sampleBase + 1] * 0.7152F +
                                                  output.pixels[sampleBase + 2] * 0.0722F);
                    }
                    if (sideLumaSamples.size() < 6) {
                        continue;
                    }

                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                               output.pixels[base + 2] * 0.0722F;
                    const float targetLuma = median(sideLumaSamples);
                    const float delta = targetLuma - currentLuma;
                    if (std::fabs(delta) < 0.0032F) {
                        continue;
                    }

                    const float innerRamp = std::clamp((distance.distance - 3.40F) / 3.00F, 0.0F, 1.0F);
                    const float outerRamp =
                        std::clamp((shoulderRadius - distance.distance) / std::max(1.0F, shoulderRadius - 7.00F), 0.0F, 1.0F);
                    const float shoulderWeight = innerRamp * outerRamp;
                    if (shoulderWeight <= 0.0F) {
                        continue;
                    }

                    const float adjustment = std::clamp(delta * 0.34F, -0.0080F, 0.0100F) * shoulderWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        midLowShoulderBalanced.pixels[base + channel] =
                            std::clamp(output.pixels[base + channel] + adjustment, 0.0F, 1.0F);
                    }
                }
            }
        }
        output = std::move(midLowShoulderBalanced);
    }

    if (output.channels >= 3 && !removedMidLowArtificialTrails.empty()) {
        ImageBuffer endpointBandTextured = output;
        for (const auto& trail : removedMidLowArtificialTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            const bool midLowTrail = centerRatio >= 0.60F && centerRatio < 0.90F && trail.length >= 160.0F &&
                                     trail.length <= 760.0F;
            const bool lowHorizonTrail = centerRatio >= 0.93F && centerRatio <= 0.985F && trail.length >= 150.0F &&
                                         trail.length <= 720.0F;
            const bool bottomEdgeHorizonTrail = trail.kind == ArtifactTrailKind::Airplane && trail.path.size() < 2 &&
                                                centerRatio >= 0.955F && centerRatio <= 0.995F &&
                                                trail.length >= 120.0F && trail.length <= 4000.0F;
            const bool lowHorizonEndpointDrone = trail.kind == ArtifactTrailKind::Drone && trail.path.size() < 2 &&
                                                 centerRatio >= 0.90F && centerRatio <= 0.955F &&
                                                 trail.length >= 180.0F && trail.length <= 1300.0F;
            const bool midShortEndpointDrone = trail.kind == ArtifactTrailKind::Drone && trail.path.size() < 2 &&
                                               centerRatio >= 0.60F && centerRatio < 0.74F &&
                                               trail.length >= 150.0F && trail.length <= 420.0F;
            if (!midLowTrail && !lowHorizonTrail && !lowHorizonEndpointDrone && !bottomEdgeHorizonTrail) {
                continue;
            }

            const float bandRadius = (lowHorizonTrail || lowHorizonEndpointDrone)
                                         ? std::clamp(trail.width * 1.80F + 5.50F, 7.0F, 11.0F)
                                         : (midShortEndpointDrone
                                                ? std::clamp(trail.width * 1.05F + 4.10F, 6.20F, 8.20F)
                                                : std::clamp(trail.width * 2.20F + 9.50F, 12.5F, 17.5F));
            const auto minX = static_cast<std::int32_t>(std::floor(std::min(trail.x1, trail.x2) - bandRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(std::max(trail.x1, trail.x2) + bandRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(std::min(trail.y1, trail.y2) - bandRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(std::max(trail.y1, trail.y2) + bandRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > bandRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& samples : targetSamples) {
                        samples.reserve(30);
                    }
                    targetLumaSamples.reserve(30);
                    const std::array<float, 5> sampleOffsets =
                        (lowHorizonTrail || lowHorizonEndpointDrone)
                            ? std::array<float, 5>{14.0F, 22.0F, 34.0F, 50.0F, 70.0F}
                            : std::array<float, 5>{26.0F, 36.0F, 50.0F, 68.0F, 88.0F};
                    const bool sampleSameSideOnly =
                        bottomEdgeHorizonTrail && std::fabs(distance.signedDistance) > 0.65F;
                    const float preferredSide = distance.signedDistance >= 0.0F ? 1.0F : -1.0F;
                    for (const float sampleOffset : sampleOffsets) {
                        for (const float side : {-1.0F, 1.0F}) {
                            if (sampleSameSideOnly && side != preferredSide) {
                                continue;
                            }
                            for (const float tangentShift : {-5.0F, 0.0F, 5.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * sampleOffset * side +
                                                distance.unitX * tangentShift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * sampleOffset * side +
                                                distance.unitY * tangentShift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= bandRadius + 4.0F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                const float red = output.pixels[sampleBase];
                                const float green = output.pixels[sampleBase + 1];
                                const float blue = output.pixels[sampleBase + 2];
                                targetSamples[0].push_back(red);
                                targetSamples[1].push_back(green);
                                targetSamples[2].push_back(blue);
                                targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (targetLumaSamples.size() < 8) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    float channelStd[3] = {0.0F, 0.0F, 0.0F};
                    const float targetPercentile = bottomEdgeHorizonTrail ? 0.64F : 0.50F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] =
                            bottomEdgeHorizonTrail ? percentileValue(targetSamples[channel], targetPercentile)
                                                   : median(targetSamples[channel]);
                        const float meanSample =
                            std::accumulate(targetSamples[channel].begin(), targetSamples[channel].end(), 0.0F) /
                            static_cast<float>(targetSamples[channel].size());
                        float variance = 0.0F;
                        for (const float sample : targetSamples[channel]) {
                            const float delta = sample - meanSample;
                            variance += delta * delta;
                        }
                        variance /= static_cast<float>(targetSamples[channel].size());
                        channelStd[channel] = std::sqrt(std::max(0.0F, variance));
                    }

                    const auto base = pixel * output.channels;
                    const float current[3] = {output.pixels[base], output.pixels[base + 1], output.pixels[base + 2]};
                    const float currentLuma = current[0] * 0.2126F + current[1] * 0.7152F + current[2] * 0.0722F;
                    const float targetLuma =
                        bottomEdgeHorizonTrail ? percentileValue(targetLumaSamples, targetPercentile) : median(targetLumaSamples);
                    float colorResidual = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        colorResidual = std::max(colorResidual, std::fabs(current[channel] - target[channel]));
                    }

                    const float normalizedDistance = distance.distance / std::max(0.1F, bandRadius);
                    const float distanceWeight = std::pow(1.0F - std::clamp(normalizedDistance, 0.0F, 1.0F),
                                                          (lowHorizonTrail || lowHorizonEndpointDrone) ? 0.54F : 0.62F);
                    const bool darkerMidLowBand = midLowTrail && currentLuma + 0.0022F < targetLuma;
                    const float lumaThreshold = lowHorizonEndpointDrone
                                                    ? 0.0024F
                                                    : (lowHorizonTrail ? 0.0032F : (midShortEndpointDrone ? 0.0048F : 0.0036F));
                    const float colorThreshold = lowHorizonEndpointDrone
                                                     ? 0.0048F
                                                     : (lowHorizonTrail ? 0.0062F : (midShortEndpointDrone ? 0.0080F : 0.0068F));
                    const bool visibleBand =
                        bottomEdgeHorizonTrail ||
                        (darkerMidLowBand && (!midShortEndpointDrone || distance.distance <= bandRadius * 0.72F)) ||
                        std::fabs(currentLuma - targetLuma) > lumaThreshold || colorResidual > colorThreshold;
                    if (!visibleBand) {
                        continue;
                    }
                    const float originalPointEvidence = localDottedPointScore(
                        trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                    const bool bottomEdgeCore =
                        bottomEdgeHorizonTrail &&
                        distance.distance <= std::clamp(trail.width * 0.72F + 1.20F, 2.50F, 4.20F);
                    const bool originalArtificial =
                        bottomEdgeCore || likelyArtificialColorSample(image, pixel) ||
                        originalPointEvidence >
                            (bottomEdgeHorizonTrail ? 0.0022F : (lowHorizonTrail ? 0.0032F : 0.0045F));
                    const float originalRestoreStrength =
                        bottomEdgeHorizonTrail && !originalArtificial
                            ? 0.0F
                            : (midLowTrail && !originalArtificial ? 0.22F * distanceWeight : 0.0F);
                    const float toneStrength =
                        (bottomEdgeHorizonTrail
                             ? (originalArtificial ? 0.86F : 0.035F)
                             : (lowHorizonEndpointDrone
                                    ? 0.74F
                                    : (lowHorizonTrail
                                           ? 0.58F
                                                        : (midShortEndpointDrone ? (darkerMidLowBand ? 0.50F : 0.42F)
                                                                                 : (darkerMidLowBand ? 0.68F : 0.54F))))) *
                        distanceWeight;
                    const float textureStrength =
                        (bottomEdgeHorizonTrail
                             ? (originalArtificial ? 0.028F : 0.006F)
                             : (lowHorizonEndpointDrone
                                    ? 0.045F
                                    : (lowHorizonTrail ? 0.055F : (midShortEndpointDrone ? 0.095F : 0.060F)))) *
                        distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float noise =
                            deterministicNoise(static_cast<std::uint32_t>(x),
                                               static_cast<std::uint32_t>(y),
                                               static_cast<std::uint16_t>(channel + 607U)) *
                            channelStd[channel] * textureStrength;
                        const float originalValue = image.pixels[base + channel];
                        const float restored =
                            current[channel] * (1.0F - originalRestoreStrength) + originalValue * originalRestoreStrength;
                        const float toned = restored * (1.0F - toneStrength) + target[channel] * toneStrength;
                        endpointBandTextured.pixels[base + channel] = std::clamp(toned + noise, 0.0F, 1.0F);
                    }
                }
            }
        }
        output = std::move(endpointBandTextured);
    }

    if (output.channels >= 3 && !removedMidLowArtificialTrails.empty()) {
        ImageBuffer midShortCoreMatched = output;
        for (const auto& trail : removedMidLowArtificialTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            const bool midShortEndpointDrone = trail.kind == ArtifactTrailKind::Drone && trail.path.size() < 2 &&
                                               centerRatio >= 0.60F && centerRatio < 0.74F &&
                                               trail.length >= 140.0F && trail.length <= 460.0F;
            const bool midLowerShortEndpointDrone = trail.kind == ArtifactTrailKind::Drone && trail.path.size() < 2 &&
                                                    centerRatio >= 0.78F && centerRatio <= 0.88F &&
                                                    trail.length >= 220.0F && trail.length <= 340.0F;
            const bool bottomEdgeHorizonAirplane = trail.kind == ArtifactTrailKind::Airplane && trail.path.size() < 2 &&
                                                   centerRatio >= 0.955F && centerRatio <= 0.995F &&
                                                   trail.length >= 120.0F && trail.length <= 4000.0F;
            if (!midShortEndpointDrone && !midLowerShortEndpointDrone && !bottomEdgeHorizonAirplane) {
                continue;
            }
            const float coreRadius = bottomEdgeHorizonAirplane
                                         ? std::clamp(trail.width * 1.35F + 4.60F, 7.20F, 9.80F)
                                         : (midLowerShortEndpointDrone
                                                ? std::clamp(trail.width * 0.92F + 2.70F, 4.80F, 6.40F)
                                                : std::clamp(trail.width * 1.55F + 4.80F, 8.20F, 10.80F));
            const auto minX = static_cast<std::int32_t>(std::floor(std::min(trail.x1, trail.x2) - coreRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(std::max(trail.x1, trail.x2) + coreRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(std::min(trail.y1, trail.y2) - coreRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(std::max(trail.y1, trail.y2) + coreRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > coreRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    if (likelyNeutralBrightStarSample(image, pixel)) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& samples : targetSamples) {
                        samples.reserve(24);
                    }
                    targetLumaSamples.reserve(24);
                    const std::array<float, 5> repairOffsets =
                        bottomEdgeHorizonAirplane
                            ? std::array<float, 5>{14.0F, 24.0F, 38.0F, 56.0F, 78.0F}
                            : (midLowerShortEndpointDrone ? std::array<float, 5>{20.0F, 34.0F, 52.0F, 72.0F, 96.0F}
                                                          : std::array<float, 5>{24.0F, 40.0F, 62.0F, 88.0F, 118.0F});
                    const bool sampleSameSideOnly =
                        bottomEdgeHorizonAirplane && std::fabs(distance.signedDistance) > 0.65F;
                    const float preferredSide = distance.signedDistance >= 0.0F ? 1.0F : -1.0F;
                    for (const float offset : repairOffsets) {
                        for (const float side : {-1.0F, 1.0F}) {
                            if (sampleSameSideOnly && side != preferredSide) {
                                continue;
                            }
                            for (const float shift : {-5.0F, 0.0F, 5.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * offset * side +
                                                distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * offset * side +
                                                distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= coreRadius + 3.0F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                const float red = output.pixels[sampleBase];
                                const float green = output.pixels[sampleBase + 1];
                                const float blue = output.pixels[sampleBase + 2];
                                targetSamples[0].push_back(red);
                                targetSamples[1].push_back(green);
                                targetSamples[2].push_back(blue);
                                targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (targetLumaSamples.size() < 4) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    float channelStd[3] = {0.0F, 0.0F, 0.0F};
                    const float targetPercentile =
                        bottomEdgeHorizonAirplane ? 0.64F : (midLowerShortEndpointDrone ? 0.48F : 0.55F);
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = percentileValue(targetSamples[channel], targetPercentile);
                        const float meanSample =
                            std::accumulate(targetSamples[channel].begin(), targetSamples[channel].end(), 0.0F) /
                            static_cast<float>(targetSamples[channel].size());
                        float variance = 0.0F;
                        for (const float sample : targetSamples[channel]) {
                            const float delta = sample - meanSample;
                            variance += delta * delta;
                        }
                        channelStd[channel] = std::sqrt(std::max(0.0F, variance / static_cast<float>(targetSamples[channel].size())));
                    }

                    const auto base = pixel * output.channels;
                    const float current[3] = {output.pixels[base], output.pixels[base + 1], output.pixels[base + 2]};
                    const float currentLuma = current[0] * 0.2126F + current[1] * 0.7152F + current[2] * 0.0722F;
                    const float targetLuma = percentileValue(targetLumaSamples, targetPercentile);
                    float colorResidual = 0.0F;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        colorResidual = std::max(colorResidual, std::fabs(current[channel] - target[channel]));
                    }
                    const float distanceWeight =
                        std::pow(1.0F - std::clamp(distance.distance / std::max(0.1F, coreRadius), 0.0F, 1.0F), 0.70F);
                    const float originalPointEvidence = localDottedPointScore(
                        trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                    const bool bottomEdgeCore =
                        bottomEdgeHorizonAirplane &&
                        distance.distance <= std::clamp(trail.width * 0.72F + 1.20F, 2.50F, 4.20F);
                    const bool originalArtificial =
                        bottomEdgeCore || likelyArtificialColorSample(image, pixel) ||
                        originalPointEvidence > (bottomEdgeHorizonAirplane ? 0.0022F : 0.0048F);
                    const float evidence = std::max(std::fabs(currentLuma - targetLuma), colorResidual * 0.75F);
                    const bool visibleMismatch = std::fabs(currentLuma - targetLuma) >= 0.0016F || colorResidual >= 0.0038F;
                    const float baseToneStrength =
                        bottomEdgeHorizonAirplane
                            ? (originalArtificial ? 0.90F : 0.025F)
                            : (midLowerShortEndpointDrone ? (originalArtificial ? 0.58F : 0.34F)
                                                          : (originalArtificial ? 0.70F : 0.54F));
                    const float evidenceToneStrength =
                        bottomEdgeHorizonAirplane && !originalArtificial
                            ? 0.0F
                            : (visibleMismatch ? std::clamp((evidence - 0.0008F) / 0.009F, 0.34F, 0.94F) : 0.0F);
                    const float toneStrength = std::max(baseToneStrength, evidenceToneStrength) * distanceWeight;
                    const float textureStrength =
                        (bottomEdgeHorizonAirplane
                             ? (originalArtificial ? 0.040F : 0.006F)
                             : (midLowerShortEndpointDrone ? (visibleMismatch ? 0.11F : 0.04F)
                                                           : (visibleMismatch ? 0.18F : 0.07F))) *
                        distanceWeight;
                    const float originalRestoreStrength =
                        originalArtificial ? 0.0F
                                           : (bottomEdgeHorizonAirplane
                                                  ? 0.0F
                                                  : (midLowerShortEndpointDrone ? (visibleMismatch ? 0.28F : 0.42F)
                                                                               : (visibleMismatch ? 0.72F : 0.88F))) *
                                                 distanceWeight;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float noise =
                            deterministicNoise(static_cast<std::uint32_t>(x),
                                               static_cast<std::uint32_t>(y),
                                               static_cast<std::uint16_t>(channel + 719U)) *
                            channelStd[channel] * textureStrength;
                        const float restored =
                            current[channel] * (1.0F - originalRestoreStrength) +
                            image.pixels[base + channel] * originalRestoreStrength;
                        const float toned = restored * (1.0F - toneStrength) + target[channel] * toneStrength;
                        midShortCoreMatched.pixels[base + channel] = std::clamp(toned + noise, 0.0F, 1.0F);
                    }
                }
            }
        }
        output = std::move(midShortCoreMatched);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.95);

    if (output.channels >= 3 && !removedMidLowArtificialTrails.empty()) {
        ImageBuffer bottomEdgeScratchMatched = output;
        for (const auto& trail : removedMidLowArtificialTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            const bool bottomEdgeHorizonAirplane = trail.kind == ArtifactTrailKind::Airplane && trail.path.size() < 2 &&
                                                   centerRatio >= 0.955F && centerRatio <= 0.995F &&
                                                   trail.length >= 120.0F && trail.length <= 4000.0F;
            if (!bottomEdgeHorizonAirplane) {
                continue;
            }

            const float coreRadius = std::clamp(trail.width * 0.65F + 1.15F, 2.25F, 3.80F);
            const auto minX = static_cast<std::int32_t>(std::floor(std::min(trail.x1, trail.x2) - coreRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(std::max(trail.x1, trail.x2) + coreRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(std::min(trail.y1, trail.y2) - coreRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(std::max(trail.y1, trail.y2) + coreRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > coreRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float originalPointEvidence = localDottedPointScore(
                        trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                    if (likelyNeutralBrightStarSample(image, pixel) && originalPointEvidence < 0.0035F) {
                        continue;
                    }

                    std::vector<float> targetSamples[3];
                    for (auto& samples : targetSamples) {
                        samples.reserve(40);
                    }
                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    for (const float offset : {18.0F, 28.0F, 42.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            for (const float shift : {-7.0F, 0.0F, 7.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * offset * side +
                                                distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * offset * side +
                                                distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance =
                                    distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= coreRadius + 1.25F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                targetSamples[0].push_back(output.pixels[sampleBase]);
                                targetSamples[1].push_back(output.pixels[sampleBase + 1]);
                                targetSamples[2].push_back(output.pixels[sampleBase + 2]);
                            }
                        }
                    }
                    if (targetSamples[0].size() < 12 || targetSamples[1].size() < 12 || targetSamples[2].size() < 12) {
                        continue;
                    }

                    float target[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = percentileValue(targetSamples[channel], 0.62F);
                    }
                    const float distanceWeight =
                        std::pow(1.0F - std::clamp(distance.distance / std::max(0.1F, coreRadius), 0.0F, 1.0F), 0.52F);
                    const float strength = std::clamp(0.58F + distanceWeight * 0.40F, 0.58F, 0.98F);
                    const auto base = pixel * output.channels;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        bottomEdgeScratchMatched.pixels[base + channel] =
                            output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                    }
                }
            }
        }
        output = std::move(bottomEdgeScratchMatched);
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        for (std::uint8_t microPass = 0; microPass < 2; ++microPass) {
            const bool conservativePass = microPass == 1;
            ImageBuffer microMatched = output;
            for (const auto& trail : removedDottedDroneTrails) {
                const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
                if (centerRatio < 0.90F || trail.path.size() < 2) {
                    continue;
                }

                const float microRadius = conservativePass ? std::clamp(trail.width * 0.82F + 3.6F, 4.2F, 5.6F)
                                                           : std::clamp(trail.width * 0.92F + 4.4F, 4.8F, 6.7F);
                float minTrailX = std::min(trail.x1, trail.x2);
                float maxTrailX = std::max(trail.x1, trail.x2);
                float minTrailY = std::min(trail.y1, trail.y2);
                float maxTrailY = std::max(trail.y1, trail.y2);
                for (const auto& point : trail.path) {
                    minTrailX = std::min(minTrailX, point.x);
                    maxTrailX = std::max(maxTrailX, point.x);
                    minTrailY = std::min(minTrailY, point.y);
                    maxTrailY = std::max(maxTrailY, point.y);
                }

                const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - microRadius - 2.0F));
                const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + microRadius + 2.0F));
                const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - microRadius - 2.0F));
                const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + microRadius + 2.0F));
                for (std::int32_t y = minY; y <= maxY; ++y) {
                    for (std::int32_t x = minX; x <= maxX; ++x) {
                        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                            y >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }

                        const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                        if (distance.distance > microRadius) {
                            continue;
                        }

                        const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                           static_cast<std::uint32_t>(x);
                        if (likelyNeutralBrightStarSample(image, pixel)) {
                            continue;
                        }

                        const float normalX = -distance.unitY;
                        const float normalY = distance.unitX;
                        std::vector<float> targetSamples[3];
                        std::vector<float> targetLumaSamples;
                        for (auto& channelSamples : targetSamples) {
                            channelSamples.reserve(30);
                        }
                        targetLumaSamples.reserve(30);
                        for (const float baseOffset : {10.0F, 16.0F, 25.0F, 38.0F, 52.0F}) {
                            const float offset = conservativePass ? (baseOffset < 12.0F   ? 12.0F
                                                                     : baseOffset < 20.0F ? 18.0F
                                                                     : baseOffset < 32.0F ? 28.0F
                                                                     : baseOffset < 46.0F ? 42.0F
                                                                                          : 58.0F)
                                                                  : baseOffset;
                            for (const float side : {-1.0F, 1.0F}) {
                                for (const float shift : {-6.0F, 0.0F, 6.0F}) {
                                    const float adjustedShift = conservativePass ? shift * 1.1666666F : shift;
                                    const auto sx = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(x) + normalX * offset * side +
                                                    distance.unitX * adjustedShift));
                                    const auto sy = static_cast<std::int32_t>(
                                        std::lround(static_cast<float>(y) + normalY * offset * side +
                                                    distance.unitY * adjustedShift));
                                    if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                        sy >= static_cast<std::int32_t>(image.height)) {
                                        continue;
                                    }
                                    const auto sampleDistance =
                                        distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                    if (sampleDistance.distance <= microRadius + 2.5F) {
                                        continue;
                                    }
                                    const auto samplePixel =
                                        static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                        static_cast<std::uint32_t>(sx);
                                    if (likelyArtificialColorSample(image, samplePixel)) {
                                        continue;
                                    }
                                    const auto sampleBase = samplePixel * output.channels;
                                    const float red = output.pixels[sampleBase];
                                    const float green = output.pixels[sampleBase + 1];
                                    const float blue = output.pixels[sampleBase + 2];
                                    targetSamples[0].push_back(red);
                                    targetSamples[1].push_back(green);
                                    targetSamples[2].push_back(blue);
                                    targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                }
                            }
                        }
                        if (targetLumaSamples.size() < 8) {
                            continue;
                        }

                        float target[3] = {0.0F, 0.0F, 0.0F};
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            target[channel] = percentileValue(targetSamples[channel], conservativePass ? 0.36F : 0.32F);
                        }
                        const float targetLuma = percentileValue(targetLumaSamples, conservativePass ? 0.36F : 0.32F);
                        const auto base = pixel * output.channels;
                        const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                                  output.pixels[base + 2] * 0.0722F;
                        const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                                   image.pixels[base + 2] * 0.0722F;
                        const float currentExcess = currentLuma - targetLuma;
                        const float originalExcess = originalLuma - targetLuma;
                        const float originalPointEvidence = localDottedPointScore(
                            trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                        const float originalColorEvidence =
                            navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 1.2F +
                            std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.24F;
                        const bool pathCore = distance.distance <= microRadius * (conservativePass ? 0.35F : 0.60F);
                        const bool originalBacked = originalPointEvidence > (conservativePass ? 0.0032F : 0.0024F) ||
                                                    originalColorEvidence > (conservativePass ? 0.0018F : 0.0016F) ||
                                                    originalExcess > (conservativePass ? 0.0120F : 0.0080F);
                        const bool residualBacked =
                            currentExcess > (conservativePass ? 0.0056F : 0.0028F) &&
                            (pathCore || originalBacked || originalExcess > (conservativePass ? 0.0090F : 0.0055F));
                        if (!residualBacked) {
                            continue;
                        }

                        const float distanceWeight =
                            1.0F - std::clamp(distance.distance / std::max(0.1F, microRadius), 0.0F, 1.0F) *
                                       (conservativePass ? 0.72F : 0.62F);
                        const float evidence =
                            conservativePass ? std::max({currentExcess, originalExcess * 0.24F,
                                                         originalPointEvidence * 0.85F, originalColorEvidence * 1.35F})
                                             : std::max({currentExcess, originalExcess * 0.34F,
                                                         originalPointEvidence * 1.16F, originalColorEvidence * 1.90F});
                        float strength = conservativePass
                                             ? std::clamp((evidence - 0.0042F) / 0.018F, 0.0F, 0.54F) * distanceWeight
                                             : std::clamp((evidence - 0.0017F) / 0.018F, 0.0F, 0.90F) * distanceWeight;
                        if (originalBacked && currentExcess > (conservativePass ? 0.0070F : 0.0038F)) {
                            strength = std::max(strength, (conservativePass ? 0.32F : 0.50F) * distanceWeight);
                        }
                        if (currentExcess > (conservativePass ? 0.0100F : 0.0085F)) {
                            strength = std::max(strength, (conservativePass ? 0.45F : 0.70F) * distanceWeight);
                        }
                        strength = std::clamp(strength, 0.0F, conservativePass ? 0.56F : 0.90F);
                        if (strength <= 0.0F) {
                            continue;
                        }
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            microMatched.pixels[base + channel] =
                                output.pixels[base + channel] * (1.0F - strength) + target[channel] * strength;
                        }
                    }
                }
            }
            output = std::move(microMatched);
        }
    }

    if (output.channels >= 3 && !removedDottedDroneTrails.empty()) {
        ImageBuffer cappedResiduals = output;
        for (const auto& trail : removedDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || trail.path.size() < 2) {
                continue;
            }

            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); ++step) {
                    const float centerX = start.x + unitX * static_cast<float>(step);
                    const float centerY = start.y + unitY * static_cast<float>(step);

                    float bestResidual = -1.0F;
                    float bestX = centerX;
                    float bestY = centerY;
                    float bestBackground = 0.0F;
                    for (const float normalOffset : {-2.0F, -1.0F, 0.0F, 1.0F, 2.0F}) {
                        const float px = centerX + normalX * normalOffset;
                        const float py = centerY + normalY * normalOffset;
                        const auto x = static_cast<std::int32_t>(std::lround(px));
                        const auto y = static_cast<std::int32_t>(std::lround(py));
                        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                            y >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                           static_cast<std::uint32_t>(x);
                        const auto base = pixel * cappedResiduals.channels;
                        const float currentLuma = cappedResiduals.pixels[base] * 0.2126F +
                                                  cappedResiduals.pixels[base + 1] * 0.7152F +
                                                  cappedResiduals.pixels[base + 2] * 0.0722F;

                        std::vector<float> backgroundSamples;
                        backgroundSamples.reserve(8);
                        for (const float offset : {10.0F, 16.0F, 24.0F, 36.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                const auto sx = static_cast<std::int32_t>(std::lround(px + normalX * offset * side));
                                const auto sy = static_cast<std::int32_t>(std::lround(py + normalY * offset * side));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                const auto sampleBase = samplePixel * cappedResiduals.channels;
                                backgroundSamples.push_back(cappedResiduals.pixels[sampleBase] * 0.2126F +
                                                            cappedResiduals.pixels[sampleBase + 1] * 0.7152F +
                                                            cappedResiduals.pixels[sampleBase + 2] * 0.0722F);
                            }
                        }
                        if (backgroundSamples.size() < 4) {
                            continue;
                        }
                        const float background = median(backgroundSamples);
                        const float residual = currentLuma - background;
                        if (residual > bestResidual) {
                            bestResidual = residual;
                            bestX = px;
                            bestY = py;
                            bestBackground = background;
                        }
                    }
                    if (bestResidual <= 0.0052F) {
                        continue;
                    }

                    const auto bestPixel = nearestPixelIndex(image, bestX, bestY);
                    if (likelyNeutralBrightStarSample(image, bestPixel)) {
                        continue;
                    }
                    const auto originalBase = bestPixel * image.channels;
                    const float originalLuma = image.pixels[originalBase] * 0.2126F +
                                               image.pixels[originalBase + 1] * 0.7152F +
                                               image.pixels[originalBase + 2] * 0.0722F;
                    const float originalColorEvidence =
                        navigationLightScoreAt(image, bestPixel) + warmExcessAt(image, bestPixel) * 1.2F +
                        std::sqrt(std::max(0.0F, colorVarianceAt(image, bestPixel))) * 0.24F;
                    if (originalLuma - bestBackground <= 0.0080F && originalColorEvidence <= 0.0014F) {
                        continue;
                    }

                    std::vector<float> targetSamples[3];
                    for (auto& samples : targetSamples) {
                        samples.reserve(8);
                    }
                    for (const float offset : {10.0F, 16.0F, 24.0F, 36.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            const auto sx = static_cast<std::int32_t>(std::lround(bestX + normalX * offset * side));
                            const auto sy = static_cast<std::int32_t>(std::lround(bestY + normalY * offset * side));
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            const auto sampleBase = samplePixel * cappedResiduals.channels;
                            targetSamples[0].push_back(cappedResiduals.pixels[sampleBase]);
                            targetSamples[1].push_back(cappedResiduals.pixels[sampleBase + 1]);
                            targetSamples[2].push_back(cappedResiduals.pixels[sampleBase + 2]);
                        }
                    }
                    if (targetSamples[0].size() < 4 || targetSamples[1].size() < 4 || targetSamples[2].size() < 4) {
                        continue;
                    }
                    float target[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        target[channel] = median(targetSamples[channel]);
                    }
                    const float targetLuma = target[0] * 0.2126F + target[1] * 0.7152F + target[2] * 0.0722F;
                    const float lumaCap = targetLuma + 0.0032F;
                    const auto centerPixelX = static_cast<std::int32_t>(std::lround(bestX));
                    const auto centerPixelY = static_cast<std::int32_t>(std::lround(bestY));
                    for (std::uint8_t repeat = 0; repeat < 2; ++repeat) {
                        bool localChanged = false;
                        for (std::int32_t dy = -1; dy <= 1; ++dy) {
                            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                                if (std::abs(dx) + std::abs(dy) > 2) {
                                    continue;
                                }
                                const auto x = centerPixelX + dx;
                                const auto y = centerPixelY + dy;
                                if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                                    y >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto pixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                    static_cast<std::uint32_t>(x);
                                if (likelyNeutralBrightStarSample(image, pixel)) {
                                    continue;
                                }
                                const auto base = pixel * cappedResiduals.channels;
                                const float currentLuma = cappedResiduals.pixels[base] * 0.2126F +
                                                          cappedResiduals.pixels[base + 1] * 0.7152F +
                                                          cappedResiduals.pixels[base + 2] * 0.0722F;
                                const float excess = currentLuma - lumaCap;
                                if (excess <= 0.0F) {
                                    continue;
                                }
                                const float denominator = std::max(0.001F, currentLuma - targetLuma);
                                float strength = std::clamp(excess / denominator, 0.18F, 0.75F);
                                const auto manhattan = std::abs(dx) + std::abs(dy);
                                if (manhattan == 1) {
                                    strength *= 0.62F;
                                } else if (manhattan == 2) {
                                    strength *= 0.38F;
                                }
                                for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                    cappedResiduals.pixels[base + channel] =
                                        cappedResiduals.pixels[base + channel] * (1.0F - strength) +
                                        target[channel] * strength;
                                }
                                localChanged = true;
                            }
                        }
                        if (!localChanged) {
                            break;
                        }
                    }
                }
            }
        }
        output = std::move(cappedResiduals);
    }

    if (output.channels >= 3 && !removedHorizonDottedDroneTrails.empty()) {
        ImageBuffer offAxisResiduals = output;
        for (const auto& trail : removedHorizonDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || trail.path.size() < 2 || trail.length < 520.0F) {
                continue;
            }

            const float snapRadius = std::clamp(trail.width * 1.80F + 7.0F, 8.0F, 12.0F);
            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;
                const float normalX = -unitY;
                const float normalY = unitX;

                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); step += 4) {
                    const float centerX = start.x + unitX * static_cast<float>(step);
                    const float centerY = start.y + unitY * static_cast<float>(step);

                    float bestScore = -1.0F;
                    float bestX = centerX;
                    float bestY = centerY;
                    float bestTarget[3] = {0.0F, 0.0F, 0.0F};
                    float bestTargetLuma = 0.0F;
                    for (float normalOffset = -snapRadius; normalOffset <= snapRadius + 0.01F; normalOffset += 2.0F) {
                        const float px = centerX + normalX * normalOffset;
                        const float py = centerY + normalY * normalOffset;
                        const auto x = static_cast<std::int32_t>(std::lround(px));
                        const auto y = static_cast<std::int32_t>(std::lround(py));
                        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                            y >= static_cast<std::int32_t>(image.height)) {
                            continue;
                        }
                        const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                           static_cast<std::uint32_t>(x);

                        const auto base = pixel * output.channels;
                        const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                                  output.pixels[base + 2] * 0.0722F;
                        const float originalLuma = image.pixels[base] * 0.2126F + image.pixels[base + 1] * 0.7152F +
                                                   image.pixels[base + 2] * 0.0722F;

                        std::vector<float> backgroundSamples;
                        backgroundSamples.reserve(6);
                        for (const float sideOffset : {18.0F, 28.0F, 42.0F}) {
                            for (const float side : {-1.0F, 1.0F}) {
                                const auto sx = static_cast<std::int32_t>(std::lround(px + normalX * sideOffset * side));
                                const auto sy = static_cast<std::int32_t>(std::lround(py + normalY * sideOffset * side));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance =
                                    distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= snapRadius + 2.0F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                backgroundSamples.push_back(output.pixels[sampleBase] * 0.2126F +
                                                            output.pixels[sampleBase + 1] * 0.7152F +
                                                            output.pixels[sampleBase + 2] * 0.0722F);
                            }
                        }
                        if (backgroundSamples.size() < 4) {
                            continue;
                        }

                        const float targetLuma = percentileValue(backgroundSamples, 0.38F);
                        const float currentExcess = currentLuma - targetLuma;
                        const float originalExcess = originalLuma - targetLuma;
                        const float originalPointEvidence = localDottedPointScore(
                            trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                        const float originalColorEvidence =
                            navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 1.25F +
                            std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.25F;
                        const bool colorBacked = originalColorEvidence > 0.0015F;
                        const bool centerBacked = std::fabs(normalOffset) <= 4.5F && originalPointEvidence > 0.0032F;
                        const bool artificialBacked = colorBacked || centerBacked;
                        if (!artificialBacked && currentExcess <= 0.0054F) {
                            continue;
                        }
                        if (std::fabs(normalOffset) > 5.0F && !colorBacked) {
                            continue;
                        }
                        if (likelyNeutralBrightStarSample(image, pixel) && !artificialBacked) {
                            continue;
                        }

                        const float lateralPenalty = std::fabs(normalOffset) / std::max(1.0F, snapRadius);
                        const float residualScore =
                            std::max({currentExcess, originalExcess * 0.40F, originalPointEvidence * 1.05F,
                                      originalColorEvidence * 1.75F}) -
                            lateralPenalty * 0.0016F;
                        if (residualScore <= bestScore || residualScore <= 0.0032F) {
                            continue;
                        }
                        bestScore = residualScore;
                        bestX = px;
                        bestY = py;
                    }

                    if (bestScore <= 0.0032F) {
                        continue;
                    }

                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& samples : targetSamples) {
                        samples.reserve(30);
                    }
                    targetLumaSamples.reserve(30);
                    for (const float sideOffset : {20.0F, 30.0F, 44.0F, 60.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            for (const float shift : {-8.0F, 0.0F, 8.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(bestX + normalX * sideOffset * side + unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(bestY + normalY * sideOffset * side + unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= snapRadius + 2.0F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                const float red = output.pixels[sampleBase];
                                const float green = output.pixels[sampleBase + 1];
                                const float blue = output.pixels[sampleBase + 2];
                                targetSamples[0].push_back(red);
                                targetSamples[1].push_back(green);
                                targetSamples[2].push_back(blue);
                                targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (targetLumaSamples.size() < 8) {
                        continue;
                    }
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        bestTarget[channel] = percentileValue(targetSamples[channel], 0.38F);
                    }
                    bestTargetLuma = percentileValue(targetLumaSamples, 0.38F);

                    const auto centerPixelX = static_cast<std::int32_t>(std::lround(bestX));
                    const auto centerPixelY = static_cast<std::int32_t>(std::lround(bestY));
                    const float patchRadius = bestScore > 0.012F ? 2.45F : 1.85F;
                    for (std::int32_t dy = -3; dy <= 3; ++dy) {
                        for (std::int32_t dx = -3; dx <= 3; ++dx) {
                            const float radialDistance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                            if (radialDistance > patchRadius) {
                                continue;
                            }
                            const auto x = centerPixelX + dx;
                            const auto y = centerPixelY + dy;
                            if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                                y >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                            if (distance.distance > snapRadius + 1.5F) {
                                continue;
                            }
                            const auto pixel = static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                               static_cast<std::uint32_t>(x);
                            const auto base = pixel * output.channels;
                            const float currentLuma = offAxisResiduals.pixels[base] * 0.2126F +
                                                      offAxisResiduals.pixels[base + 1] * 0.7152F +
                                                      offAxisResiduals.pixels[base + 2] * 0.0722F;
                            const float currentExcess = currentLuma - bestTargetLuma;
                            const float originalPointEvidence = localDottedPointScore(
                                trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                            const float originalColorEvidence =
                                navigationLightScoreAt(image, pixel) + warmExcessAt(image, pixel) * 1.25F +
                                std::sqrt(std::max(0.0F, colorVarianceAt(image, pixel))) * 0.25F;
                            const bool artificialBacked =
                                originalPointEvidence > 0.0020F || originalColorEvidence > 0.0014F ||
                                currentExcess > 0.0048F;
                            if (likelyNeutralBrightStarSample(image, pixel) && !artificialBacked) {
                                continue;
                            }
                            if (currentExcess <= 0.0005F && !artificialBacked) {
                                continue;
                            }

                            const float falloff =
                                1.0F - std::clamp(radialDistance / std::max(0.1F, patchRadius), 0.0F, 1.0F) * 0.62F;
                            const float evidence =
                                std::max({currentExcess, bestScore * 0.70F, originalPointEvidence, originalColorEvidence * 1.45F});
                            float strength = std::clamp((evidence - 0.0014F) / 0.015F, 0.0F, 0.82F) * falloff;
                            if (artificialBacked && currentExcess > 0.0028F) {
                                strength = std::max(strength, 0.46F * falloff);
                            }
                            if (currentExcess > 0.0080F) {
                                strength = std::max(strength, 0.66F * falloff);
                            }
                            strength = std::clamp(strength, 0.0F, 0.84F);
                            if (strength <= 0.0F) {
                                continue;
                            }
                            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                offAxisResiduals.pixels[base + channel] =
                                    offAxisResiduals.pixels[base + channel] * (1.0F - strength) +
                                    bestTarget[channel] * strength;
                            }
                        }
                    }
                }
            }
        }
        output = std::move(offAxisResiduals);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.97);

    if (output.channels >= 3 && !removedHorizonDottedDroneTrails.empty()) {
        ImageBuffer horizonPathScratchMatched = output;
        for (const auto& trail : removedHorizonDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || trail.path.size() < 2 || trail.length < 520.0F) {
                continue;
            }

            const float coreRadius = std::clamp(trail.width * 0.58F + 1.45F, 2.20F, 3.35F);
            for (std::size_t segment = 1; segment < trail.path.size(); ++segment) {
                const auto& start = trail.path[segment - 1];
                const auto& end = trail.path[segment];
                const float vx = end.x - start.x;
                const float vy = end.y - start.y;
                const float segmentLength = std::sqrt(std::max(1.0F, vx * vx + vy * vy));
                const float unitX = vx / segmentLength;
                const float unitY = vy / segmentLength;

                for (std::int32_t step = 0; step <= static_cast<std::int32_t>(segmentLength); step += 3) {
                    const float centerPx = start.x + unitX * static_cast<float>(step);
                    const float centerPy = start.y + unitY * static_cast<float>(step);
                    const auto centerPixelX = static_cast<std::int32_t>(std::lround(centerPx));
                    const auto centerPixelY = static_cast<std::int32_t>(std::lround(centerPy));

                    for (std::int32_t dy = -4; dy <= 4; ++dy) {
                        for (std::int32_t dx = -4; dx <= 4; ++dx) {
                            const auto x = centerPixelX + dx;
                            const auto y = centerPixelY + dy;
                            if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                                y >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }

                            const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                            if (distance.distance > coreRadius) {
                                continue;
                            }

                            const auto pixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                                static_cast<std::uint32_t>(x);
                            const float originalPointEvidence = localDottedPointScore(
                                trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                            if (likelyNeutralBrightStarSample(image, pixel) && originalPointEvidence < 0.0028F) {
                                continue;
                            }

                            std::vector<float> targetSamples[3];
                            std::vector<float> targetLumaSamples;
                            for (auto& samples : targetSamples) {
                                samples.reserve(36);
                            }
                            targetLumaSamples.reserve(36);
                            for (const float offset : {18.0F, 30.0F, 46.0F}) {
                                for (const float side : {-1.0F, 1.0F}) {
                                    for (const float shift : {-7.0F, 0.0F, 7.0F}) {
                                        const auto sx = static_cast<std::int32_t>(
                                            std::lround(static_cast<float>(x) + (-distance.unitY) * offset * side +
                                                        distance.unitX * shift));
                                        const auto sy = static_cast<std::int32_t>(
                                            std::lround(static_cast<float>(y) + distance.unitX * offset * side +
                                                        distance.unitY * shift));
                                        if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                            sy >= static_cast<std::int32_t>(image.height)) {
                                            continue;
                                        }
                                        const auto sampleDistance = distanceToPath(static_cast<float>(sx),
                                                                                   static_cast<float>(sy),
                                                                                   trail);
                                        if (sampleDistance.distance <= coreRadius + 1.35F) {
                                            continue;
                                        }
                                        const auto samplePixel =
                                            static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                            static_cast<std::uint32_t>(sx);
                                        if (likelyArtificialColorSample(image, samplePixel) ||
                                            likelyNeutralBrightStarSample(image, samplePixel)) {
                                            continue;
                                        }
                                        const auto sampleBase = samplePixel * output.channels;
                                        const float red = output.pixels[sampleBase];
                                        const float green = output.pixels[sampleBase + 1];
                                        const float blue = output.pixels[sampleBase + 2];
                                        targetSamples[0].push_back(red);
                                        targetSamples[1].push_back(green);
                                        targetSamples[2].push_back(blue);
                                        targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                                    }
                                }
                            }
                            if (targetLumaSamples.size() < 10) {
                                continue;
                            }

                            float target[3] = {0.0F, 0.0F, 0.0F};
                            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                target[channel] = percentileValue(targetSamples[channel], 0.52F);
                            }
                            const float targetLuma = percentileValue(targetLumaSamples, 0.52F);
                            const auto base = pixel * output.channels;
                            const float currentLuma =
                                horizonPathScratchMatched.pixels[base] * 0.2126F +
                                horizonPathScratchMatched.pixels[base + 1] * 0.7152F +
                                horizonPathScratchMatched.pixels[base + 2] * 0.0722F;
                            float colorResidual = 0.0F;
                            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                colorResidual =
                                    std::max(colorResidual, std::fabs(horizonPathScratchMatched.pixels[base + channel] - target[channel]));
                            }

                            const float lumaResidual = std::fabs(currentLuma - targetLuma);
                            const bool artificialBacked =
                                originalPointEvidence > 0.0024F || likelyArtificialColorSample(image, pixel);

                            const float distanceWeight =
                                std::pow(1.0F - std::clamp(distance.distance / std::max(0.1F, coreRadius), 0.0F, 1.0F), 0.55F);
                            const float evidence = std::max(lumaResidual, colorResidual * 0.72F);
                            float strength = std::clamp((evidence - 0.0010F) / 0.010F, 0.0F, 0.88F) * distanceWeight;
                            if (artificialBacked) {
                                strength = std::max(strength, 0.56F * distanceWeight);
                            }
                            strength = std::max(strength, 0.34F * distanceWeight);
                            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                horizonPathScratchMatched.pixels[base + channel] =
                                    horizonPathScratchMatched.pixels[base + channel] * (1.0F - strength) +
                                    target[channel] * strength;
                            }
                        }
                    }
                }
            }
        }
        output = std::move(horizonPathScratchMatched);
    }

    if (output.channels >= 3 &&
        (!removedMidLowArtificialTrails.empty() || !removedHorizonDottedDroneTrails.empty())) {
        ImageBuffer lowHorizonShoulderRestored = output;
        const auto restoreShoulder = [&](const ArtifactTrail& trail,
                                         float coreRadius,
                                         float shoulderRadius,
                                         float evidenceThreshold,
                                         float strengthCeiling) {
            const auto minX = static_cast<std::int32_t>(std::floor(std::min(trail.x1, trail.x2) - shoulderRadius - 2.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(std::max(trail.x1, trail.x2) + shoulderRadius + 2.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(std::min(trail.y1, trail.y2) - shoulderRadius - 2.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(std::max(trail.y1, trail.y2) + shoulderRadius + 2.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance <= coreRadius || distance.distance > shoulderRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float originalPointEvidence = localDottedPointScore(
                        trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                    if (likelyArtificialColorSample(image, pixel) || originalPointEvidence > evidenceThreshold) {
                        continue;
                    }

                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (auto& samples : targetSamples) {
                        samples.reserve(24);
                    }
                    targetLumaSamples.reserve(24);
                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    const float sampleInner = std::max(shoulderRadius + 8.0F, distance.distance + 12.0F);
                    const float preferredSide = distance.signedDistance >= 0.0F ? 1.0F : -1.0F;
                    for (const float offset : {sampleInner, sampleInner + 18.0F, sampleInner + 42.0F}) {
                        for (const float side : {-1.0F, 1.0F}) {
                            if (side != preferredSide) {
                                continue;
                            }
                            for (const float shift : {-9.0F, 0.0F, 9.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(x) + normalX * offset * side +
                                                distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(static_cast<float>(y) + normalY * offset * side +
                                                distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance =
                                    distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= shoulderRadius + 4.0F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                const float red = output.pixels[sampleBase];
                                const float green = output.pixels[sampleBase + 1];
                                const float blue = output.pixels[sampleBase + 2];
                                targetSamples[0].push_back(red);
                                targetSamples[1].push_back(green);
                                targetSamples[2].push_back(blue);
                                targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (targetSamples[0].size() < 8 || targetSamples[1].size() < 8 || targetSamples[2].size() < 8 ||
                        targetLumaSamples.size() < 8) {
                        continue;
                    }

                    const float targetLuma = percentileValue(targetLumaSamples, 0.52F);
                    float channelStd[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float meanSample =
                            std::accumulate(targetSamples[channel].begin(), targetSamples[channel].end(), 0.0F) /
                            static_cast<float>(targetSamples[channel].size());
                        float variance = 0.0F;
                        for (const float sample : targetSamples[channel]) {
                            const float delta = sample - meanSample;
                            variance += delta * delta;
                        }
                        channelStd[channel] =
                            std::sqrt(std::max(0.0F, variance / static_cast<float>(targetSamples[channel].size())));
                    }

                    const float shoulderPosition = std::clamp(
                        (shoulderRadius - distance.distance) / std::max(0.1F, shoulderRadius - coreRadius),
                        0.0F,
                        1.0F);
                    const float strength = std::pow(shoulderPosition, 0.42F) * strengthCeiling;
                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                              output.pixels[base + 2] * 0.0722F;
                    const float lumaOffset = std::clamp(targetLuma - currentLuma, -0.040F, 0.040F) * strength;
                    std::vector<float> originalLumaSamples;
                    originalLumaSamples.reserve(24);
                    for (std::int32_t dy = -9; dy <= 9; dy += 3) {
                        for (std::int32_t dx = -9; dx <= 9; dx += 3) {
                            if (dx == 0 && dy == 0) {
                                continue;
                            }
                            const auto sx = x + dx;
                            const auto sy = y + dy;
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto sampleDistance = distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                            if (sampleDistance.distance <= coreRadius + 1.5F) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyArtificialColorSample(image, samplePixel) ||
                                likelyNeutralBrightStarSample(image, samplePixel)) {
                                continue;
                            }
                            originalLumaSamples.push_back(luminanceAt(image, samplePixel));
                        }
                    }
                    float originalTexture = 0.0F;
                    if (originalLumaSamples.size() >= 8) {
                        originalTexture = std::clamp(luminanceAt(image, pixel) - median(originalLumaSamples),
                                                     -0.020F,
                                                     0.020F) *
                                          1.10F * strength;
                    }
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float texture =
                            deterministicNoise(static_cast<std::uint32_t>(x),
                                               static_cast<std::uint32_t>(y),
                                               static_cast<std::uint16_t>(channel + 911U)) *
                            channelStd[channel] * 0.68F * strength;
                        lowHorizonShoulderRestored.pixels[base + channel] =
                            std::clamp(output.pixels[base + channel] + lumaOffset + texture + originalTexture, 0.0F, 1.0F);
                    }
                }
            }
        };

        for (const auto& trail : removedMidLowArtificialTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            const bool bottomEdgeHorizonAirplane = trail.kind == ArtifactTrailKind::Airplane && trail.path.size() < 2 &&
                                                   centerRatio >= 0.955F && centerRatio <= 0.995F &&
                                                   trail.length >= 120.0F && trail.length <= 4000.0F;
            if (!bottomEdgeHorizonAirplane) {
                continue;
            }
            const float coreRadius = std::clamp(trail.width * 0.68F + 1.35F, 2.50F, 4.20F);
            const float shoulderRadius = std::clamp(trail.width * 10.0F + 60.0F, 72.0F, 96.0F);
            restoreShoulder(trail, coreRadius, shoulderRadius, 0.0020F, 0.62F);
        }

        // Dotted drone paths are now fitted to the actual light centers and use
        // a narrow cleanup mask. A broad shoulder reconstruction here would
        // replace untouched sky up to 72 pixels away and leave a visible band.

        output = std::move(lowHorizonShoulderRestored);
    }

    if (output.channels >= 3 &&
        (!removedMidLowArtificialTrails.empty() || !removedHorizonDottedDroneTrails.empty())) {
        ImageBuffer lowHorizonCrossSectionMatched = output;
        const auto repairCrossSection = [&](const ArtifactTrail& trail,
                                            float repairRadius,
                                            float sampleRadius,
                                            float strengthCeiling,
                                            float evidenceThreshold) {
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - sampleRadius - 3.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + sampleRadius + 3.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - sampleRadius - 3.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + sampleRadius + 3.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > repairRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float originalPointEvidence = localDottedPointScore(
                        trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                    const bool artificialBacked =
                        likelyArtificialColorSample(image, pixel) || originalPointEvidence > evidenceThreshold;
                    if (likelyNeutralBrightStarSample(image, pixel) && !artificialBacked) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    const float anchorX = static_cast<float>(x) - normalX * distance.signedDistance;
                    const float anchorY = static_cast<float>(y) - normalY * distance.signedDistance;
                    std::vector<float> negativeSamples[3];
                    std::vector<float> positiveSamples[3];
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        negativeSamples[channel].reserve(12);
                        positiveSamples[channel].reserve(12);
                    }

                    for (const float sampleSide : {-1.0F, 1.0F}) {
                        for (const float offset : {sampleRadius, sampleRadius + 16.0F, sampleRadius + 34.0F}) {
                            for (const float shift : {-8.0F, 0.0F, 8.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(anchorX + normalX * offset * sampleSide + distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(anchorY + normalY * offset * sampleSide + distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance =
                                    distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= repairRadius + 3.0F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                auto& samples = sampleSide < 0.0F ? negativeSamples : positiveSamples;
                                samples[0].push_back(output.pixels[sampleBase]);
                                samples[1].push_back(output.pixels[sampleBase + 1]);
                                samples[2].push_back(output.pixels[sampleBase + 2]);
                            }
                        }
                    }
                    if (negativeSamples[0].size() < 4 || positiveSamples[0].size() < 4) {
                        continue;
                    }

                    float negative[3] = {0.0F, 0.0F, 0.0F};
                    float positive[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        negative[channel] = percentileValue(negativeSamples[channel], 0.50F);
                        positive[channel] = percentileValue(positiveSamples[channel], 0.50F);
                    }
                    const float blend =
                        std::clamp((distance.signedDistance + repairRadius) / std::max(0.1F, repairRadius * 2.0F),
                                   0.0F,
                                   1.0F);
                    const float distanceWeight =
                        std::pow(1.0F - std::clamp(distance.distance / std::max(0.1F, repairRadius), 0.0F, 1.0F), 0.70F);
                    const float strength =
                        (artificialBacked ? std::max(0.72F, strengthCeiling) : strengthCeiling) * distanceWeight;
                    const auto base = pixel * output.channels;
                    const float targetRed = negative[0] * (1.0F - blend) + positive[0] * blend;
                    const float targetGreen = negative[1] * (1.0F - blend) + positive[1] * blend;
                    const float targetBlue = negative[2] * (1.0F - blend) + positive[2] * blend;
                    const float targetLuma = targetRed * 0.2126F + targetGreen * 0.7152F + targetBlue * 0.0722F;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                              output.pixels[base + 2] * 0.0722F;
                    const float lumaOffset = std::clamp(targetLuma - currentLuma, -0.024F, 0.024F) * strength;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        lowHorizonCrossSectionMatched.pixels[base + channel] =
                            std::clamp(output.pixels[base + channel] + lumaOffset, 0.0F, 1.0F);
                    }
                }
            }
        };

        for (const auto& trail : removedMidLowArtificialTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            const bool bottomEdgeHorizonAirplane = trail.kind == ArtifactTrailKind::Airplane && trail.path.size() < 2 &&
                                                   centerRatio >= 0.955F && centerRatio <= 0.995F &&
                                                   trail.length >= 120.0F && trail.length <= 4000.0F;
            if (!bottomEdgeHorizonAirplane) {
                continue;
            }
            repairCrossSection(trail, std::clamp(trail.width * 1.05F + 4.4F, 7.0F, 10.0F),
                               std::clamp(trail.width * 2.6F + 16.0F, 22.0F, 34.0F),
                               0.20F,
                               0.0022F);
            repairCrossSection(trail, std::clamp(trail.width * 9.0F + 54.0F, 64.0F, 88.0F),
                               std::clamp(trail.width * 10.0F + 66.0F, 78.0F, 104.0F),
                               0.16F,
                               0.0022F);
        }

        for (const auto& trail : removedHorizonDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || trail.path.size() < 2 || trail.length < 520.0F) {
                continue;
            }
            repairCrossSection(trail, std::clamp(trail.width * 1.00F + 4.0F, 6.0F, 8.2F),
                               std::clamp(trail.width * 3.0F + 16.0F, 22.0F, 34.0F),
                               0.18F,
                               0.0020F);
            repairCrossSection(trail, std::clamp(trail.width * 7.5F + 38.0F, 46.0F, 64.0F),
                               std::clamp(trail.width * 9.0F + 46.0F, 58.0F, 78.0F),
                               0.14F,
                               0.0020F);
        }

        output = std::move(lowHorizonCrossSectionMatched);
    }
    progress.report(ArtifactTrailProgressStage::Cleaning, 0.985);

    if (output.channels >= 3 &&
        (!removedMidLowArtificialTrails.empty() || !removedHorizonDottedDroneTrails.empty())) {
        ImageBuffer lowHorizonCoreDiffused = output;
        const auto diffuseLowHorizonCore = [&](const ArtifactTrail& trail,
                                               float diffuseRadius,
                                               float evidenceThreshold,
                                               float blendStrength) {
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = std::max<std::int32_t>(
                0, static_cast<std::int32_t>(std::floor(minTrailX - diffuseRadius - 3.0F)));
            const auto maxX = std::min<std::int32_t>(
                static_cast<std::int32_t>(image.width) - 1,
                static_cast<std::int32_t>(std::ceil(maxTrailX + diffuseRadius + 3.0F)));
            const auto minY = std::max<std::int32_t>(
                0, static_cast<std::int32_t>(std::floor(minTrailY - diffuseRadius - 3.0F)));
            const auto maxY = std::min<std::int32_t>(
                static_cast<std::int32_t>(image.height) - 1,
                static_cast<std::int32_t>(std::ceil(maxTrailY + diffuseRadius + 3.0F)));
            if (maxX < minX || maxY < minY) {
                return;
            }

            const auto patchWidth = static_cast<std::uint32_t>(maxX - minX + 1);
            const auto patchHeight = static_cast<std::uint32_t>(maxY - minY + 1);
            std::vector<std::uint8_t> patchMask(static_cast<std::size_t>(patchWidth) * patchHeight, 0);
            std::vector<float> patch(static_cast<std::size_t>(patchWidth) * patchHeight * output.channels, 0.0F);
            for (std::uint32_t py = 0; py < patchHeight; ++py) {
                for (std::uint32_t px = 0; px < patchWidth; ++px) {
                    const auto x = minX + static_cast<std::int32_t>(px);
                    const auto y = minY + static_cast<std::int32_t>(py);
                    const auto imagePixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                        static_cast<std::uint32_t>(x);
                    const auto patchPixel = static_cast<std::size_t>(py) * patchWidth + px;
                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance <= diffuseRadius) {
                        const float originalPointEvidence = localDottedPointScore(
                            trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                        const bool artificialBacked =
                            likelyArtificialColorSample(image, imagePixel) || originalPointEvidence > evidenceThreshold;
                        if (!likelyNeutralBrightStarSample(image, imagePixel) || artificialBacked) {
                            patchMask[patchPixel] = 1;
                        }
                    }
                    const auto imageBase = imagePixel * output.channels;
                    const auto patchBase = patchPixel * output.channels;
                    for (std::uint16_t channel = 0; channel < output.channels; ++channel) {
                        patch[patchBase + channel] = lowHorizonCoreDiffused.pixels[imageBase + channel];
                    }
                }
            }

            std::vector<float> nextPatch = patch;
            for (std::uint8_t iteration = 0; iteration < 7; ++iteration) {
                nextPatch = patch;
                for (std::uint32_t py = 0; py < patchHeight; ++py) {
                    for (std::uint32_t px = 0; px < patchWidth; ++px) {
                        const auto patchPixel = static_cast<std::size_t>(py) * patchWidth + px;
                        if (!patchMask[patchPixel]) {
                            continue;
                        }
                        float sum[3] = {0.0F, 0.0F, 0.0F};
                        float weightSum = 0.0F;
                        for (std::int32_t dy = -1; dy <= 1; ++dy) {
                            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                                if (dx == 0 && dy == 0) {
                                    continue;
                                }
                                const auto nx = static_cast<std::int32_t>(px) + dx;
                                const auto ny = static_cast<std::int32_t>(py) + dy;
                                if (nx < 0 || ny < 0 || nx >= static_cast<std::int32_t>(patchWidth) ||
                                    ny >= static_cast<std::int32_t>(patchHeight)) {
                                    continue;
                                }
                                const auto neighborPixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(ny)) * patchWidth +
                                    static_cast<std::uint32_t>(nx);
                                const float weight = (dx == 0 || dy == 0) ? 1.0F : 0.70F;
                                const auto neighborBase = neighborPixel * output.channels;
                                for (std::uint16_t channel = 0; channel < 3; ++channel) {
                                    sum[channel] += patch[neighborBase + channel] * weight;
                                }
                                weightSum += weight;
                            }
                        }
                        if (weightSum <= 0.0F) {
                            continue;
                        }
                        const auto base = patchPixel * output.channels;
                        const float iterationBlend = 0.58F;
                        for (std::uint16_t channel = 0; channel < 3; ++channel) {
                            const float neighborAverage = sum[channel] / weightSum;
                            nextPatch[base + channel] =
                                patch[base + channel] * (1.0F - iterationBlend) + neighborAverage * iterationBlend;
                        }
                    }
                }
                patch.swap(nextPatch);
            }

            for (std::uint32_t py = 0; py < patchHeight; ++py) {
                for (std::uint32_t px = 0; px < patchWidth; ++px) {
                    const auto patchPixel = static_cast<std::size_t>(py) * patchWidth + px;
                    if (!patchMask[patchPixel]) {
                        continue;
                    }
                    const auto x = minX + static_cast<std::int32_t>(px);
                    const auto y = minY + static_cast<std::int32_t>(py);
                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    const float distanceWeight =
                        std::pow(1.0F - std::clamp(distance.distance / std::max(0.1F, diffuseRadius), 0.0F, 1.0F), 0.62F);
                    const float strength = blendStrength * distanceWeight;
                    const auto imagePixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width +
                        static_cast<std::uint32_t>(x);
                    const auto imageBase = imagePixel * output.channels;
                    const auto patchBase = patchPixel * output.channels;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        lowHorizonCoreDiffused.pixels[imageBase + channel] =
                            lowHorizonCoreDiffused.pixels[imageBase + channel] * (1.0F - strength) +
                            patch[patchBase + channel] * strength;
                    }
                }
            }
        };

        for (const auto& trail : removedMidLowArtificialTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            const bool bottomEdgeHorizonAirplane = trail.kind == ArtifactTrailKind::Airplane && trail.path.size() < 2 &&
                                                   centerRatio >= 0.955F && centerRatio <= 0.995F &&
                                                   trail.length >= 120.0F && trail.length <= 4000.0F;
            const bool midLowArtificial = centerRatio >= 0.86F && centerRatio <= 0.955F &&
                                          trail.kind != ArtifactTrailKind::Meteor && trail.length >= 150.0F;
            if (!bottomEdgeHorizonAirplane && !midLowArtificial) {
                continue;
            }
            diffuseLowHorizonCore(trail,
                                  bottomEdgeHorizonAirplane ? std::clamp(trail.width * 0.90F + 3.8F, 5.8F, 8.2F)
                                                            : std::clamp(trail.width * 0.82F + 3.4F, 5.2F, 7.2F),
                                  bottomEdgeHorizonAirplane ? 0.0022F : 0.0025F,
                                  bottomEdgeHorizonAirplane ? 0.68F : 0.56F);
        }

        for (const auto& trail : removedHorizonDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || trail.path.size() < 2 || trail.length < 520.0F) {
                continue;
            }
            diffuseLowHorizonCore(trail,
                                  std::clamp(trail.width * 0.78F + 3.4F, 5.0F, 7.0F),
                                  0.0020F,
                                  0.60F);
        }

        output = std::move(lowHorizonCoreDiffused);
    }

    if (output.channels >= 3 &&
        (!removedMidLowArtificialTrails.empty() || !removedHorizonDottedDroneTrails.empty())) {
        ImageBuffer lowHorizonTextureRebalanced = output;
        const auto rebalanceLowHorizonTexture = [&](const ArtifactTrail& trail,
                                                    float repairRadius,
                                                    float sampleRadius,
                                                    float evidenceThreshold,
                                                    float lumaStrength,
                                                    float textureStrength) {
            float minTrailX = std::min(trail.x1, trail.x2);
            float maxTrailX = std::max(trail.x1, trail.x2);
            float minTrailY = std::min(trail.y1, trail.y2);
            float maxTrailY = std::max(trail.y1, trail.y2);
            for (const auto& point : trail.path) {
                minTrailX = std::min(minTrailX, point.x);
                maxTrailX = std::max(maxTrailX, point.x);
                minTrailY = std::min(minTrailY, point.y);
                maxTrailY = std::max(maxTrailY, point.y);
            }

            const auto minX = static_cast<std::int32_t>(std::floor(minTrailX - sampleRadius - 4.0F));
            const auto maxX = static_cast<std::int32_t>(std::ceil(maxTrailX + sampleRadius + 4.0F));
            const auto minY = static_cast<std::int32_t>(std::floor(minTrailY - sampleRadius - 4.0F));
            const auto maxY = static_cast<std::int32_t>(std::ceil(maxTrailY + sampleRadius + 4.0F));
            for (std::int32_t y = minY; y <= maxY; ++y) {
                for (std::int32_t x = minX; x <= maxX; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) ||
                        y >= static_cast<std::int32_t>(image.height)) {
                        continue;
                    }

                    const auto distance = distanceToPath(static_cast<float>(x), static_cast<float>(y), trail);
                    if (distance.distance > repairRadius) {
                        continue;
                    }

                    const auto pixel =
                        static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * image.width + static_cast<std::uint32_t>(x);
                    const float originalPointEvidence = localDottedPointScore(
                        trail, luminance, image, static_cast<float>(x), static_cast<float>(y));
                    const bool artificialBacked =
                        likelyArtificialColorSample(image, pixel) || originalPointEvidence > evidenceThreshold;
                    if (likelyNeutralBrightStarSample(image, pixel) && !artificialBacked) {
                        continue;
                    }

                    const float normalX = -distance.unitY;
                    const float normalY = distance.unitX;
                    const float anchorX = static_cast<float>(x) - normalX * distance.signedDistance;
                    const float anchorY = static_cast<float>(y) - normalY * distance.signedDistance;
                    std::vector<float> targetSamples[3];
                    std::vector<float> targetLumaSamples;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        targetSamples[channel].reserve(24);
                    }
                    targetLumaSamples.reserve(24);

                    for (const float sampleSide : {-1.0F, 1.0F}) {
                        for (const float offset : {sampleRadius, sampleRadius + 10.0F, sampleRadius + 24.0F}) {
                            for (const float shift : {-7.0F, 0.0F, 7.0F}) {
                                const auto sx = static_cast<std::int32_t>(
                                    std::lround(anchorX + normalX * offset * sampleSide + distance.unitX * shift));
                                const auto sy = static_cast<std::int32_t>(
                                    std::lround(anchorY + normalY * offset * sampleSide + distance.unitY * shift));
                                if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                    sy >= static_cast<std::int32_t>(image.height)) {
                                    continue;
                                }
                                const auto sampleDistance =
                                    distanceToPath(static_cast<float>(sx), static_cast<float>(sy), trail);
                                if (sampleDistance.distance <= repairRadius + 2.0F) {
                                    continue;
                                }
                                const auto samplePixel =
                                    static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                    static_cast<std::uint32_t>(sx);
                                if (likelyArtificialColorSample(image, samplePixel) ||
                                    likelyNeutralBrightStarSample(image, samplePixel)) {
                                    continue;
                                }
                                const auto sampleBase = samplePixel * output.channels;
                                const float red = output.pixels[sampleBase];
                                const float green = output.pixels[sampleBase + 1];
                                const float blue = output.pixels[sampleBase + 2];
                                targetSamples[0].push_back(red);
                                targetSamples[1].push_back(green);
                                targetSamples[2].push_back(blue);
                                targetLumaSamples.push_back(red * 0.2126F + green * 0.7152F + blue * 0.0722F);
                            }
                        }
                    }
                    if (targetLumaSamples.size() < 8) {
                        continue;
                    }

                    std::vector<float> localLumaSamples;
                    std::vector<float> originalLumaSamples;
                    localLumaSamples.reserve(24);
                    originalLumaSamples.reserve(24);
                    for (std::int32_t dy = -3; dy <= 3; ++dy) {
                        for (std::int32_t dx = -3; dx <= 3; ++dx) {
                            if (std::abs(dx) + std::abs(dy) > 4) {
                                continue;
                            }
                            const auto sx = x + dx;
                            const auto sy = y + dy;
                            if (sx < 0 || sy < 0 || sx >= static_cast<std::int32_t>(image.width) ||
                                sy >= static_cast<std::int32_t>(image.height)) {
                                continue;
                            }
                            const auto samplePixel =
                                static_cast<std::size_t>(static_cast<std::uint32_t>(sy)) * image.width +
                                static_cast<std::uint32_t>(sx);
                            if (likelyNeutralBrightStarSample(image, samplePixel)) {
                                continue;
                            }
                            const auto sampleBase = samplePixel * output.channels;
                            localLumaSamples.push_back(output.pixels[sampleBase] * 0.2126F +
                                                       output.pixels[sampleBase + 1] * 0.7152F +
                                                       output.pixels[sampleBase + 2] * 0.0722F);
                            if (!likelyArtificialColorSample(image, samplePixel)) {
                                originalLumaSamples.push_back(luminanceAt(image, samplePixel));
                            }
                        }
                    }
                    if (localLumaSamples.size() < 6) {
                        continue;
                    }

                    float targetLumaMean = 0.0F;
                    for (const float sample : targetLumaSamples) {
                        targetLumaMean += sample;
                    }
                    targetLumaMean /= static_cast<float>(targetLumaSamples.size());
                    float targetLumaVariance = 0.0F;
                    for (const float sample : targetLumaSamples) {
                        const float delta = sample - targetLumaMean;
                        targetLumaVariance += delta * delta;
                    }
                    const float targetLumaStd = std::sqrt(
                        std::max(0.0F, targetLumaVariance / static_cast<float>(targetLumaSamples.size())));

                    float localLumaMean = 0.0F;
                    for (const float sample : localLumaSamples) {
                        localLumaMean += sample;
                    }
                    localLumaMean /= static_cast<float>(localLumaSamples.size());
                    float localLumaVariance = 0.0F;
                    for (const float sample : localLumaSamples) {
                        const float delta = sample - localLumaMean;
                        localLumaVariance += delta * delta;
                    }
                    const float localLumaStd =
                        std::sqrt(std::max(0.0F, localLumaVariance / static_cast<float>(localLumaSamples.size())));

                    float channelStd[3] = {0.0F, 0.0F, 0.0F};
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        if (targetSamples[channel].empty()) {
                            continue;
                        }
                        const float meanSample =
                            std::accumulate(targetSamples[channel].begin(), targetSamples[channel].end(), 0.0F) /
                            static_cast<float>(targetSamples[channel].size());
                        float varianceSample = 0.0F;
                        for (const float sample : targetSamples[channel]) {
                            const float delta = sample - meanSample;
                            varianceSample += delta * delta;
                        }
                        channelStd[channel] =
                            std::sqrt(std::max(0.0F, varianceSample / static_cast<float>(targetSamples[channel].size())));
                    }

                    const float targetLuma = percentileValue(targetLumaSamples, 0.50F);
                    const auto base = pixel * output.channels;
                    const float currentLuma = output.pixels[base] * 0.2126F + output.pixels[base + 1] * 0.7152F +
                                              output.pixels[base + 2] * 0.0722F;
                    const float smoothnessGap = std::max(0.0F, targetLumaStd * 0.92F - localLumaStd);
                    const float smoothnessWeight =
                        std::clamp(smoothnessGap / std::max(0.0025F, targetLumaStd * 0.92F), 0.0F, 1.0F);
                    const float residualWeight =
                        std::clamp((std::fabs(targetLuma - currentLuma) - 0.0014F) / 0.012F, 0.0F, 1.0F);
                    if (!artificialBacked && smoothnessWeight < 0.16F && residualWeight < 0.12F) {
                        continue;
                    }

                    const float distanceWeight =
                        std::pow(1.0F - std::clamp(distance.distance / std::max(0.1F, repairRadius), 0.0F, 1.0F), 0.55F);
                    const float evidenceWeight = std::max(smoothnessWeight * 0.82F, residualWeight);
                    const float strength =
                        distanceWeight * std::clamp((artificialBacked ? 0.36F : 0.18F) + evidenceWeight * 0.82F,
                                                    0.0F,
                                                    1.0F);
                    const float lumaOffset = std::clamp(targetLuma - currentLuma, -0.018F, 0.018F) * lumaStrength * strength;

                    float originalTexture = 0.0F;
                    if (!artificialBacked && originalLumaSamples.size() >= 8) {
                        originalTexture = std::clamp(luminanceAt(image, pixel) - median(originalLumaSamples),
                                                     -0.012F,
                                                     0.012F) *
                                          0.58F * strength;
                    }
                    const float missingTexture = std::min(0.018F, std::max(0.0F, targetLumaStd - localLumaStd * 0.55F));
                    const float lumaGrain =
                        deterministicNoise(static_cast<std::uint32_t>(x),
                                           static_cast<std::uint32_t>(y),
                                           1319U) *
                        missingTexture * textureStrength * strength;
                    for (std::uint16_t channel = 0; channel < 3; ++channel) {
                        const float chromaGrain =
                            deterministicNoise(static_cast<std::uint32_t>(x),
                                               static_cast<std::uint32_t>(y),
                                               static_cast<std::uint16_t>(channel + 1471U)) *
                            channelStd[channel] * textureStrength * 0.18F * strength;
                        lowHorizonTextureRebalanced.pixels[base + channel] =
                            std::clamp(output.pixels[base + channel] + lumaOffset + lumaGrain + chromaGrain +
                                           originalTexture,
                                       0.0F,
                                       1.0F);
                    }
                }
            }
        };

        for (const auto& trail : removedMidLowArtificialTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            const bool bottomEdgeHorizonAirplane = trail.kind == ArtifactTrailKind::Airplane && trail.path.size() < 2 &&
                                                   centerRatio >= 0.955F && centerRatio <= 0.995F &&
                                                   trail.length >= 120.0F && trail.length <= 4000.0F;
            const bool midLowArtificial = centerRatio >= 0.86F && centerRatio <= 0.955F &&
                                          trail.kind != ArtifactTrailKind::Meteor && trail.length >= 150.0F;
            if (!bottomEdgeHorizonAirplane && !midLowArtificial) {
                continue;
            }
            const float repairRadius = bottomEdgeHorizonAirplane
                                           ? std::clamp(trail.width * 1.15F + 4.8F, 7.5F, 11.0F)
                                           : std::clamp(trail.width * 1.05F + 4.2F, 6.3F, 8.8F);
            const float sampleRadius = bottomEdgeHorizonAirplane
                                           ? std::clamp(trail.width * 1.5F + 8.0F, 12.0F, 18.0F)
                                           : std::clamp(trail.width * 1.4F + 8.0F, 11.0F, 17.0F);
            rebalanceLowHorizonTexture(trail,
                                       repairRadius,
                                       sampleRadius,
                                       bottomEdgeHorizonAirplane ? 0.0022F : 0.0024F,
                                       bottomEdgeHorizonAirplane ? 1.00F : 0.74F,
                                       bottomEdgeHorizonAirplane ? 1.24F : 0.86F);
        }

        for (const auto& trail : removedHorizonDottedDroneTrails) {
            const float centerRatio = centerY(trail) / std::max(1.0F, static_cast<float>(image.height));
            if (centerRatio < 0.90F || trail.path.size() < 2 || trail.length < 520.0F) {
                continue;
            }
            rebalanceLowHorizonTexture(trail,
                                       std::clamp(trail.width * 1.05F + 4.2F, 6.3F, 8.8F),
                                       std::clamp(trail.width * 1.6F + 8.0F, 12.0F, 18.0F),
                                       0.0020F,
                                       0.86F,
                                       1.04F);
        }

        output = std::move(lowHorizonTextureRebalanced);
    }

    progress.report(ArtifactTrailProgressStage::Cleaning, 1.0);
    result.image = std::move(output);
    return result;
}

} // namespace photonstack
