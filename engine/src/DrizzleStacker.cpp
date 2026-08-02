#include "photonstack/DrizzleStacker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#include "photonstack/ImageCodec.hpp"

namespace photonstack {
namespace {

DrizzleResult drizzleError(std::string code, std::string message) {
    DrizzleResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool validAlignment(DrizzleAlignment alignment) {
    switch (alignment) {
    case DrizzleAlignment::None:
    case DrizzleAlignment::Translation:
    case DrizzleAlignment::Similarity:
    case DrizzleAlignment::Affine:
    case DrizzleAlignment::Distortion:
        return true;
    }
    return false;
}

bool sameShape(const ImageBuffer& left, const ImageBuffer& right) {
    return left.width == right.width && left.height == right.height && left.channels == right.channels &&
           right.pixels.size() == right.sampleCount();
}

bool hasAlpha(const ImageBuffer& image) {
    return image.channels >= 4;
}

struct AxisFootprint {
    int first = 0;
    std::uint8_t count = 0;
    std::array<float, 5> overlaps{};
};

AxisFootprint footprint(float coordinate, std::uint32_t scale, float pixfrac, std::uint32_t outputLength) {
    const float scaled = static_cast<float>(scale);
    const float center = (coordinate + 0.5F) * scaled;
    const float halfDrop = pixfrac * scaled * 0.5F;
    const float dropStart = center - halfDrop;
    const float dropEnd = center + halfDrop;
    const int first = std::max(0, static_cast<int>(std::floor(dropStart)));
    const int last = std::min(static_cast<int>(outputLength) - 1,
                              static_cast<int>(std::ceil(dropEnd)) - 1);

    AxisFootprint result;
    result.first = first;
    for (int output = first; output <= last && result.count < result.overlaps.size(); ++output) {
        const float overlap = std::min(dropEnd, static_cast<float>(output + 1)) -
                              std::max(dropStart, static_cast<float>(output));
        if (overlap > 0.0F) {
            result.overlaps[result.count++] = overlap;
        }
    }
    return result;
}

} // namespace

DrizzleResult DrizzleStacker::drizzle(const std::vector<std::filesystem::path>& inputs,
                                      const DrizzleOptions& options) const {
    if (inputs.empty()) {
        return drizzleError("InputMissing", "At least one input image is required");
    }
    if (!validAlignment(options.alignment)) {
        return drizzleError("ArgumentInvalid", "Unsupported drizzle alignment mode");
    }
    if (options.fitsDecodeMode != FitsDecodeMode::DisplayNormalized &&
        options.fitsDecodeMode != FitsDecodeMode::Scientific) {
        return drizzleError("ArgumentInvalid", "Unsupported FITS decode mode");
    }
    if (options.scale < 1 || options.scale > 4) {
        return drizzleError("ArgumentInvalid", "Drizzle scale must be between 1 and 4");
    }
    if (!std::isfinite(options.pixfrac) || options.pixfrac <= 0.0F || options.pixfrac > 1.0F) {
        return drizzleError("ArgumentInvalid", "Drizzle pixfrac must be greater than 0 and no greater than 1");
    }

    double lastProgress = 0.0;
    const auto reportProgress = [&](DrizzleProgressStage stage, double progress, std::size_t frame,
                                    std::uint32_t row, std::uint32_t rowCount) {
        if (!options.progress) {
            return;
        }
        lastProgress = std::max(lastProgress, std::clamp(progress, 0.0, 1.0));
        options.progress({
            .stage = stage,
            .progress = lastProgress,
            .frame = frame,
            .frameCount = inputs.size(),
            .row = row,
            .rowCount = rowCount,
        });
    };
    const auto reportRowProgress = [&](DrizzleProgressStage stage, std::uint32_t row, std::uint32_t rowCount,
                                       double start, double end, std::size_t frame) {
        const auto completed = row + 1;
        const auto interval = std::max<std::uint32_t>(1, rowCount / 100);
        if (completed != rowCount && completed % interval != 0) {
            return;
        }
        const double fraction = rowCount == 0 ? 1.0 : static_cast<double>(completed) / rowCount;
        reportProgress(stage, start + (end - start) * fraction, frame, completed, rowCount);
    };

    const ImageCodec codec;
    ImageReadOptions readOptions;
    readOptions.fits.mode = options.fitsDecodeMode;
    readOptions.fits.maskNonFinitePixels = true;
    reportProgress(DrizzleProgressStage::Reading, 0.0, 1, 0, 0);
    auto referenceRead = codec.read(inputs.front(), readOptions);
    if (!referenceRead.ok) {
        return drizzleError(referenceRead.errorCode,
                            "Failed to read " + inputs.front().string() + ": " + referenceRead.message);
    }
    if (referenceRead.image.empty() || referenceRead.image.pixels.size() != referenceRead.image.sampleCount()) {
        return drizzleError("ImageBufferInvalid", "Decoded drizzle reference is empty or incomplete");
    }

    ImageBuffer reference = std::move(referenceRead.image);
    reportProgress(DrizzleProgressStage::Reading, 0.02, 1, 0, 0);
    if (reference.width > std::numeric_limits<std::uint32_t>::max() / options.scale ||
        reference.height > std::numeric_limits<std::uint32_t>::max() / options.scale) {
        return drizzleError("ImageDimensionsInvalid", "Scaled drizzle dimensions exceed the supported limit");
    }

    ImageBuffer output;
    output.width = reference.width * options.scale;
    output.height = reference.height * options.scale;
    output.channels = reference.channels;
    output.format = reference.format;
    output.colorEncoding = reference.colorEncoding;
    output.sourceBitsPerChannel = reference.sourceBitsPerChannel;
    std::vector<float> weights;
    try {
        output.pixels.assign(output.sampleCount(), 0.0F);
        if (!hasAlpha(output)) {
            weights.assign(output.pixelCount(), 0.0F);
        }
    } catch (const std::bad_alloc&) {
        return drizzleError("MemoryAllocationFailed", "Unable to allocate the scaled drizzle accumulator");
    } catch (const std::length_error&) {
        return drizzleError("MemoryAllocationFailed", "Unable to allocate the scaled drizzle accumulator");
    }

    const auto deposit = [&](const ImageBuffer& image, std::size_t inputOffset,
                             const AxisFootprint& xFootprint, const AxisFootprint& yFootprint) {
        const float sourceAlpha = hasAlpha(image) ? image.pixels[inputOffset + 3] : 1.0F;
        if (!std::isfinite(sourceAlpha)) {
            return;
        }
        const float alpha = std::clamp(sourceAlpha, 0.0F, 1.0F);
        if (alpha <= 1.0e-6F || xFootprint.count == 0 || yFootprint.count == 0) {
            return;
        }
        const auto colorChannels = std::min<std::uint16_t>(3, image.channels);
        for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
            if (!std::isfinite(image.pixels[inputOffset + channel])) {
                return;
            }
        }
        for (std::uint8_t yIndex = 0; yIndex < yFootprint.count; ++yIndex) {
            const auto outY = static_cast<std::uint32_t>(yFootprint.first + yIndex);
            for (std::uint8_t xIndex = 0; xIndex < xFootprint.count; ++xIndex) {
                const auto outX = static_cast<std::uint32_t>(xFootprint.first + xIndex);
                const float sampleWeight = yFootprint.overlaps[yIndex] * xFootprint.overlaps[xIndex] * alpha;
                const auto outputPixel = static_cast<std::size_t>(outY) * output.width + outX;
                const auto outputOffset = outputPixel * output.channels;
                const float previousWeight = hasAlpha(output) ? output.pixels[outputOffset + 3]
                                                              : weights[outputPixel];
                const float combinedWeight = previousWeight + sampleWeight;
                const double blend = static_cast<double>(sampleWeight) / combinedWeight;
                for (std::uint16_t channel = 0; channel < output.channels; ++channel) {
                    if (hasAlpha(output) && channel == 3) {
                        continue;
                    }
                    const double previous = output.pixels[outputOffset + channel];
                    const double source = image.pixels[inputOffset + channel];
                    output.pixels[outputOffset + channel] =
                        static_cast<float>(previous + (source - previous) * blend);
                }
                if (hasAlpha(output)) {
                    output.pixels[outputOffset + 3] = combinedWeight;
                } else {
                    weights[outputPixel] = combinedWeight;
                }
            }
        }
    };

    const auto accumulateSeparable = [&](const ImageBuffer& image, const Translation& translation,
                                         std::size_t frame, double start, double end) {
        reportProgress(DrizzleProgressStage::Accumulating, start, frame, 0, image.height);
        std::vector<AxisFootprint> xFootprints(image.width);
        std::vector<AxisFootprint> yFootprints(image.height);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            xFootprints[x] =
                footprint(static_cast<float>(x) + translation.dx, options.scale, options.pixfrac, output.width);
        }
        for (std::uint32_t y = 0; y < image.height; ++y) {
            yFootprints[y] =
                footprint(static_cast<float>(y) + translation.dy, options.scale, options.pixfrac, output.height);
        }

        for (std::uint32_t y = 0; y < image.height; ++y) {
            const auto& yFootprint = yFootprints[y];
            if (yFootprint.count != 0) {
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    const auto& xFootprint = xFootprints[x];
                    if (xFootprint.count == 0) {
                        continue;
                    }
                    const auto inputOffset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
                    deposit(image, inputOffset, xFootprint, yFootprint);
                }
            }
            reportRowProgress(DrizzleProgressStage::Accumulating, y, image.height, start, end, frame);
        }
    };

    const auto accumulateMapped = [&]<typename Mapper>(const ImageBuffer& image, Mapper mapper,
                                                       std::size_t frame, double start, double end) {
        reportProgress(DrizzleProgressStage::Accumulating, start, frame, 0, image.height);
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const auto target = mapper(static_cast<float>(x), static_cast<float>(y));
                if (!std::isfinite(target.first) || !std::isfinite(target.second)) {
                    continue;
                }
                const auto xFootprint = footprint(target.first, options.scale, options.pixfrac, output.width);
                const auto yFootprint = footprint(target.second, options.scale, options.pixfrac, output.height);
                const auto inputOffset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
                deposit(image, inputOffset, xFootprint, yFootprint);
            }
            reportRowProgress(DrizzleProgressStage::Accumulating, y, image.height, start, end, frame);
        }
    };

    constexpr double framePhaseStart = 0.02;
    constexpr double framePhaseEnd = 0.78;
    const double frameSpan = (framePhaseEnd - framePhaseStart) / static_cast<double>(inputs.size());
    try {
        accumulateSeparable(reference, {}, 1, framePhaseStart, framePhaseStart + frameSpan);
    } catch (const std::bad_alloc&) {
        return drizzleError("MemoryAllocationFailed", "Unable to allocate drizzle projection footprints");
    } catch (const std::length_error&) {
        return drizzleError("MemoryAllocationFailed", "Unable to allocate drizzle projection footprints");
    }
    if (options.alignment == DrizzleAlignment::None) {
        std::vector<float>().swap(reference.pixels);
    }

    const Registration registration;
    std::size_t alignedFrames = 0;
    std::size_t alignmentFallbacks = 0;
    std::size_t minimumAlignmentMatches = std::numeric_limits<std::size_t>::max();
    for (std::size_t frame = 1; frame < inputs.size(); ++frame) {
        const double frameStart = framePhaseStart + frameSpan * static_cast<double>(frame);
        const double frameEnd = frameStart + frameSpan;
        const double readEnd = frameStart + frameSpan * 0.08;
        const double alignmentEnd = frameStart + frameSpan * 0.45;
        reportProgress(DrizzleProgressStage::Reading, frameStart, frame + 1, 0, 0);
        auto read = codec.read(inputs[frame], readOptions);
        if (!read.ok) {
            return drizzleError(read.errorCode, "Failed to read " + inputs[frame].string() + ": " + read.message);
        }
        if (!sameShape(reference, read.image)) {
            return drizzleError("ImageDimensionsMismatch", "All drizzle inputs must share dimensions and channels");
        }
        if (reference.colorEncoding != read.image.colorEncoding) {
            return drizzleError("ImageColorEncodingMismatch",
                                "All drizzle inputs must use the same color encoding");
        }
        reportProgress(DrizzleProgressStage::Reading, readEnd, frame + 1, 0, 0);

        try {
            RegistrationResult aligned;
            const auto recordAlignment = [&](const RegistrationResult& value) {
                alignedFrames += 1;
                alignmentFallbacks += value.usedFallback ? 1 : 0;
                minimumAlignmentMatches = std::min(minimumAlignmentMatches, value.matches);
            };
            switch (options.alignment) {
            case DrizzleAlignment::None:
                accumulateSeparable(read.image, {}, frame + 1, readEnd, frameEnd);
                break;
            case DrizzleAlignment::Translation:
                reportProgress(DrizzleProgressStage::Aligning, readEnd, frame + 1, 0, 0);
                aligned = registration.estimateTranslation(reference, read.image, options.registration);
                reportProgress(DrizzleProgressStage::Aligning, alignmentEnd, frame + 1, 0, 0);
                if (aligned.ok) {
                    accumulateSeparable(read.image, aligned.translation, frame + 1, alignmentEnd, frameEnd);
                }
                break;
            case DrizzleAlignment::Similarity:
                reportProgress(DrizzleProgressStage::Aligning, readEnd, frame + 1, 0, 0);
                aligned = registration.estimateSimilarity(reference, read.image, options.registration);
                reportProgress(DrizzleProgressStage::Aligning, alignmentEnd, frame + 1, 0, 0);
                if (aligned.ok) {
                    const auto transform = aligned.transform;
                    const float cosTheta = std::cos(transform.rotationRadians);
                    const float sinTheta = std::sin(transform.rotationRadians);
                    accumulateMapped(read.image, [&](float x, float y) {
                        return std::pair<float, float>{
                            transform.scale * (cosTheta * x - sinTheta * y) + transform.dx,
                            transform.scale * (sinTheta * x + cosTheta * y) + transform.dy,
                        };
                    }, frame + 1, alignmentEnd, frameEnd);
                }
                break;
            case DrizzleAlignment::Affine:
                reportProgress(DrizzleProgressStage::Aligning, readEnd, frame + 1, 0, 0);
                aligned = registration.estimateAffine(reference, read.image, options.registration);
                reportProgress(DrizzleProgressStage::Aligning, alignmentEnd, frame + 1, 0, 0);
                if (aligned.ok) {
                    const auto transform = aligned.affine;
                    accumulateMapped(read.image, [&](float x, float y) {
                        return std::pair<float, float>{transform.a * x + transform.b * y + transform.dx,
                                                       transform.c * x + transform.d * y + transform.dy};
                    }, frame + 1, alignmentEnd, frameEnd);
                }
                break;
            case DrizzleAlignment::Distortion: {
                DistortionTransform transform;
                reportProgress(DrizzleProgressStage::Aligning, readEnd, frame + 1, 0, 0);
                aligned = registration.estimateDistortion(reference, read.image, transform, options.registration);
                reportProgress(DrizzleProgressStage::Aligning, alignmentEnd, frame + 1, 0, 0);
                if (aligned.ok) {
                    reportProgress(DrizzleProgressStage::Accumulating, alignmentEnd, frame + 1, 0,
                                   read.image.height);
                    const bool projected = registration.renderDistortionForwardRows(
                        read.image, transform,
                        [&](std::uint32_t row, const float* coordinates, std::size_t coordinateCount) {
                            if (coordinateCount != static_cast<std::size_t>(read.image.width) * 2) {
                                return false;
                            }
                            for (std::uint32_t x = 0; x < read.image.width; ++x) {
                                const auto coordinateOffset = static_cast<std::size_t>(x) * 2;
                                const float targetX = coordinates[coordinateOffset];
                                const float targetY = coordinates[coordinateOffset + 1];
                                if (!std::isfinite(targetX) || !std::isfinite(targetY)) {
                                    continue;
                                }
                                const auto xFootprint =
                                    footprint(targetX, options.scale, options.pixfrac, output.width);
                                const auto yFootprint =
                                    footprint(targetY, options.scale, options.pixfrac, output.height);
                                const auto inputOffset =
                                    (static_cast<std::size_t>(row) * read.image.width + x) * read.image.channels;
                                deposit(read.image, inputOffset, xFootprint, yFootprint);
                            }
                            reportRowProgress(DrizzleProgressStage::Accumulating, row, read.image.height,
                                              alignmentEnd, frameEnd, frame + 1);
                            return true;
                        });
                    if (!projected) {
                        return drizzleError("RegistrationMappingFailed",
                                            "Failed to project distortion coordinates for frame " +
                                                std::to_string(frame));
                    }
                }
                break;
            }
            }
            if (options.alignment != DrizzleAlignment::None) {
                if (!aligned.ok) {
                    return drizzleError(aligned.errorCode,
                                        "Failed to align frame " + std::to_string(frame) + ": " + aligned.message);
                }
                recordAlignment(aligned);
            }
        } catch (const std::bad_alloc&) {
            return drizzleError("MemoryAllocationFailed", "Unable to allocate drizzle projection footprints");
        } catch (const std::length_error&) {
            return drizzleError("MemoryAllocationFailed", "Unable to allocate drizzle projection footprints");
        }
    }

    reportProgress(DrizzleProgressStage::Normalizing, framePhaseEnd, 0, 0, output.height);
    for (std::uint32_t y = 0; y < output.height; ++y) {
        for (std::uint32_t x = 0; x < output.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * output.width + x;
            const auto offset = pixel * output.channels;
            const float weight = hasAlpha(output) ? output.pixels[offset + 3] : weights[pixel];
            if (weight <= 0.0F) {
                for (std::uint16_t channel = 0; channel < output.channels; ++channel) {
                    output.pixels[offset + channel] = 0.0F;
                }
                continue;
            }
            for (std::uint16_t channel = 0; channel < output.channels; ++channel) {
                if (hasAlpha(output) && channel == 3) {
                    continue;
                }
                if (!std::isfinite(output.pixels[offset + channel])) {
                    return drizzleError("ImageValueInvalid", "Drizzle produced a non-finite output value");
                }
            }
            if (hasAlpha(output)) {
                output.pixels[offset + 3] = std::clamp(weight, 0.0F, 1.0F);
            }
        }
        reportRowProgress(DrizzleProgressStage::Normalizing, y, output.height, framePhaseEnd, 1.0, 0);
    }

    DrizzleResult result;
    result.ok = true;
    result.image = std::move(output);
    result.alignedFrames = alignedFrames;
    result.alignmentFallbacks = alignmentFallbacks;
    result.minimumAlignmentMatches = alignedFrames == 0 ? 0 : minimumAlignmentMatches;
    return result;
}

} // namespace photonstack
