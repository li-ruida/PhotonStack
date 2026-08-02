#include "photonstack/MosaicBuilder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "photonstack/ImageCodec.hpp"
#include "photonstack/ImageResizer.hpp"
#include "photonstack/Registration.hpp"

#include "StraightAlphaSampling.hpp"

namespace photonstack {
namespace {

MosaicResult mosaicError(std::string code, std::string message) {
    MosaicResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool representableFloat(double value) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    return std::isfinite(value) && value >= -maximum && value <= maximum;
}

bool validMosaicOptions(const MosaicOptions& options) {
    const auto validProjection = [&] {
        switch (options.projection) {
        case MosaicProjection::Planar:
        case MosaicProjection::Cylindrical:
            return true;
        }
        return false;
    }();
    const auto validLayout = [&] {
        switch (options.layout) {
        case MosaicLayout::Horizontal:
        case MosaicLayout::Grid:
            return true;
        }
        return false;
    }();
    const auto validAlignment = [&] {
        switch (options.alignment) {
        case MosaicAlignment::Manual:
        case MosaicAlignment::Auto:
            return true;
        }
        return false;
    }();
    const auto validBlendMode = [&] {
        switch (options.blendMode) {
        case MosaicBlendMode::Average:
        case MosaicBlendMode::Feather:
        case MosaicBlendMode::Multiband:
            return true;
        }
        return false;
    }();
    const auto validFitsDecodeMode = options.fitsDecodeMode == FitsDecodeMode::DisplayNormalized ||
                                     options.fitsDecodeMode == FitsDecodeMode::Scientific;
    if (!validProjection || !validLayout || !validAlignment || !validBlendMode || !validFitsDecodeMode) {
        return false;
    }
    if (options.alignment != MosaicAlignment::Auto) {
        return true;
    }
    return std::isfinite(options.registration.matchTolerance) && options.registration.matchTolerance > 0.0F &&
           options.registration.minimumMatches > 0 &&
           std::isfinite(options.registration.starDetection.sigmaThreshold) &&
           options.registration.starDetection.sigmaThreshold > 0.0F &&
           std::isfinite(options.registration.starDetection.minPeak) &&
           options.registration.starDetection.maxStars > 0;
}

bool validImage(const ImageBuffer& image) {
    return !image.empty() && image.channels > 0 && image.pixels.size() == image.sampleCount() &&
           std::all_of(image.pixels.begin(), image.pixels.end(), [](float value) {
               return std::isfinite(value);
           });
}

class MosaicProgressReporter {
  public:
    MosaicProgressReporter(const MosaicOptions& options, std::size_t panelCount)
        : options_(options), panelCount_(panelCount) {}

    void report(MosaicProgressStage stage, double progress, std::size_t panel = 0,
                std::uint32_t row = 0, std::uint32_t rowCount = 0,
                std::size_t level = 0, std::size_t levelCount = 0) {
        if (!options_.progress) {
            return;
        }
        lastProgress_ = std::max(lastProgress_, std::clamp(progress, 0.0, 1.0));
        options_.progress({
            .stage = stage,
            .progress = lastProgress_,
            .panel = panel,
            .panelCount = panelCount_,
            .row = row,
            .rowCount = rowCount,
            .level = level,
            .levelCount = levelCount,
        });
    }

    void reportRow(MosaicProgressStage stage, std::uint32_t row, std::uint32_t rowCount,
                   double start, double end, std::size_t panel = 0,
                   std::size_t level = 0, std::size_t levelCount = 0) {
        if (rowCount == 0) {
            report(stage, end, panel, 0, 0, level, levelCount);
            return;
        }
        const auto interval = std::max<std::uint32_t>(1, rowCount / 100);
        if (row != rowCount && row % interval != 0) {
            return;
        }
        const double fraction = static_cast<double>(row) / rowCount;
        report(stage, start + (end - start) * fraction, panel, row, rowCount, level, levelCount);
    }

  private:
    const MosaicOptions& options_;
    std::size_t panelCount_ = 0;
    double lastProgress_ = 0.0;
};

float sampleBilinear(const ImageBuffer& image, float x, float y, std::uint16_t channel) {
    return detail::sampleBilinearStraightAlpha(image, x, y, channel, false);
}

ImageBuffer cylindricalWarp(const ImageBuffer& image, MosaicProgressReporter& progress,
                            std::size_t panel, double start, double end) {
    ImageBuffer output = image;
    output.pixels.assign(output.sampleCount(), 0.0F);

    const float focalLength = std::max(1.0F, static_cast<float>(image.width) * 0.65F);
    const float centerX = static_cast<float>(image.width - 1) * 0.5F;
    const float centerY = static_cast<float>(image.height - 1) * 0.5F;

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float theta = (static_cast<float>(x) - centerX) / focalLength;
            const float cosTheta = std::max(1.0e-6F, std::cos(theta));
            const float sourceX = focalLength * std::tan(theta) + centerX;
            const float sourceY = (static_cast<float>(y) - centerY) / cosTheta + centerY;
            const auto outputOffset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                output.pixels[outputOffset + channel] = sampleBilinear(image, sourceX, sourceY, channel);
            }
        }
        progress.reportRow(MosaicProgressStage::Projecting, y + 1, image.height, start, end, panel);
    }

    return output;
}

std::uint32_t clampedColumns(const MosaicOptions& options, std::size_t imageCount) {
    if (imageCount == 0) {
        return 1;
    }
    if (options.layout == MosaicLayout::Horizontal) {
        return static_cast<std::uint32_t>(imageCount);
    }
    return std::clamp<std::uint32_t>(options.columns == 0 ? 1 : options.columns, 1, static_cast<std::uint32_t>(imageCount));
}

std::uint32_t effectiveOverlap(const std::vector<ImageBuffer>& images, std::uint32_t requestedOverlap) {
    const auto minWidth = std::min_element(
        images.begin(),
        images.end(),
        [](const ImageBuffer& left, const ImageBuffer& right) { return left.width < right.width; }
    )->width;
    return minWidth > 1 ? std::min(requestedOverlap, minWidth - 1) : 0;
}

std::vector<MosaicPlacement> manualPlacements(const std::vector<ImageBuffer>& images, const MosaicOptions& options) {
    std::vector<MosaicPlacement> placements(images.size());
    const auto columns = clampedColumns(options, images.size());
    const auto overlap = effectiveOverlap(images, options.overlapPixels);
    float y = 0.0F;
    for (std::size_t rowStart = 0; rowStart < images.size(); rowStart += columns) {
        const auto rowEnd = std::min<std::size_t>(rowStart + columns, images.size());
        float x = 0.0F;
        std::uint32_t rowHeight = 0;
        for (std::size_t index = rowStart; index < rowEnd; ++index) {
            placements[index].x = x;
            placements[index].y = y;
            x += static_cast<float>(images[index].width) - static_cast<float>(overlap);
            rowHeight = std::max(rowHeight, images[index].height);
        }
        y += static_cast<float>(rowHeight) - static_cast<float>(overlap);
    }
    return placements;
}

struct MosaicPoint {
    float x = 0.0F;
    float y = 0.0F;
};

struct MosaicBounds {
    float minX = 0.0F;
    float minY = 0.0F;
    float maxX = 0.0F;
    float maxY = 0.0F;
};

MosaicPoint transformPoint(const MosaicPlacement& placement, float x, float y) {
    return {
        .x = placement.a * x + placement.b * y + placement.x,
        .y = placement.c * x + placement.d * y + placement.y,
    };
}

bool inverseTransformPoint(const MosaicPlacement& placement, float x, float y, MosaicPoint& point) {
    const float determinant = placement.a * placement.d - placement.b * placement.c;
    if (!std::isfinite(determinant) || std::fabs(determinant) <= 1.0e-8F) {
        return false;
    }
    const float translatedX = x - placement.x;
    const float translatedY = y - placement.y;
    point = {
        .x = (placement.d * translatedX - placement.b * translatedY) / determinant,
        .y = (-placement.c * translatedX + placement.a * translatedY) / determinant,
    };
    return std::isfinite(point.x) && std::isfinite(point.y);
}

MosaicBounds transformedBounds(const ImageBuffer& image, const MosaicPlacement& placement) {
    const float maxLocalX = static_cast<float>(image.width - 1);
    const float maxLocalY = static_cast<float>(image.height - 1);
    const std::array<MosaicPoint, 4> corners = {
        transformPoint(placement, 0.0F, 0.0F),
        transformPoint(placement, maxLocalX, 0.0F),
        transformPoint(placement, 0.0F, maxLocalY),
        transformPoint(placement, maxLocalX, maxLocalY),
    };
    MosaicBounds bounds{
        .minX = corners.front().x,
        .minY = corners.front().y,
        .maxX = corners.front().x,
        .maxY = corners.front().y,
    };
    for (const auto& corner : corners) {
        bounds.minX = std::min(bounds.minX, corner.x);
        bounds.minY = std::min(bounds.minY, corner.y);
        bounds.maxX = std::max(bounds.maxX, corner.x);
        bounds.maxY = std::max(bounds.maxY, corner.y);
    }
    return bounds;
}

MosaicPlacement composePlacement(const MosaicPlacement& reference, const AffineTransform& relative) {
    MosaicPlacement placement;
    placement.a = reference.a * relative.a + reference.b * relative.c;
    placement.b = reference.a * relative.b + reference.b * relative.d;
    placement.c = reference.c * relative.a + reference.d * relative.c;
    placement.d = reference.c * relative.b + reference.d * relative.d;
    placement.x = reference.a * relative.dx + reference.b * relative.dy + reference.x;
    placement.y = reference.c * relative.dx + reference.d * relative.dy + reference.y;
    placement.dx = relative.dx;
    placement.dy = relative.dy;
    return placement;
}

MosaicTransformModel registrationTransformModel(const RegistrationResult& result) {
    if (!result.usedFallback) {
        return MosaicTransformModel::Affine;
    }
    constexpr float tolerance = 1.0e-4F;
    const bool translationOnly =
        std::fabs(result.affine.a - 1.0F) <= tolerance && std::fabs(result.affine.b) <= tolerance &&
        std::fabs(result.affine.c) <= tolerance && std::fabs(result.affine.d - 1.0F) <= tolerance;
    return translationOnly ? MosaicTransformModel::Translation : MosaicTransformModel::Similarity;
}

bool saneLinearTransform(const AffineTransform& transform) {
    const float determinant = transform.a * transform.d - transform.b * transform.c;
    const float scaleX = std::hypot(transform.a, transform.c);
    const float scaleY = std::hypot(transform.b, transform.d);
    const float normalizedDot = (transform.a * transform.b + transform.c * transform.d) /
                                std::max(1.0e-6F, scaleX * scaleY);
    const float rotation = std::atan2(transform.c, transform.a);
    return std::isfinite(determinant) && std::isfinite(scaleX) && std::isfinite(scaleY) &&
           determinant > 0.0F && scaleX >= 0.60F && scaleX <= 1.60F &&
           scaleY >= 0.60F && scaleY <= 1.60F && std::fabs(normalizedDot) <= 0.50F &&
           std::fabs(rotation) <= 0.70F;
}

MosaicPoint transformedCenterOffset(
    const ImageBuffer& reference,
    const ImageBuffer& moving,
    const AffineTransform& transform
) {
    const float movingCenterX = static_cast<float>(moving.width - 1) * 0.5F;
    const float movingCenterY = static_cast<float>(moving.height - 1) * 0.5F;
    const float referenceCenterX = static_cast<float>(reference.width - 1) * 0.5F;
    const float referenceCenterY = static_cast<float>(reference.height - 1) * 0.5F;
    return {
        .x = transform.a * movingCenterX + transform.b * movingCenterY + transform.dx - referenceCenterX,
        .y = transform.c * movingCenterX + transform.d * movingCenterY + transform.dy - referenceCenterY,
    };
}

bool saneHorizontalOffset(const ImageBuffer& reference, const ImageBuffer& moving, const AffineTransform& transform) {
    if (!saneLinearTransform(transform)) {
        return false;
    }
    const auto offset = transformedCenterOffset(reference, moving, transform);
    const float expectedMinimum = static_cast<float>(std::min(reference.width, moving.width)) * 0.15F;
    const float maxVerticalDrift = static_cast<float>(std::max(reference.height, moving.height)) * 0.75F;
    return offset.x > expectedMinimum && std::fabs(offset.y) <= maxVerticalDrift;
}

bool saneVerticalOffset(const ImageBuffer& reference, const ImageBuffer& moving, const AffineTransform& transform) {
    if (!saneLinearTransform(transform)) {
        return false;
    }
    const auto offset = transformedCenterOffset(reference, moving, transform);
    const float expectedMinimum = static_cast<float>(std::min(reference.height, moving.height)) * 0.15F;
    const float maxHorizontalDrift = static_cast<float>(std::max(reference.width, moving.width)) * 0.75F;
    return offset.y > expectedMinimum && std::fabs(offset.x) <= maxHorizontalDrift;
}

struct MosaicAlignmentImage {
    ImageBuffer image;
    float scaleX = 1.0F;
    float scaleY = 1.0F;
};

std::vector<MosaicAlignmentImage> alignmentImages(
    const std::vector<ImageBuffer>& images,
    std::uint32_t alignmentWidth,
    bool& usedCoarseAlignment
) {
    usedCoarseAlignment = false;
    std::vector<MosaicAlignmentImage> result;
    result.reserve(images.size());
    const auto widest = std::max_element(
        images.begin(), images.end(),
        [](const ImageBuffer& left, const ImageBuffer& right) { return left.width < right.width; }
    )->width;
    if (alignmentWidth == 0 || widest <= alignmentWidth) {
        return result;
    }

    usedCoarseAlignment = true;
    const double commonScale = static_cast<double>(alignmentWidth) / static_cast<double>(widest);
    const ImageResizer resizer;
    for (const auto& image : images) {
        const auto targetWidth = static_cast<std::uint32_t>(std::max(
            1.0, std::round(static_cast<double>(image.width) * commonScale)
        ));
        auto resized = resizer.resizeToWidth(image, targetWidth);
        if (!resized.ok) {
            result.clear();
            return result;
        }
        const float scaleX = static_cast<float>(resized.image.width) / static_cast<float>(image.width);
        const float scaleY = static_cast<float>(resized.image.height) / static_cast<float>(image.height);
        result.push_back({
            .image = std::move(resized.image),
            .scaleX = scaleX,
            .scaleY = scaleY,
        });
    }
    return result;
}

AffineTransform liftAlignmentTransform(
    const AffineTransform& transform,
    const MosaicAlignmentImage& reference,
    const MosaicAlignmentImage& moving
) {
    return {
        .a = transform.a * moving.scaleX / reference.scaleX,
        .b = transform.b * moving.scaleY / reference.scaleX,
        .c = transform.c * moving.scaleX / reference.scaleY,
        .d = transform.d * moving.scaleY / reference.scaleY,
        .dx = transform.dx / reference.scaleX,
        .dy = transform.dy / reference.scaleY,
    };
}

struct MosaicRegistrationStrip {
    ImageBuffer image;
    float originX = 0.0F;
    float originY = 0.0F;
};

MosaicRegistrationStrip registrationStrip(
    const ImageBuffer& image,
    const ImageBuffer& fullResolutionImage,
    std::uint32_t requestedOverlap,
    bool horizontal,
    bool reference
) {
    const auto axisSize = horizontal ? image.width : image.height;
    const auto fullAxisSize = horizontal ? fullResolutionImage.width : fullResolutionImage.height;
    const float scale = static_cast<float>(axisSize) / static_cast<float>(std::max<std::uint32_t>(1, fullAxisSize));
    const auto scaledOverlap = static_cast<std::uint32_t>(std::lround(
        static_cast<double>(requestedOverlap) * static_cast<double>(scale)));
    const auto minimumStrip = static_cast<std::uint32_t>(std::ceil(static_cast<double>(axisSize) * 0.40));
    const auto maximumStrip = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(
        std::ceil(static_cast<double>(axisSize) * 0.85)));
    const auto desiredStrip = scaledOverlap > 0
        ? static_cast<std::uint32_t>(std::ceil(static_cast<double>(scaledOverlap) * 1.5 + 8.0))
        : static_cast<std::uint32_t>(std::ceil(static_cast<double>(axisSize) * 0.60));
    const auto stripSize = std::clamp<std::uint32_t>(
        std::max(minimumStrip, desiredStrip), 1, std::min(axisSize, maximumStrip));

    const auto originX = horizontal && reference ? image.width - stripSize : 0U;
    const auto originY = !horizontal && reference ? image.height - stripSize : 0U;
    const auto width = horizontal ? stripSize : image.width;
    const auto height = horizontal ? image.height : stripSize;
    ImageBuffer strip;
    strip.width = width;
    strip.height = height;
    strip.channels = image.channels;
    strip.format = image.format;
    strip.colorEncoding = image.colorEncoding;
    strip.sourceBitsPerChannel = image.sourceBitsPerChannel;
    strip.pixels.assign(strip.sampleCount(), 0.0F);
    const auto rowSamples = static_cast<std::size_t>(width) * image.channels;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto sourceOffset =
            (static_cast<std::size_t>(originY + y) * image.width + originX) * image.channels;
        const auto destinationOffset = static_cast<std::size_t>(y) * rowSamples;
        std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
                    rowSamples, strip.pixels.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
    }
    return {
        .image = std::move(strip),
        .originX = static_cast<float>(originX),
        .originY = static_cast<float>(originY),
    };
}

AffineTransform restoreRegistrationStripTransform(
    const AffineTransform& transform,
    const MosaicRegistrationStrip& reference,
    const MosaicRegistrationStrip& moving
) {
    auto restored = transform;
    restored.dx += reference.originX - transform.a * moving.originX - transform.b * moving.originY;
    restored.dy += reference.originY - transform.c * moving.originX - transform.d * moving.originY;
    return restored;
}

struct MosaicAlignmentCandidate {
    MosaicPlacement placement;
    MosaicTransformModel transformModel = MosaicTransformModel::Manual;
    std::size_t matches = 0;
    float score = 0.0F;
    bool referenceTrusted = false;
    bool usedReducedModel = false;
};

float placementDisagreement(
    const MosaicPlacement& left,
    const MosaicPlacement& right,
    const ImageBuffer& image
) {
    const float maxX = static_cast<float>(image.width - 1);
    const float maxY = static_cast<float>(image.height - 1);
    const std::array<MosaicPoint, 5> samples = {{
        {0.0F, 0.0F}, {maxX, 0.0F}, {0.0F, maxY}, {maxX, maxY}, {maxX * 0.5F, maxY * 0.5F},
    }};
    float maximum = 0.0F;
    for (const auto& sample : samples) {
        const auto leftPoint = transformPoint(left, sample.x, sample.y);
        const auto rightPoint = transformPoint(right, sample.x, sample.y);
        maximum = std::max(maximum, std::hypot(leftPoint.x - rightPoint.x, leftPoint.y - rightPoint.y));
    }
    return maximum;
}

MosaicTransformModel moreExpressiveModel(MosaicTransformModel left, MosaicTransformModel right) {
    return static_cast<int>(left) >= static_cast<int>(right) ? left : right;
}

MosaicPlacement fuseAlignmentCandidates(
    const std::vector<MosaicAlignmentCandidate>& candidates,
    const ImageBuffer& image,
    float matchTolerance,
    std::size_t& acceptedMatches
) {
    const auto best = std::max_element(
        candidates.begin(), candidates.end(),
        [](const MosaicAlignmentCandidate& left, const MosaicAlignmentCandidate& right) {
            return left.score < right.score;
        });
    const float diagonal = std::hypot(static_cast<float>(image.width), static_cast<float>(image.height));
    const float consistencyTolerance = std::max(matchTolerance * 3.0F, diagonal * 0.015F);

    std::vector<const MosaicAlignmentCandidate*> consensus;
    consensus.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (candidate.referenceTrusted != best->referenceTrusted) {
            continue;
        }
        if (&candidate == &*best ||
            placementDisagreement(candidate.placement, best->placement, image) <= consistencyTolerance) {
            consensus.push_back(&candidate);
        }
    }

    MosaicPlacement placement = best->placement;
    float totalWeight = 0.0F;
    float a = 0.0F;
    float b = 0.0F;
    float c = 0.0F;
    float d = 0.0F;
    float x = 0.0F;
    float y = 0.0F;
    acceptedMatches = 0;
    placement.transformModel = MosaicTransformModel::Manual;
    placement.usedReducedModel = false;
    for (const auto* candidate : consensus) {
        const float weight = std::max(1.0F, candidate->score);
        totalWeight += weight;
        a += candidate->placement.a * weight;
        b += candidate->placement.b * weight;
        c += candidate->placement.c * weight;
        d += candidate->placement.d * weight;
        x += candidate->placement.x * weight;
        y += candidate->placement.y * weight;
        acceptedMatches += candidate->matches;
        placement.transformModel = moreExpressiveModel(placement.transformModel, candidate->transformModel);
        placement.usedReducedModel = placement.usedReducedModel || candidate->usedReducedModel;
    }
    if (totalWeight > 0.0F) {
        placement.a = a / totalWeight;
        placement.b = b / totalWeight;
        placement.c = c / totalWeight;
        placement.d = d / totalWeight;
        placement.x = x / totalWeight;
        placement.y = y / totalWeight;
    }
    placement.matches = acceptedMatches;
    placement.referenceCount = consensus.size();
    placement.autoAligned = true;
    placement.usedFallback = !best->referenceTrusted;
    return placement;
}

std::vector<MosaicPlacement> automaticPlacements(
    const std::vector<ImageBuffer>& images,
    const MosaicOptions& options,
    std::size_t& matchedPairs,
    MosaicProgressReporter& progress
) {
    auto placements = manualPlacements(images, options);
    for (std::size_t index = 1; index < placements.size(); ++index) {
        placements[index].usedFallback = true;
    }
    matchedPairs = 0;
    const auto columns = clampedColumns(options, images.size());
    const Registration registration;
    bool usedCoarseAlignment = false;
    const auto registrationImages = alignmentImages(images, options.alignmentWidth, usedCoarseAlignment);
    if (usedCoarseAlignment && registrationImages.size() != images.size()) {
        return placements;
    }

    progress.report(MosaicProgressStage::Aligning, 0.24, images.empty() ? 0 : 1);

    for (std::size_t index = 1; index < images.size(); ++index) {
        struct Neighbor {
            std::size_t index = 0;
            bool horizontal = false;
        };
        std::vector<Neighbor> neighbors;
        if (index % columns != 0) {
            neighbors.push_back({.index = index - 1, .horizontal = true});
        }
        if (index >= columns) {
            neighbors.push_back({.index = index - columns, .horizontal = false});
        }

        std::vector<MosaicAlignmentCandidate> candidates;
        candidates.reserve(neighbors.size());
        for (const auto& neighbor : neighbors) {
            const auto& registrationReferenceImage = usedCoarseAlignment
                ? registrationImages[neighbor.index].image
                : images[neighbor.index];
            const auto& registrationMovingImage = usedCoarseAlignment
                ? registrationImages[index].image
                : images[index];
            const auto registrationReference = registrationStrip(
                registrationReferenceImage,
                images[neighbor.index],
                options.overlapPixels,
                neighbor.horizontal,
                true);
            const auto registrationMoving = registrationStrip(
                registrationMovingImage,
                images[index],
                options.overlapPixels,
                neighbor.horizontal,
                false);
            const auto registrationResult = registration.estimateAffine(
                registrationReference.image, registrationMoving.image, options.registration);
            if (!registrationResult.ok) {
                continue;
            }
            const auto registrationTransform = restoreRegistrationStripTransform(
                registrationResult.affine, registrationReference, registrationMoving);
            const auto fullTransform = usedCoarseAlignment
                ? liftAlignmentTransform(
                      registrationTransform,
                      registrationImages[neighbor.index],
                      registrationImages[index])
                : registrationTransform;
            const bool sane = neighbor.horizontal
                ? saneHorizontalOffset(images[neighbor.index], images[index], fullTransform)
                : saneVerticalOffset(images[neighbor.index], images[index], fullTransform);
            if (!sane) {
                continue;
            }

            const bool referenceTrusted = !placements[neighbor.index].usedFallback;
            const float modelWeight = registrationResult.usedFallback ? 0.85F : 1.0F;
            const float score = static_cast<float>(registrationResult.matches) *
                                std::max(0.05F, registrationResult.inlierRatio) * modelWeight;
            candidates.push_back({
                .placement = composePlacement(placements[neighbor.index], fullTransform),
                .transformModel = registrationTransformModel(registrationResult),
                .matches = registrationResult.matches,
                .score = score,
                .referenceTrusted = referenceTrusted,
                .usedReducedModel = registrationResult.usedFallback,
            });
        }
        if (candidates.empty()) {
            const double fraction = images.size() <= 1
                ? 1.0
                : static_cast<double>(index) / static_cast<double>(images.size() - 1);
            progress.report(MosaicProgressStage::Aligning, 0.24 + 0.20 * fraction, index + 1);
            continue;
        }

        const bool hasTrustedCandidate = std::any_of(
            candidates.begin(), candidates.end(),
            [](const MosaicAlignmentCandidate& candidate) { return candidate.referenceTrusted; });
        if (hasTrustedCandidate) {
            candidates.erase(
                std::remove_if(
                    candidates.begin(), candidates.end(),
                    [](const MosaicAlignmentCandidate& candidate) { return !candidate.referenceTrusted; }),
                candidates.end());
        }

        std::size_t acceptedMatches = 0;
        auto placement = fuseAlignmentCandidates(
            candidates, images[index], options.registration.matchTolerance, acceptedMatches);
        placement.usedCoarseAlignment = usedCoarseAlignment;
        placements[index] = placement;
        matchedPairs += acceptedMatches;
        const double fraction = images.size() <= 1
            ? 1.0
            : static_cast<double>(index) / static_cast<double>(images.size() - 1);
        progress.report(MosaicProgressStage::Aligning, 0.24 + 0.20 * fraction, index + 1);
    }

    progress.report(MosaicProgressStage::Aligning, 0.44, images.size());

    return placements;
}

bool hasAlpha(const ImageBuffer& image) {
    return image.channels >= 4;
}

float sampleCoverage(const ImageBuffer& image, float x, float y) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fractionX = x - static_cast<float>(x0);
    const float fractionY = y - static_cast<float>(y0);
    float coverage = 0.0F;
    for (int yIndex = 0; yIndex < 2; ++yIndex) {
        const int sourceY = y0 + yIndex;
        if (sourceY < 0 || sourceY >= static_cast<int>(image.height)) {
            continue;
        }
        const float yWeight = yIndex == 0 ? 1.0F - fractionY : fractionY;
        for (int xIndex = 0; xIndex < 2; ++xIndex) {
            const int sourceX = x0 + xIndex;
            if (sourceX < 0 || sourceX >= static_cast<int>(image.width)) {
                continue;
            }
            const float xWeight = xIndex == 0 ? 1.0F - fractionX : fractionX;
            const float weight = xWeight * yWeight;
            if (hasAlpha(image)) {
                const auto offset =
                    (static_cast<std::size_t>(sourceY) * image.width + static_cast<std::uint32_t>(sourceX)) *
                    image.channels;
                const float alpha = image.pixels[offset + 3];
                if (std::isfinite(alpha)) {
                    coverage += std::clamp(alpha, 0.0F, 1.0F) * weight;
                }
            } else {
                coverage += weight;
            }
        }
    }
    return std::clamp(coverage, 0.0F, 1.0F);
}

float sampleStraightColor(const ImageBuffer& image, float x, float y, std::uint16_t channel) {
    if (channel >= image.channels || (hasAlpha(image) && channel == 3)) {
        return 0.0F;
    }
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fractionX = x - static_cast<float>(x0);
    const float fractionY = y - static_cast<float>(y0);
    double weightedColor = 0.0;
    double colorWeight = 0.0;
    for (int yIndex = 0; yIndex < 2; ++yIndex) {
        const int sourceY = y0 + yIndex;
        if (sourceY < 0 || sourceY >= static_cast<int>(image.height)) {
            continue;
        }
        const float yWeight = yIndex == 0 ? 1.0F - fractionY : fractionY;
        for (int xIndex = 0; xIndex < 2; ++xIndex) {
            const int sourceX = x0 + xIndex;
            if (sourceX < 0 || sourceX >= static_cast<int>(image.width)) {
                continue;
            }
            const float xWeight = xIndex == 0 ? 1.0F - fractionX : fractionX;
            const auto offset =
                (static_cast<std::size_t>(sourceY) * image.width + static_cast<std::uint32_t>(sourceX)) *
                image.channels;
            const float sourceAlpha = hasAlpha(image) ? image.pixels[offset + 3] : 1.0F;
            const float color = image.pixels[offset + channel];
            if (!std::isfinite(sourceAlpha) || !std::isfinite(color)) {
                continue;
            }
            const float alpha = std::clamp(sourceAlpha, 0.0F, 1.0F);
            const float weight = xWeight * yWeight * alpha;
            weightedColor += static_cast<double>(color) * weight;
            colorWeight += weight;
        }
    }
    return colorWeight > 1.0e-8 ? static_cast<float>(weightedColor / colorWeight) : 0.0F;
}

struct ExposureCorrection {
    std::array<float, 3> gains{1.0F, 1.0F, 1.0F};
    std::array<float, 3> offsets{};
};

struct ChannelSamples {
    std::array<std::vector<double>, 3> reference;
    std::array<std::vector<double>, 3> moving;
    std::size_t samples = 0;
};

double correctedSample(const ImageBuffer& image, float x, float y, std::uint16_t channel,
    const ExposureCorrection& correction, bool clampColor) {
    const double sample = sampleStraightColor(image, x, y, channel);
    if (channel >= correction.offsets.size()) {
        return sample;
    }
    const double corrected = sample * correction.gains[channel] + correction.offsets[channel];
    return clampColor ? std::clamp(corrected, 0.0, 1.0) : corrected;
}

ChannelSamples overlapSamples(
    const std::vector<ImageBuffer>& images,
    const std::vector<MosaicPlacement>& placements,
    const std::vector<ExposureCorrection>& corrections,
    std::size_t movingIndex,
    bool clampColor
) {
    ChannelSamples samples;
    const auto& moving = images[movingIndex];
    const auto movingBounds = transformedBounds(moving, placements[movingIndex]);

    for (std::size_t referenceIndex = 0; referenceIndex < movingIndex; ++referenceIndex) {
        const auto& reference = images[referenceIndex];
        const auto referenceBounds = transformedBounds(reference, placements[referenceIndex]);
        const int left = static_cast<int>(std::ceil(std::max(referenceBounds.minX, movingBounds.minX)));
        const int top = static_cast<int>(std::ceil(std::max(referenceBounds.minY, movingBounds.minY)));
        const int right = static_cast<int>(std::floor(std::min(referenceBounds.maxX, movingBounds.maxX)));
        const int bottom = static_cast<int>(std::floor(std::min(referenceBounds.maxY, movingBounds.maxY)));
        if (right < left || bottom < top) {
            continue;
        }

        const int stepX = std::max(1, (right - left + 1) / 48);
        const int stepY = std::max(1, (bottom - top + 1) / 48);
        for (int y = top; y <= bottom; y += stepY) {
            for (int x = left; x <= right; x += stepX) {
                MosaicPoint referenceLocal;
                MosaicPoint movingLocal;
                if (!inverseTransformPoint(
                        placements[referenceIndex], static_cast<float>(x), static_cast<float>(y), referenceLocal) ||
                    !inverseTransformPoint(
                        placements[movingIndex], static_cast<float>(x), static_cast<float>(y), movingLocal) ||
                    sampleCoverage(reference, referenceLocal.x, referenceLocal.y) <= 0.5F ||
                    sampleCoverage(moving, movingLocal.x, movingLocal.y) <= 0.5F) {
                    continue;
                }
                const auto channelCount = std::min<std::uint16_t>(moving.channels, 3);
                for (std::uint16_t channel = 0; channel < channelCount; ++channel) {
                    const double referenceValue = correctedSample(
                        reference, referenceLocal.x, referenceLocal.y, channel, corrections[referenceIndex],
                        clampColor);
                    const double movingValue = sampleStraightColor(
                        moving, movingLocal.x, movingLocal.y, channel);
                    samples.reference[channel].push_back(referenceValue);
                    samples.moving[channel].push_back(movingValue);
                }
                ++samples.samples;
            }
        }
    }

    return samples;
}

double median(std::vector<double>& values) {
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

std::vector<ExposureCorrection> exposureCorrections(
    const std::vector<ImageBuffer>& images,
    const std::vector<MosaicPlacement>& placements,
    bool& applied,
    MosaicProgressReporter& progress,
    bool clampColor
) {
    std::vector<ExposureCorrection> corrections(images.size());
    applied = false;
    progress.report(MosaicProgressStage::ExposureMatching, 0.44, images.empty() ? 0 : 1);
    for (std::size_t imageIndex = 1; imageIndex < images.size(); ++imageIndex) {
        auto samples = overlapSamples(images, placements, corrections, imageIndex, clampColor);
        if (samples.samples < 16) {
            const double fraction = images.size() <= 1
                ? 1.0
                : static_cast<double>(imageIndex) / static_cast<double>(images.size() - 1);
            progress.report(MosaicProgressStage::ExposureMatching, 0.44 + 0.06 * fraction, imageIndex + 1);
            continue;
        }

        const auto channelCount = std::min<std::uint16_t>(images[imageIndex].channels, 3);
        for (std::uint16_t channel = 0; channel < channelCount; ++channel) {
            if (samples.reference[channel].empty()) {
                continue;
            }
            double referenceMean = 0.0;
            double movingMean = 0.0;
            double minimumSample = std::numeric_limits<double>::max();
            double maximumSample = std::numeric_limits<double>::lowest();
            for (std::size_t sample = 0; sample < samples.reference[channel].size(); ++sample) {
                const double referenceValue = samples.reference[channel][sample];
                const double movingValue = samples.moving[channel][sample];
                referenceMean += referenceValue;
                movingMean += movingValue;
                minimumSample = std::min({minimumSample, referenceValue, movingValue});
                maximumSample = std::max({maximumSample, referenceValue, movingValue});
            }
            referenceMean /= static_cast<double>(samples.reference[channel].size());
            movingMean /= static_cast<double>(samples.moving[channel].size());

            double covariance = 0.0;
            double movingVariance = 0.0;
            double movingEnergy = 0.0;
            for (std::size_t sample = 0; sample < samples.reference[channel].size(); ++sample) {
                const double referenceDelta = samples.reference[channel][sample] - referenceMean;
                const double movingDelta = samples.moving[channel][sample] - movingMean;
                covariance += referenceDelta * movingDelta;
                movingVariance += movingDelta * movingDelta;
                movingEnergy += samples.moving[channel][sample] * samples.moving[channel][sample];
            }
            const double varianceFloor = std::numeric_limits<double>::epsilon() *
                std::max(movingEnergy, std::numeric_limits<double>::min());
            const double fittedGain = movingVariance > varianceFloor
                ? covariance / movingVariance
                : 1.0;
            const float gain = std::isfinite(fittedGain)
                ? static_cast<float>(std::clamp(fittedGain, 0.5, 2.0))
                : 1.0F;
            corrections[imageIndex].gains[channel] = gain;

            std::vector<double> residuals;
            residuals.reserve(samples.reference[channel].size());
            for (std::size_t sample = 0; sample < samples.reference[channel].size(); ++sample) {
                residuals.push_back(
                    samples.reference[channel][sample] - gain * samples.moving[channel][sample]);
            }
            if (!residuals.empty()) {
                const double sampleSpan = maximumSample - minimumSample;
                const double offsetLimit = clampColor ? 0.25 : 0.25 * std::max(1.0, sampleSpan);
                const double offset = std::clamp(median(residuals), -offsetLimit, offsetLimit);
                if (representableFloat(offset)) {
                    corrections[imageIndex].offsets[channel] = static_cast<float>(offset);
                }
            }
        }
        applied = true;
        const double fraction = images.size() <= 1
            ? 1.0
            : static_cast<double>(imageIndex) / static_cast<double>(images.size() - 1);
        progress.report(MosaicProgressStage::ExposureMatching, 0.44 + 0.06 * fraction, imageIndex + 1);
    }
    progress.report(MosaicProgressStage::ExposureMatching, 0.50, images.size());
    return corrections;
}

float featherWeight(const ImageBuffer& image, float x, float y) {
    const float edgeDistance = std::min({
        x + 1.0F,
        y + 1.0F,
        static_cast<float>(image.width) - x,
        static_cast<float>(image.height) - y,
    });
    const float featherRadius = std::max(1.0F, static_cast<float>(std::min(image.width, image.height)) * 0.08F);
    return std::clamp(edgeDistance / featherRadius, 0.08F, 1.0F);
}

float multibandWeight(const ImageBuffer& image, float x, float y) {
    const float edgeDistance = std::min({
        x + 1.0F,
        y + 1.0F,
        static_cast<float>(image.width) - x,
        static_cast<float>(image.height) - y,
    });
    const float normalized = std::clamp(edgeDistance / 4.0F, 0.02F, 1.0F);
    return normalized * normalized * (3.0F - 2.0F * normalized);
}

ImageBuffer emptyLike(const ImageBuffer& source, std::uint32_t width, std::uint32_t height) {
    ImageBuffer output;
    output.width = width;
    output.height = height;
    output.channels = source.channels;
    output.format = source.format;
    output.colorEncoding = source.colorEncoding;
    output.sourceBitsPerChannel = source.sourceBitsPerChannel;
    output.pixels.assign(output.sampleCount(), 0.0F);
    return output;
}

float sampleBilinearClamped(const ImageBuffer& image, float x, float y, std::uint16_t channel) {
    const float clampedX = std::clamp(x, 0.0F, static_cast<float>(image.width - 1));
    const float clampedY = std::clamp(y, 0.0F, static_cast<float>(image.height - 1));
    const auto x0 = static_cast<std::uint32_t>(std::floor(clampedX));
    const auto y0 = static_cast<std::uint32_t>(std::floor(clampedY));
    const auto x1 = std::min<std::uint32_t>(x0 + 1, image.width - 1);
    const auto y1 = std::min<std::uint32_t>(y0 + 1, image.height - 1);
    const float tx = clampedX - static_cast<float>(x0);
    const float ty = clampedY - static_cast<float>(y0);
    const auto offset = [&](std::uint32_t sampleX, std::uint32_t sampleY) {
        return (static_cast<std::size_t>(sampleY) * image.width + sampleX) * image.channels + channel;
    };
    const float top = image.pixels[offset(x0, y0)] * (1.0F - tx) + image.pixels[offset(x1, y0)] * tx;
    const float bottom = image.pixels[offset(x0, y1)] * (1.0F - tx) + image.pixels[offset(x1, y1)] * tx;
    return top * (1.0F - ty) + bottom * ty;
}

ImageBuffer gaussianDownsample(const ImageBuffer& image, const ExposureCorrection* correction,
                               bool clampColor) {
    const auto width = std::max<std::uint32_t>(1, (image.width + 1) / 2);
    const auto height = std::max<std::uint32_t>(1, (image.height + 1) / 2);
    auto output = emptyLike(image, width, height);
    constexpr std::array<float, 5> kernel = {1.0F / 16.0F, 4.0F / 16.0F, 6.0F / 16.0F,
                                              4.0F / 16.0F, 1.0F / 16.0F};
    std::array<std::vector<float>, 5> horizontalRows;
    std::array<int, 5> horizontalRowIDs;
    horizontalRowIDs.fill(-1);
    for (auto& row : horizontalRows) {
        row.assign(static_cast<std::size_t>(width) * image.channels, 0.0F);
    }

    const auto fillHorizontalRow = [&](std::vector<float>& row, int sourceY) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const int centerX = static_cast<int>(x * 2);
            const auto rowOffset = static_cast<std::size_t>(x) * image.channels;
            for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                double value = 0.0;
                for (int kernelX = -2; kernelX <= 2; ++kernelX) {
                    const auto sourceX = static_cast<std::uint32_t>(std::clamp(
                        centerX + kernelX, 0, static_cast<int>(image.width) - 1));
                    const auto sourceOffset =
                        (static_cast<std::size_t>(sourceY) * image.width + sourceX) * image.channels + channel;
                    double sample = image.pixels[sourceOffset];
                    if (correction != nullptr && channel < correction->offsets.size()) {
                        sample = sample * correction->gains[channel] + correction->offsets[channel];
                        if (clampColor) {
                            sample = std::clamp(sample, 0.0, 1.0);
                        }
                    }
                    value += sample * kernel[kernelX + 2];
                }
                row[rowOffset + channel] = static_cast<float>(value);
            }
        }
    };

    for (std::uint32_t y = 0; y < height; ++y) {
        const int centerY = static_cast<int>(y * 2);
        std::array<int, 5> neededRows;
        for (int kernelY = -2; kernelY <= 2; ++kernelY) {
            neededRows[kernelY + 2] = std::clamp(
                centerY + kernelY, 0, static_cast<int>(image.height) - 1);
        }
        for (const int neededRow : neededRows) {
            if (std::find(horizontalRowIDs.begin(), horizontalRowIDs.end(), neededRow) != horizontalRowIDs.end()) {
                continue;
            }
            const auto reusable = std::find_if(
                horizontalRowIDs.begin(), horizontalRowIDs.end(),
                [&](int existingRow) {
                    return std::find(neededRows.begin(), neededRows.end(), existingRow) == neededRows.end();
                }
            );
            const auto slot = static_cast<std::size_t>(std::distance(horizontalRowIDs.begin(), reusable));
            horizontalRowIDs[slot] = neededRow;
            fillHorizontalRow(horizontalRows[slot], neededRow);
        }
        std::array<std::size_t, 5> rowSlots;
        for (std::size_t rowIndex = 0; rowIndex < neededRows.size(); ++rowIndex) {
            rowSlots[rowIndex] = static_cast<std::size_t>(std::distance(
                horizontalRowIDs.begin(),
                std::find(horizontalRowIDs.begin(), horizontalRowIDs.end(), neededRows[rowIndex])
            ));
        }

        for (std::uint32_t x = 0; x < width; ++x) {
            const auto outputOffset = (static_cast<std::size_t>(y) * width + x) * output.channels;
            for (std::uint16_t channel = 0; channel < output.channels; ++channel) {
                double value = 0.0;
                for (int kernelY = -2; kernelY <= 2; ++kernelY) {
                    value += horizontalRows[rowSlots[kernelY + 2]]
                                           [static_cast<std::size_t>(x) * image.channels + channel] *
                             kernel[kernelY + 2];
                }
                output.pixels[outputOffset + channel] = static_cast<float>(value);
            }
        }
    }
    return output;
}

std::size_t multibandLevelCount(std::uint32_t width, std::uint32_t height) {
    std::size_t levels = 1;
    while (levels < 5 && width > 1 && height > 1) {
        width = (width + 1) / 2;
        height = (height + 1) / 2;
        ++levels;
    }
    return levels;
}

struct MultibandLevel {
    ImageBuffer image;
    std::vector<float> weights;
};

MosaicResult composeMultiband(
    const std::vector<ImageBuffer>& images,
    std::vector<MosaicPlacement> placements,
    const std::vector<ExposureCorrection>& corrections,
    std::uint32_t width,
    std::uint32_t height,
    bool exposureMatched,
    MosaicProgressReporter& progress,
    bool clampColor
) {
    try {
        const auto levelCount = multibandLevelCount(width, height);
        std::vector<MultibandLevel> levels;
        levels.reserve(levelCount);
        auto levelWidth = width;
        auto levelHeight = height;
        for (std::size_t level = 0; level < levelCount; ++level) {
            MultibandLevel accumulator;
            accumulator.image = emptyLike(images.front(), levelWidth, levelHeight);
            accumulator.weights.assign(accumulator.image.pixelCount(), 0.0F);
            levels.push_back(std::move(accumulator));
            levelWidth = std::max<std::uint32_t>(1, (levelWidth + 1) / 2);
            levelHeight = std::max<std::uint32_t>(1, (levelHeight + 1) / 2);
        }

        for (std::size_t imageIndex = 0; imageIndex < images.size(); ++imageIndex) {
            const auto originalBounds = transformedBounds(images[imageIndex], placements[imageIndex]);
            const ImageBuffer* source = &images[imageIndex];
            ImageBuffer ownedSource;
            for (std::size_t levelIndex = 0; levelIndex < levelCount; ++levelIndex) {
                ImageBuffer coarserStorage;
                const ImageBuffer* coarser = nullptr;
                if (levelIndex + 1 < levelCount) {
                    coarserStorage = gaussianDownsample(
                        *source, levelIndex == 0 ? &corrections[imageIndex] : nullptr, clampColor);
                    coarser = &coarserStorage;
                }
                auto& accumulator = levels[levelIndex];
                const float outputScaleX = static_cast<float>(accumulator.image.width) / static_cast<float>(width);
                const float outputScaleY = static_cast<float>(accumulator.image.height) / static_cast<float>(height);
                const float sourceScaleX = static_cast<float>(images[imageIndex].width) /
                                           static_cast<float>(source->width);
                const float sourceScaleY = static_cast<float>(images[imageIndex].height) /
                                           static_cast<float>(source->height);
                const float minimumTargetX = (originalBounds.minX + 0.5F) * outputScaleX - 0.5F;
                const float minimumTargetY = (originalBounds.minY + 0.5F) * outputScaleY - 0.5F;
                const float maximumTargetX = (originalBounds.maxX + 0.5F) * outputScaleX - 0.5F;
                const float maximumTargetY = (originalBounds.maxY + 0.5F) * outputScaleY - 0.5F;
                const int left = std::max(0, static_cast<int>(std::floor(minimumTargetX)) - 1);
                const int top = std::max(0, static_cast<int>(std::floor(minimumTargetY)) - 1);
                const int right = std::min(
                    static_cast<int>(accumulator.image.width) - 1,
                    static_cast<int>(std::ceil(maximumTargetX)) + 1);
                const int bottom = std::min(
                    static_cast<int>(accumulator.image.height) - 1,
                    static_cast<int>(std::ceil(maximumTargetY)) + 1);

                const auto workIndex = imageIndex * levelCount + levelIndex;
                const auto workCount = std::max<std::size_t>(1, images.size() * levelCount);
                const double workStart = 0.50 + 0.38 * static_cast<double>(workIndex) /
                    static_cast<double>(workCount);
                const double workEnd = 0.50 + 0.38 * static_cast<double>(workIndex + 1) /
                    static_cast<double>(workCount);
                const auto workRows = bottom >= top
                    ? static_cast<std::uint32_t>(bottom - top + 1)
                    : 0;
                progress.report(MosaicProgressStage::Blending, workStart, imageIndex + 1,
                                0, workRows, levelIndex + 1, levelCount);

                for (int outY = top; outY <= bottom; ++outY) {
                    const float globalY = (static_cast<float>(outY) + 0.5F) / outputScaleY - 0.5F;
                    for (int outX = left; outX <= right; ++outX) {
                        const float globalX = (static_cast<float>(outX) + 0.5F) / outputScaleX - 0.5F;
                        MosaicPoint originalLocal;
                        if (!inverseTransformPoint(
                                placements[imageIndex], globalX, globalY, originalLocal)) {
                            continue;
                        }
                        const float sourceX = (originalLocal.x + 0.5F) / sourceScaleX - 0.5F;
                        const float sourceY = (originalLocal.y + 0.5F) / sourceScaleY - 0.5F;
                        const float alpha = sampleCoverage(*source, sourceX, sourceY);
                        if (alpha <= 1.0e-6F) {
                            continue;
                        }
                        const float maskWeight = multibandWeight(*source, sourceX, sourceY);
                        const float baseWeight = maskWeight;
                        const float colorWeight = alpha * baseWeight;
                        const auto outputPixel = static_cast<std::size_t>(outY) * accumulator.image.width +
                                                 static_cast<std::uint32_t>(outX);
                        const auto outputOffset = outputPixel * accumulator.image.channels;
                        const double previousColorWeight = hasAlpha(accumulator.image)
                            ? accumulator.image.pixels[outputOffset + 3]
                            : accumulator.weights[outputPixel];
                        const double combinedColorWeight = previousColorWeight + colorWeight;
                        const double incomingFraction = colorWeight / combinedColorWeight;
                        for (std::uint16_t channel = 0; channel < accumulator.image.channels; ++channel) {
                            if (hasAlpha(accumulator.image) && channel == 3) {
                                continue;
                            }
                            double value = sampleStraightColor(*source, sourceX, sourceY, channel);
                            if (levelIndex == 0 && channel < corrections[imageIndex].offsets.size()) {
                                value = value * corrections[imageIndex].gains[channel] +
                                        corrections[imageIndex].offsets[channel];
                                if (clampColor) {
                                    value = std::clamp(value, 0.0, 1.0);
                                }
                            }
                            if (coarser != nullptr) {
                                const float coarseX = (sourceX + 0.5F) *
                                                      static_cast<float>(coarser->width) /
                                                      static_cast<float>(source->width) - 0.5F;
                                const float coarseY = (sourceY + 0.5F) *
                                                      static_cast<float>(coarser->height) /
                                                      static_cast<float>(source->height) - 0.5F;
                                value -= static_cast<double>(
                                    sampleStraightColor(*coarser, coarseX, coarseY, channel));
                            }
                            const double previous = accumulator.image.pixels[outputOffset + channel];
                            const double blended = previous + (value - previous) * incomingFraction;
                            if (!representableFloat(blended)) {
                                return mosaicError(
                                    "ImageValueInvalid", "Multiband mosaic values exceed the Float32 range");
                            }
                            accumulator.image.pixels[outputOffset + channel] = static_cast<float>(blended);
                        }
                        if (hasAlpha(accumulator.image)) {
                            accumulator.image.pixels[outputOffset + 3] =
                                static_cast<float>(combinedColorWeight);
                        }
                        accumulator.weights[outputPixel] +=
                            hasAlpha(accumulator.image) ? baseWeight : colorWeight;
                    }
                    progress.reportRow(
                        MosaicProgressStage::Blending,
                        static_cast<std::uint32_t>(outY - top + 1), workRows,
                        workStart, workEnd, imageIndex + 1, levelIndex + 1, levelCount);
                }
                if (coarser != nullptr) {
                    ownedSource = std::move(coarserStorage);
                    source = &ownedSource;
                }
            }
        }

        for (std::size_t levelIndex = 0; levelIndex < levels.size(); ++levelIndex) {
            auto& level = levels[levelIndex];
            const double levelStart = 0.88 + 0.04 * static_cast<double>(levelIndex) /
                static_cast<double>(levels.size());
            const double levelEnd = 0.88 + 0.04 * static_cast<double>(levelIndex + 1) /
                static_cast<double>(levels.size());
            for (std::uint32_t y = 0; y < level.image.height; ++y) {
                for (std::uint32_t x = 0; x < level.image.width; ++x) {
                    const auto pixel = static_cast<std::size_t>(y) * level.image.width + x;
                    const auto offset = pixel * level.image.channels;
                    const float colorWeight = hasAlpha(level.image)
                        ? level.image.pixels[offset + 3]
                        : level.weights[pixel];
                    if (colorWeight <= 0.0F) {
                        continue;
                    }
                    for (std::uint16_t channel = 0; channel < level.image.channels; ++channel) {
                        if (hasAlpha(level.image) && channel == 3) {
                            continue;
                        }
                        if (!std::isfinite(level.image.pixels[offset + channel])) {
                            return mosaicError("ImageValueInvalid", "Multiband mosaic contains non-finite values");
                        }
                    }
                    if (hasAlpha(level.image)) {
                        level.image.pixels[offset + 3] =
                            std::clamp(colorWeight / level.weights[pixel], 0.0F, 1.0F);
                    }
                }
                progress.reportRow(MosaicProgressStage::Normalizing, y + 1, level.image.height,
                                   levelStart, levelEnd, 0, levelIndex + 1, levels.size());
            }
            std::vector<float>().swap(level.weights);
        }

        ImageBuffer reconstructed = std::move(levels.back().image);
        for (std::size_t levelIndex = levels.size() - 1; levelIndex-- > 0;) {
            auto& detail = levels[levelIndex].image;
            const float scaleX = static_cast<float>(reconstructed.width) / static_cast<float>(detail.width);
            const float scaleY = static_cast<float>(reconstructed.height) / static_cast<float>(detail.height);
            for (std::uint32_t y = 0; y < detail.height; ++y) {
                const float sourceY = (static_cast<float>(y) + 0.5F) * scaleY - 0.5F;
                for (std::uint32_t x = 0; x < detail.width; ++x) {
                    const float sourceX = (static_cast<float>(x) + 0.5F) * scaleX - 0.5F;
                    const auto offset = (static_cast<std::size_t>(y) * detail.width + x) * detail.channels;
                    for (std::uint16_t channel = 0; channel < detail.channels; ++channel) {
                        if (!hasAlpha(detail) || channel != 3) {
                            const double reconstructedValue =
                                static_cast<double>(detail.pixels[offset + channel]) +
                                sampleBilinearClamped(reconstructed, sourceX, sourceY, channel);
                            if (!representableFloat(reconstructedValue)) {
                                return mosaicError(
                                    "ImageValueInvalid", "Multiband reconstruction exceeds the Float32 range");
                            }
                            detail.pixels[offset + channel] = static_cast<float>(reconstructedValue);
                        }
                    }
                }
            }
            reconstructed = std::move(detail);
            const auto completed = levels.size() - 1 - levelIndex;
            const auto total = std::max<std::size_t>(1, levels.size() - 1);
            progress.report(
                MosaicProgressStage::Normalizing,
                0.92 + 0.04 * static_cast<double>(completed) / static_cast<double>(total),
                0, 0, 0, completed, total);
        }

        for (std::uint32_t y = 0; y < reconstructed.height; ++y) {
            for (std::uint32_t x = 0; x < reconstructed.width; ++x) {
                const auto pixel = static_cast<std::size_t>(y) * reconstructed.width + x;
                const auto offset = pixel * reconstructed.channels;
                for (std::uint16_t channel = 0; channel < reconstructed.channels; ++channel) {
                    if (!std::isfinite(reconstructed.pixels[offset + channel])) {
                        return mosaicError("ImageValueInvalid", "Multiband mosaic contains non-finite values");
                    }
                    if ((hasAlpha(reconstructed) && channel == 3) || clampColor) {
                        reconstructed.pixels[offset + channel] =
                            std::clamp(reconstructed.pixels[offset + channel], 0.0F, 1.0F);
                    }
                }
            }
            progress.reportRow(MosaicProgressStage::Normalizing, y + 1, reconstructed.height,
                               0.96, 0.98);
        }

        MosaicResult result;
        result.ok = true;
        result.image = std::move(reconstructed);
        result.placements = std::move(placements);
        result.exposureMatched = exposureMatched;
        result.blendMode = MosaicBlendMode::Multiband;
        return result;
    } catch (const std::bad_alloc&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the multiband mosaic pyramid");
    } catch (const std::length_error&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the multiband mosaic pyramid");
    }
}

MosaicResult composeMosaic(
    const std::vector<ImageBuffer>& images,
    std::vector<MosaicPlacement> placements,
    const MosaicOptions& options,
    MosaicProgressReporter& progress
) {
    const bool clampColor = options.fitsDecodeMode == FitsDecodeMode::DisplayNormalized;
    bool exposureMatched = false;
    const auto corrections = options.exposureMatching
        ? exposureCorrections(images, placements, exposureMatched, progress, clampColor)
        : std::vector<ExposureCorrection>(images.size());
    if (!options.exposureMatching) {
        progress.report(MosaicProgressStage::ExposureMatching, 0.50, images.size());
    }
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    for (std::size_t index = 0; index < images.size(); ++index) {
        const auto bounds = transformedBounds(images[index], placements[index]);
        minX = std::min(minX, bounds.minX);
        minY = std::min(minY, bounds.minY);
        maxX = std::max(maxX, bounds.maxX);
        maxY = std::max(maxY, bounds.maxY);
    }

    const float shiftX = -std::floor(minX);
    const float shiftY = -std::floor(minY);
    const float outputWidth = std::max(1.0F, std::ceil(maxX + shiftX + 1.0F - 1.0e-4F));
    const float outputHeight = std::max(1.0F, std::ceil(maxY + shiftY + 1.0F - 1.0e-4F));
    if (!std::isfinite(outputWidth) || !std::isfinite(outputHeight) ||
        outputWidth >= static_cast<float>(std::numeric_limits<std::uint32_t>::max()) ||
        outputHeight >= static_cast<float>(std::numeric_limits<std::uint32_t>::max())) {
        return mosaicError("ImageDimensionsInvalid", "Mosaic output dimensions exceed the supported limit");
    }
    const auto width = static_cast<std::uint32_t>(outputWidth);
    const auto height = static_cast<std::uint32_t>(outputHeight);
    for (auto& placement : placements) {
        placement.x += shiftX;
        placement.y += shiftY;
    }

    if (options.blendMode == MosaicBlendMode::Multiband) {
        return composeMultiband(
            images, std::move(placements), corrections, width, height, exposureMatched, progress, clampColor);
    }

    ImageBuffer output;
    output.width = width;
    output.height = height;
    output.channels = images.front().channels;
    output.format = images.front().format;
    output.colorEncoding = images.front().colorEncoding;
    output.sourceBitsPerChannel = images.front().sourceBitsPerChannel;
    std::vector<float> weights;
    try {
        output.pixels.assign(output.sampleCount(), 0.0F);
        weights.assign(output.pixelCount(), 0.0F);
    } catch (const std::bad_alloc&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the mosaic accumulator");
    } catch (const std::length_error&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the mosaic accumulator");
    }

    for (std::size_t imageIndex = 0; imageIndex < images.size(); ++imageIndex) {
        const auto& image = images[imageIndex];
        const auto bounds = transformedBounds(image, placements[imageIndex]);
        const int left = std::max(0, static_cast<int>(std::floor(bounds.minX)) - 1);
        const int top = std::max(0, static_cast<int>(std::floor(bounds.minY)) - 1);
        const int right = std::min(
            static_cast<int>(output.width) - 1,
            static_cast<int>(std::ceil(bounds.maxX)) + 1);
        const int bottom = std::min(
            static_cast<int>(output.height) - 1,
            static_cast<int>(std::ceil(bounds.maxY)) + 1);
        const auto workRows = bottom >= top
            ? static_cast<std::uint32_t>(bottom - top + 1)
            : 0;
        const double workStart = 0.50 + 0.38 * static_cast<double>(imageIndex) /
            static_cast<double>(images.size());
        const double workEnd = 0.50 + 0.38 * static_cast<double>(imageIndex + 1) /
            static_cast<double>(images.size());
        progress.report(MosaicProgressStage::Blending, workStart, imageIndex + 1, 0, workRows);
        for (int outY = top; outY <= bottom; ++outY) {
            for (int outX = left; outX <= right; ++outX) {
                MosaicPoint source;
                if (!inverseTransformPoint(
                        placements[imageIndex], static_cast<float>(outX), static_cast<float>(outY), source)) {
                    continue;
                }
                const float alpha = sampleCoverage(image, source.x, source.y);
                if (alpha <= 1.0e-6F) {
                    continue;
                }
                float blendWeight = 1.0F;
                if (options.blendMode == MosaicBlendMode::Feather) {
                    blendWeight = featherWeight(image, source.x, source.y);
                }

                const float baseWeight = blendWeight;
                const float colorWeight = baseWeight * alpha;
                const auto outputPixel = static_cast<std::size_t>(outY) * output.width +
                                         static_cast<std::uint32_t>(outX);
                const auto outputOffset = outputPixel * output.channels;
                const double previousColorWeight = hasAlpha(output)
                    ? output.pixels[outputOffset + 3]
                    : weights[outputPixel];
                const double combinedColorWeight = previousColorWeight + colorWeight;
                const double incomingFraction = colorWeight / combinedColorWeight;
                for (std::uint16_t channel = 0; channel < output.channels; ++channel) {
                    if (hasAlpha(output) && channel == 3) {
                        continue;
                    }
                    double value = sampleStraightColor(image, source.x, source.y, channel);
                    if (channel < corrections[imageIndex].offsets.size()) {
                        value = value * corrections[imageIndex].gains[channel] +
                                corrections[imageIndex].offsets[channel];
                        if (clampColor) {
                            value = std::clamp(value, 0.0, 1.0);
                        }
                    }
                    const double previous = output.pixels[outputOffset + channel];
                    const double blended = previous + (value - previous) * incomingFraction;
                    if (!representableFloat(blended)) {
                        return mosaicError("ImageValueInvalid", "Mosaic values exceed the Float32 range");
                    }
                    output.pixels[outputOffset + channel] = static_cast<float>(blended);
                }
                if (hasAlpha(output)) {
                    output.pixels[outputOffset + 3] = static_cast<float>(combinedColorWeight);
                }
                weights[outputPixel] += hasAlpha(output) ? baseWeight : colorWeight;
            }
            progress.reportRow(
                MosaicProgressStage::Blending,
                static_cast<std::uint32_t>(outY - top + 1), workRows,
                workStart, workEnd, imageIndex + 1);
        }
    }

    for (std::uint32_t y = 0; y < output.height; ++y) {
        for (std::uint32_t x = 0; x < output.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * output.width + x;
            const auto offset = pixel * output.channels;
            const float normalizationWeight = hasAlpha(output) ? output.pixels[offset + 3] : weights[pixel];
            if (normalizationWeight <= 0.0F) {
                continue;
            }
            for (std::uint16_t channel = 0; channel < output.channels; ++channel) {
                if (hasAlpha(output) && channel == 3) {
                    continue;
                }
                if (!std::isfinite(output.pixels[offset + channel])) {
                    return mosaicError("ImageValueInvalid", "Mosaic contains non-finite values");
                }
                if (clampColor) {
                    output.pixels[offset + channel] =
                        std::clamp(output.pixels[offset + channel], 0.0F, 1.0F);
                }
            }
            if (hasAlpha(output)) {
                output.pixels[offset + 3] = std::clamp(normalizationWeight / weights[pixel], 0.0F, 1.0F);
            }
        }
        progress.reportRow(MosaicProgressStage::Normalizing, y + 1, output.height, 0.88, 0.98);
    }

    MosaicResult result;
    result.ok = true;
    result.image = std::move(output);
    result.placements = std::move(placements);
    result.exposureMatched = exposureMatched;
    result.blendMode = options.blendMode;
    return result;
}

MosaicResult loadInputs(const std::vector<std::filesystem::path>& inputs, const FitsDecodeMode fitsDecodeMode,
                        std::vector<ImageBuffer>& images, MosaicProgressReporter& progress) {
    if (inputs.empty()) {
        return mosaicError("InputMissing", "At least one input image is required");
    }

    const ImageCodec codec;
    ImageReadOptions readOptions;
    readOptions.fits.mode = fitsDecodeMode;
    readOptions.fits.maskNonFinitePixels = true;
    try {
        images.clear();
        images.reserve(inputs.size());
        progress.report(MosaicProgressStage::Reading, 0.0, 1);
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            const auto& input = inputs[index];
            const auto read = codec.read(input, readOptions);
            if (!read.ok) {
                return mosaicError(read.errorCode, "Failed to read " + input.string() + ": " + read.message);
            }
            images.push_back(std::move(read.image));
            progress.report(
                MosaicProgressStage::Reading,
                0.08 * static_cast<double>(index + 1) / static_cast<double>(inputs.size()),
                index + 1);
        }
    } catch (const std::bad_alloc&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the mosaic input sequence");
    } catch (const std::length_error&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the mosaic input sequence");
    }

    MosaicResult result;
    result.ok = true;
    return result;
}

} // namespace

MosaicResult MosaicBuilder::stitchHorizontal(const std::vector<std::filesystem::path>& inputs,
                                             const MosaicOptions& options) const {
    if (!validMosaicOptions(options)) {
        return mosaicError("ArgumentInvalid", "Mosaic options are invalid");
    }
    std::vector<ImageBuffer> images;
    MosaicProgressReporter progress(options, inputs.size());
    const auto loadResult = loadInputs(inputs, options.fitsDecodeMode, images, progress);
    if (!loadResult.ok) {
        return loadResult;
    }

    try {
        MosaicOptions effectiveOptions = options;
        std::uint32_t sourceWidest = 0;
        std::uint32_t workingWidest = 0;
        if (options.previewWidth > 0) {
            const ImageResizer resizer;
            sourceWidest = std::max_element(
                images.begin(), images.end(),
                [](const ImageBuffer& left, const ImageBuffer& right) { return left.width < right.width; }
            )->width;
            const auto registrationWidth = options.alignment == MosaicAlignment::Auto && options.alignmentWidth > 0
                ? options.alignmentWidth
                : options.previewWidth;
            workingWidest = std::min(sourceWidest, std::max(options.previewWidth, registrationWidth));
            if (sourceWidest > workingWidest) {
                const double scale = static_cast<double>(workingWidest) / static_cast<double>(sourceWidest);
                progress.report(MosaicProgressStage::Resizing, 0.08, 1);
                for (std::size_t index = 0; index < images.size(); ++index) {
                    auto& image = images[index];
                    const auto targetWidth = static_cast<std::uint32_t>(std::max(
                        1.0, std::round(static_cast<double>(image.width) * scale)
                    ));
                    auto scaled = resizer.resizeToWidth(image, targetWidth);
                    if (!scaled.ok) {
                        return mosaicError(scaled.errorCode, scaled.message);
                    }
                    image = std::move(scaled.image);
                    progress.report(
                        MosaicProgressStage::Resizing,
                        0.08 + 0.06 * static_cast<double>(index + 1) / static_cast<double>(images.size()),
                        index + 1);
                }
                effectiveOptions.overlapPixels = static_cast<std::uint32_t>(
                    std::max(0.0, std::round(static_cast<double>(options.overlapPixels) * scale))
                );
            }
        }

        progress.report(MosaicProgressStage::Resizing, 0.14, images.size());

        auto result = stitchHorizontal(images, effectiveOptions);
        if (!result.ok || options.previewWidth == 0 || workingWidest <= options.previewWidth) {
            return result;
        }

        const double previewScale = static_cast<double>(options.previewWidth) / static_cast<double>(workingWidest);
        const auto targetWidth = static_cast<std::uint32_t>(std::max(
            1.0, std::round(static_cast<double>(result.image.width) * previewScale)
        ));
        const auto sourceOutputWidth = result.image.width;
        const auto sourceOutputHeight = result.image.height;
        const ImageResizer resizer;
        auto preview = resizer.resizeToWidth(result.image, targetWidth);
        if (!preview.ok) {
            return mosaicError(preview.errorCode, preview.message);
        }
        const float outputScaleX = static_cast<float>(preview.image.width) / static_cast<float>(sourceOutputWidth);
        const float outputScaleY = static_cast<float>(preview.image.height) / static_cast<float>(sourceOutputHeight);
        result.image = std::move(preview.image);
        for (auto& placement : result.placements) {
            placement.x *= outputScaleX;
            placement.y *= outputScaleY;
            placement.dx *= outputScaleX;
            placement.dy *= outputScaleY;
            if (placement.autoAligned && sourceWidest > workingWidest) {
                placement.usedCoarseAlignment = true;
            }
        }
        progress.report(MosaicProgressStage::Resizing, 1.0, images.size());
        return result;
    } catch (const std::bad_alloc&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the mosaic preview");
    } catch (const std::length_error&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the mosaic preview");
    }
}

MosaicResult MosaicBuilder::stitchHorizontal(const std::vector<ImageBuffer>& images,
                                             const MosaicOptions& options) const {
    if (!validMosaicOptions(options)) {
        return mosaicError("ArgumentInvalid", "Mosaic options are invalid");
    }
    if (images.empty()) {
        return mosaicError("InputMissing", "At least one input image is required");
    }
    if (!validImage(images.front())) {
        return mosaicError("ImageBufferInvalid", "Input images must be non-empty finite float buffers");
    }
    for (std::size_t index = 1; index < images.size(); ++index) {
        if (!validImage(images[index])) {
            return mosaicError("ImageBufferInvalid", "Input images must be non-empty finite float buffers");
        }
        if (images.front().channels != images[index].channels) {
            return mosaicError("ImageDimensionsMismatch", "Mosaic inputs must share channel count");
        }
        if (images.front().colorEncoding != images[index].colorEncoding) {
            return mosaicError("ImageColorEncodingMismatch",
                               "Mosaic inputs must use the same color encoding");
        }
    }

    try {
        MosaicProgressReporter progress(options, images.size());
        progress.report(MosaicProgressStage::Projecting, 0.14, images.empty() ? 0 : 1);
        std::vector<ImageBuffer> projectedImages;
        const std::vector<ImageBuffer>* sourceImages = &images;
        if (options.projection == MosaicProjection::Cylindrical) {
            projectedImages.reserve(images.size());
            for (std::size_t index = 0; index < images.size(); ++index) {
                const double start = 0.14 + 0.10 * static_cast<double>(index) /
                    static_cast<double>(images.size());
                const double end = 0.14 + 0.10 * static_cast<double>(index + 1) /
                    static_cast<double>(images.size());
                projectedImages.push_back(cylindricalWarp(images[index], progress, index + 1, start, end));
            }
            sourceImages = &projectedImages;
        } else {
            progress.report(MosaicProgressStage::Projecting, 0.24, images.size());
        }

        const auto& workingImages = *sourceImages;
        std::size_t matchedPairs = 0;
        const auto placements = options.alignment == MosaicAlignment::Auto
            ? automaticPlacements(workingImages, options, matchedPairs, progress)
            : manualPlacements(workingImages, options);
        if (options.alignment != MosaicAlignment::Auto) {
            progress.report(MosaicProgressStage::Aligning, 0.44, workingImages.size());
        }
        auto result = composeMosaic(workingImages, placements, options, progress);
        result.usedAutoAlignment = options.alignment == MosaicAlignment::Auto && matchedPairs > 0;
        result.matchedPairs = matchedPairs;
        result.fallbackPanels = static_cast<std::size_t>(std::count_if(
            result.placements.begin(),
            result.placements.end(),
            [](const MosaicPlacement& placement) { return placement.usedFallback; }
        ));
        if (result.ok) {
            progress.report(MosaicProgressStage::Normalizing, 1.0);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the mosaic working set");
    } catch (const std::length_error&) {
        return mosaicError("MemoryAllocationFailed", "Unable to allocate the mosaic working set");
    }
}

} // namespace photonstack
