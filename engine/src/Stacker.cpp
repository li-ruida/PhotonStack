#include "photonstack/Stacker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <system_error>
#include <vector>

#include "photonstack/FrameQualityAnalyzer.hpp"
#include "photonstack/ImageCodec.hpp"
#include "photonstack/Registration.hpp"

namespace photonstack {
namespace {

StackResult stackError(std::string code, std::string message) {
    StackResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

ImageBuffer createOutputLike(const ImageBuffer& image) {
    ImageBuffer output;
    output.width = image.width;
    output.height = image.height;
    output.channels = image.channels;
    output.format = image.format;
    output.colorEncoding = image.colorEncoding;
    output.sourceBitsPerChannel = image.sourceBitsPerChannel;
    output.pixels.assign(image.sampleCount(), 0.0F);
    return output;
}

bool hasAlpha(const ImageBuffer& image) {
    return image.channels >= 4;
}

bool isAlphaChannel(const ImageBuffer& image, std::uint16_t channel) {
    return hasAlpha(image) && channel == 3;
}

float pixelCoverage(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    const auto colorChannels = std::min<std::uint16_t>(3, image.channels);
    for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
        if (!std::isfinite(image.pixels[offset + channel])) {
            return 0.0F;
        }
    }
    if (!hasAlpha(image)) {
        return 1.0F;
    }
    const float alpha = image.pixels[offset + 3];
    return std::isfinite(alpha) ? std::clamp(alpha, 0.0F, 1.0F) : 0.0F;
}

float rowPixelCoverage(const float* samples, std::uint16_t channels, std::size_t offset) {
    const auto colorChannels = std::min<std::uint16_t>(3, channels);
    for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
        if (!std::isfinite(samples[offset + channel])) {
            return 0.0F;
        }
    }
    if (channels < 4) {
        return 1.0F;
    }
    const float alpha = samples[offset + 3];
    return std::isfinite(alpha) ? std::clamp(alpha, 0.0F, 1.0F) : 0.0F;
}

void setCoverageAlpha(ImageBuffer& output, std::size_t pixel, float coverage) {
    if (!hasAlpha(output)) {
        return;
    }
    output.pixels[pixel * output.channels + 3] = std::clamp(coverage, 0.0F, 1.0F);
}

class StackProgressReporter {
  public:
    StackProgressReporter(const StackOptions& options, std::size_t frameCount)
        : options_(options), frameCount_(frameCount) {}

    void report(StackProgressStage stage, double progress, std::size_t frame = 0,
                std::uint32_t row = 0, std::uint32_t rowCount = 0) {
        if (!options_.progress) {
            return;
        }
        lastProgress_ = std::max(lastProgress_, std::clamp(progress, 0.0, 1.0));
        options_.progress({
            .stage = stage,
            .progress = lastProgress_,
            .frame = frame,
            .frameCount = frameCount_,
            .row = row,
            .rowCount = rowCount,
        });
    }

    void reportRow(StackProgressStage stage, std::uint32_t row, std::uint32_t rowCount,
                   double start, double end, std::size_t frame = 0) {
        const auto completed = row + 1;
        const auto interval = std::max<std::uint32_t>(1, rowCount / 100);
        if (completed != rowCount && completed % interval != 0) {
            return;
        }
        const double fraction = rowCount == 0 ? 1.0 : static_cast<double>(completed) / rowCount;
        report(stage, start + (end - start) * fraction, frame, completed, rowCount);
    }

  private:
    const StackOptions& options_;
    std::size_t frameCount_ = 0;
    double lastProgress_ = 0.0;
};

StackResult normalizeSingleFrameCoverage(ImageBuffer image, StackProgressReporter& progress,
                                         double start, double end) {
    progress.report(StackProgressStage::Finalizing, start, 0, 0, image.height);
    if (hasAlpha(image)) {
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const auto pixel = static_cast<std::size_t>(y) * image.width + x;
                const auto offset = pixel * image.channels;
                const float coverage = pixelCoverage(image, pixel);
                if (coverage > 1.0e-6F) {
                    image.pixels[offset + 3] = coverage;
                    continue;
                }
                for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                    image.pixels[offset + channel] = 0.0F;
                }
            }
            progress.reportRow(StackProgressStage::Finalizing, y, image.height, start, end);
        }
    } else {
        progress.report(StackProgressStage::Finalizing, end);
    }
    StackResult result;
    result.ok = true;
    result.image = std::move(image);
    return result;
}

StackResult validateStackFrame(std::uint32_t referenceWidth, std::uint32_t referenceHeight,
                               std::uint16_t referenceChannels, ColorEncoding referenceColorEncoding,
                               const ImageBuffer& frame,
                               const std::filesystem::path& input) {
    if (frame.empty()) {
        return stackError("ImageBufferInvalid", "Decoded image is empty: " + input.string());
    }
    if (frame.width != referenceWidth || frame.height != referenceHeight || frame.channels != referenceChannels) {
        return stackError("ImageDimensionsMismatch",
                          "All input images must have the same dimensions and channel count");
    }
    if (frame.colorEncoding != referenceColorEncoding) {
        return stackError("ImageColorEncodingMismatch",
                          "All input images must use the same color encoding before stacking");
    }
    StackResult result;
    result.ok = true;
    return result;
}

enum class AlignmentKind {
    None,
    Translation,
    Similarity,
    Affine,
    Distortion,
};

struct AlignmentPlan {
    AlignmentKind kind = AlignmentKind::None;
    Translation translation;
    SimilarityTransform similarity;
    AffineTransform affine;
    DistortionTransform distortion;
};

StackResult estimateStreamingAlignment(const ImageBuffer& reference, const ImageBuffer& moving,
                                       const StackOptions& options, std::size_t frameIndex, AlignmentPlan& plan) {
    const Registration registration;
    StackResult result;
    result.ok = true;
    if (options.alignDistortion) {
        const auto registrationResult =
            registration.estimateDistortion(reference, moving, plan.distortion, options.registration);
        if (!registrationResult.ok) {
            return stackError(registrationResult.errorCode, "Failed to align frame " + std::to_string(frameIndex) +
                                                                ": " + registrationResult.message);
        }
        plan.kind = AlignmentKind::Distortion;
        result.alignmentFallbacks = registrationResult.usedFallback ? 1 : 0;
        result.minimumAlignmentMatches = registrationResult.matches;
    } else if (options.alignAffine) {
        const auto registrationResult = registration.estimateAffine(reference, moving, options.registration);
        if (!registrationResult.ok) {
            return stackError(registrationResult.errorCode, "Failed to align frame " + std::to_string(frameIndex) +
                                                                ": " + registrationResult.message);
        }
        plan.kind = AlignmentKind::Affine;
        plan.affine = registrationResult.affine;
        result.alignmentFallbacks = registrationResult.usedFallback ? 1 : 0;
        result.minimumAlignmentMatches = registrationResult.matches;
    } else if (options.alignSimilarity) {
        const auto registrationResult = registration.estimateSimilarity(reference, moving, options.registration);
        if (!registrationResult.ok) {
            return stackError(registrationResult.errorCode, "Failed to align frame " + std::to_string(frameIndex) +
                                                                ": " + registrationResult.message);
        }
        plan.kind = AlignmentKind::Similarity;
        plan.similarity = registrationResult.transform;
        result.alignmentFallbacks = registrationResult.usedFallback ? 1 : 0;
        result.minimumAlignmentMatches = registrationResult.matches;
    } else if (options.alignTranslation) {
        const auto registrationResult = registration.estimateTranslation(reference, moving, options.registration);
        if (!registrationResult.ok) {
            return stackError(registrationResult.errorCode, "Failed to align frame " + std::to_string(frameIndex) +
                                                                ": " + registrationResult.message);
        }
        plan.kind = AlignmentKind::Translation;
        plan.translation = registrationResult.translation;
        result.minimumAlignmentMatches = registrationResult.matches;
    } else {
        return result;
    }
    result.alignedFrames = 1;
    return result;
}

bool renderAlignmentRows(const ImageBuffer& image, const AlignmentPlan& plan, const RegistrationRowConsumer& consumer) {
    const Registration registration;
    switch (plan.kind) {
    case AlignmentKind::Translation:
        return registration.renderTranslationRows(image, plan.translation, consumer);
    case AlignmentKind::Similarity:
        return registration.renderSimilarityRows(image, plan.similarity, consumer);
    case AlignmentKind::Affine:
        return registration.renderAffineRows(image, plan.affine, consumer);
    case AlignmentKind::Distortion:
        return registration.renderDistortionRows(image, plan.distortion, consumer);
    case AlignmentKind::None:
        break;
    }
    return false;
}

void accumulateStreamingFrame(ImageBuffer& accumulator, const ImageBuffer& frame, float weight,
                              StackProgressReporter& progress, std::size_t frameNumber,
                              double start, double end) {
    progress.report(StackProgressStage::Accumulating, start, frameNumber, 0, accumulator.height);
    for (std::uint32_t y = 0; y < accumulator.height; ++y) {
        for (std::uint32_t x = 0; x < accumulator.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * accumulator.width + x;
            const float coverage = pixelCoverage(frame, pixel);
            if (coverage <= 1.0e-6F) {
                continue;
            }
            const auto inputOffset = pixel * frame.channels;
            const auto outputOffset = pixel * accumulator.channels;
            const float contributionWeight = weight * coverage;
            for (std::uint16_t channel = 0; channel < accumulator.channels; ++channel) {
                if (!isAlphaChannel(accumulator, channel)) {
                    accumulator.pixels[outputOffset + channel] +=
                        frame.pixels[inputOffset + channel] * contributionWeight;
                }
            }
            if (hasAlpha(accumulator)) {
                accumulator.pixels[outputOffset + 3] += contributionWeight;
            }
        }
        progress.reportRow(StackProgressStage::Accumulating, y, accumulator.height, start, end, frameNumber);
    }
}

bool accumulateStreamingRow(ImageBuffer& accumulator, float weight, std::uint32_t row, const float* samples,
                            std::size_t sampleCount) {
    const auto expectedSamples = static_cast<std::size_t>(accumulator.width) * accumulator.channels;
    if (row >= accumulator.height || samples == nullptr || sampleCount != expectedSamples) {
        return false;
    }
    for (std::uint32_t x = 0; x < accumulator.width; ++x) {
        const auto inputOffset = static_cast<std::size_t>(x) * accumulator.channels;
        const float coverage = rowPixelCoverage(samples, accumulator.channels, inputOffset);
        if (coverage <= 1.0e-6F) {
            continue;
        }
        const auto outputOffset = (static_cast<std::size_t>(row) * accumulator.width + x) * accumulator.channels;
        const float contributionWeight = weight * coverage;
        for (std::uint16_t channel = 0; channel < accumulator.channels; ++channel) {
            if (!isAlphaChannel(accumulator, channel)) {
                accumulator.pixels[outputOffset + channel] += samples[inputOffset + channel] * contributionWeight;
            }
        }
        if (hasAlpha(accumulator)) {
            accumulator.pixels[outputOffset + 3] += contributionWeight;
        }
    }
    return true;
}

void finalizeStreamingAccumulator(ImageBuffer& accumulator, float totalWeight,
                                  StackProgressReporter& progress, double start, double end) {
    progress.report(StackProgressStage::Finalizing, start, 0, 0, accumulator.height);
    for (std::uint32_t y = 0; y < accumulator.height; ++y) {
        for (std::uint32_t x = 0; x < accumulator.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * accumulator.width + x;
            const auto offset = pixel * accumulator.channels;
            const float validWeight = hasAlpha(accumulator) ? accumulator.pixels[offset + 3] : totalWeight;
            if (validWeight > 0.0F) {
                for (std::uint16_t channel = 0; channel < accumulator.channels; ++channel) {
                    if (!isAlphaChannel(accumulator, channel)) {
                        accumulator.pixels[offset + channel] /= validWeight;
                    }
                }
            }
            if (hasAlpha(accumulator)) {
                accumulator.pixels[offset + 3] =
                    totalWeight > 0.0F ? std::clamp(validWeight / totalWeight, 0.0F, 1.0F) : 0.0F;
            }
        }
        progress.reportRow(StackProgressStage::Finalizing, y, accumulator.height, start, end);
    }
}

struct TemporaryFrameCache {
    std::filesystem::path directory;

    TemporaryFrameCache() = default;
    TemporaryFrameCache(const TemporaryFrameCache&) = delete;
    TemporaryFrameCache& operator=(const TemporaryFrameCache&) = delete;

    ~TemporaryFrameCache() {
        if (directory.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    [[nodiscard]] std::filesystem::path framePath(std::size_t index) const {
        return directory / ("frame-" + std::to_string(index) + ".f32");
    }
};

StackResult createTemporaryFrameCache(TemporaryFrameCache& cache);
StackResult validateTemporaryStorage(const std::filesystem::path& directory, std::size_t frameBytes,
                                     std::size_t frameCount);
StackResult writeCachedAlignedFrame(const std::filesystem::path& path, const ImageBuffer& frame,
                                    const AlignmentPlan& alignment, std::vector<float>* luminance = nullptr,
                                    StackProgressReporter* progress = nullptr, std::size_t frameNumber = 0,
                                    double start = 0.0, double end = 0.0);
StackResult accumulateCachedFrame(const std::filesystem::path& path, ImageBuffer& accumulator, float weight,
                                  StackProgressReporter* progress = nullptr, std::size_t frameNumber = 0,
                                  double start = 0.0, double end = 0.0);

StackResult streamAverageFiles(const std::vector<std::filesystem::path>& inputs, const StackOptions& options,
                               bool weighted) {
    if (inputs.empty()) {
        return stackError("InputMissing", "At least one input image is required");
    }

    StackProgressReporter progress(options, inputs.size());
    constexpr double framePhaseEnd = 0.82;
    const double frameSpan = framePhaseEnd / static_cast<double>(inputs.size());
    const std::size_t referenceIndex = options.alignDistortion && inputs.size() > 2 ? inputs.size() / 2 : 0;
    const ImageCodec codec;
    ImageReadOptions readOptions;
    readOptions.raw = options.raw;
    readOptions.fits.mode = options.fitsDecodeMode;
    readOptions.fits.maskNonFinitePixels = true;
    progress.report(StackProgressStage::Reading, 0.0, 1);
    auto referenceResult = codec.read(inputs[referenceIndex], readOptions);
    if (!referenceResult.ok) {
        return stackError(referenceResult.errorCode,
                          "Failed to read " + inputs[referenceIndex].string() + ": " + referenceResult.message);
    }
    if (referenceResult.image.empty()) {
        return stackError("ImageBufferInvalid", "Decoded image is empty: " + inputs[referenceIndex].string());
    }
    const double referenceReadEnd = frameSpan * 0.08;
    progress.report(StackProgressStage::Reading, referenceReadEnd, 1);
    if (inputs.size() == 1) {
        return normalizeSingleFrameCoverage(std::move(referenceResult.image), progress, referenceReadEnd, 1.0);
    }

    const bool requiresAlignment =
        options.alignDistortion || options.alignAffine || options.alignSimilarity || options.alignTranslation;
    ImageBuffer reference = std::move(referenceResult.image);
    const auto referenceWidth = reference.width;
    const auto referenceHeight = reference.height;
    const auto referenceChannels = reference.channels;
    const auto referenceColorEncoding = reference.colorEncoding;
    ImageBuffer accumulator = createOutputLike(reference);
    TemporaryFrameCache weightedAlignmentCache;
    if (weighted && requiresAlignment) {
        const auto cacheResult = createTemporaryFrameCache(weightedAlignmentCache);
        if (!cacheResult.ok) {
            return cacheResult;
        }
        const auto storageResult = validateTemporaryStorage(weightedAlignmentCache.directory,
                                                            reference.sampleCount() * sizeof(float), 1);
        if (!storageResult.ok) {
            return storageResult;
        }
    }
    const FrameQualityAnalyzer analyzer;
    const auto frameWeight = [&](const ImageBuffer& frame) {
        if (!weighted) {
            return 1.0F;
        }
        const auto quality = analyzer.analyze(frame);
        return quality.ok ? std::max(0.01F, quality.score) : 1.0F;
    };

    float totalWeight = frameWeight(reference);
    accumulateStreamingFrame(accumulator, reference, totalWeight, progress, 1, referenceReadEnd, frameSpan);
    if (!requiresAlignment) {
        std::vector<float>().swap(reference.pixels);
    }
    StackResult metrics;
    metrics.ok = true;
    std::size_t processedFrames = 1;

    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (index == referenceIndex) {
            continue;
        }
        ++processedFrames;
        const double frameStart = frameSpan * static_cast<double>(processedFrames - 1);
        const double frameEnd = frameStart + frameSpan;
        const double readEnd = frameStart + frameSpan * 0.08;
        const double alignmentEnd = frameStart + frameSpan * 0.45;
        progress.report(StackProgressStage::Reading, frameStart, processedFrames);
        auto readResult = codec.read(inputs[index], readOptions);
        if (!readResult.ok) {
            return stackError(readResult.errorCode,
                              "Failed to read " + inputs[index].string() + ": " + readResult.message);
        }
        const auto validation = validateStackFrame(referenceWidth, referenceHeight, referenceChannels,
                                                   referenceColorEncoding, readResult.image, inputs[index]);
        if (!validation.ok) {
            return validation;
        }
        progress.report(StackProgressStage::Reading, readEnd, processedFrames);

        AlignmentPlan alignmentPlan;
        StackResult alignment;
        alignment.ok = true;
        if (requiresAlignment) {
            progress.report(StackProgressStage::Aligning, readEnd, processedFrames);
            alignment = estimateStreamingAlignment(reference, readResult.image, options, index, alignmentPlan);
            progress.report(StackProgressStage::Aligning, alignmentEnd, processedFrames);
        }
        if (!alignment.ok) {
            return alignment;
        }

        if (requiresAlignment && !weighted) {
            constexpr float weight = 1.0F;
            progress.report(StackProgressStage::Accumulating, alignmentEnd, processedFrames, 0,
                            readResult.image.height);
            const bool rendered = renderAlignmentRows(
                readResult.image, alignmentPlan, [&](std::uint32_t row, const float* samples, std::size_t sampleCount) {
                    const bool accumulated = accumulateStreamingRow(accumulator, weight, row, samples, sampleCount);
                    if (accumulated) {
                        progress.reportRow(StackProgressStage::Accumulating, row, readResult.image.height,
                                           alignmentEnd, frameEnd, processedFrames);
                    }
                    return accumulated;
                });
            if (!rendered) {
                return stackError("ImageBufferInvalid",
                                  "Alignment row rendering failed for frame " + std::to_string(index));
            }
            totalWeight += weight;
        } else if (requiresAlignment) {
            std::vector<float> luminance;
            const auto cachePath = weightedAlignmentCache.framePath(0);
            const double cacheEnd = frameStart + frameSpan * 0.68;
            const double accumulationStart = frameStart + frameSpan * 0.74;
            const auto writeResult = writeCachedAlignedFrame(cachePath, readResult.image, alignmentPlan, &luminance,
                                                             &progress, processedFrames, alignmentEnd, cacheEnd);
            if (!writeResult.ok) {
                return writeResult;
            }
            const auto quality = analyzer.analyzeLuminance(referenceWidth, referenceHeight, luminance);
            const float weight = quality.ok ? std::max(0.01F, quality.score) : 1.0F;
            std::vector<float>().swap(luminance);
            const auto accumulationResult = accumulateCachedFrame(cachePath, accumulator, weight, &progress,
                                                                  processedFrames, accumulationStart, frameEnd);
            if (!accumulationResult.ok) {
                return accumulationResult;
            }
            totalWeight += weight;
        } else {
            const float weight = frameWeight(readResult.image);
            totalWeight += weight;
            accumulateStreamingFrame(accumulator, readResult.image, weight, progress, processedFrames,
                                     readEnd, frameEnd);
        }
        metrics.alignedFrames += alignment.alignedFrames;
        metrics.alignmentFallbacks += alignment.alignmentFallbacks;
        if (alignment.minimumAlignmentMatches > 0) {
            metrics.minimumAlignmentMatches =
                metrics.minimumAlignmentMatches == 0
                    ? alignment.minimumAlignmentMatches
                    : std::min(metrics.minimumAlignmentMatches, alignment.minimumAlignmentMatches);
        }
    }

    if (totalWeight <= 0.0F) {
        return stackError("StackWeightInvalid", "Average produced zero total weight");
    }
    finalizeStreamingAccumulator(accumulator, totalWeight, progress, framePhaseEnd, 1.0);
    metrics.image = std::move(accumulator);
    return metrics;
}

StackResult createTemporaryFrameCache(TemporaryFrameCache& cache) {
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path(error);
    if (error) {
        return stackError("TemporaryStorageUnavailable", "Unable to find the temporary directory: " + error.message());
    }

    static std::atomic<std::uint64_t> sequence = 0;
    const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (std::size_t attempt = 0; attempt < 64; ++attempt) {
        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto candidate = root / ("photonstack-stack-cache-" + std::to_string(timestamp) + "-" +
                                       std::to_string(id));
        error.clear();
        if (std::filesystem::create_directory(candidate, error)) {
            std::filesystem::permissions(candidate, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            if (error) {
                std::error_code cleanupError;
                std::filesystem::remove_all(candidate, cleanupError);
                return stackError("TemporaryStorageUnavailable",
                                  "Unable to secure the stack cache: " + error.message());
            }
            cache.directory = candidate;
            StackResult result;
            result.ok = true;
            return result;
        }
        if (error) {
            return stackError("TemporaryStorageUnavailable", "Unable to create the stack cache: " + error.message());
        }
    }
    return stackError("TemporaryStorageUnavailable", "Unable to allocate a unique stack cache directory");
}

StackResult validateTemporaryStorage(const std::filesystem::path& directory, std::size_t frameBytes,
                                     std::size_t frameCount) {
    if (frameCount > 0 && frameBytes > std::numeric_limits<std::uintmax_t>::max() / frameCount) {
        return stackError("TemporaryStorageInsufficient", "Stack cache size exceeds the supported limit");
    }
    const auto requiredBytes = static_cast<std::uintmax_t>(frameBytes) * frameCount;
    std::error_code error;
    const auto space = std::filesystem::space(directory, error);
    if (error) {
        return stackError("TemporaryStorageUnavailable", "Unable to inspect temporary storage: " + error.message());
    }
    constexpr std::uintmax_t minimumReserve = 64ULL * 1024ULL * 1024ULL;
    if (space.available < requiredBytes || space.available - requiredBytes < minimumReserve) {
        return stackError("TemporaryStorageInsufficient",
                          "Stack requires " + std::to_string(requiredBytes) + " temporary bytes, but only " +
                              std::to_string(space.available) + " are available");
    }
    StackResult result;
    result.ok = true;
    return result;
}

StackResult writeCachedFrame(const std::filesystem::path& path, const ImageBuffer& frame,
                             StackProgressReporter* progress = nullptr, std::size_t frameNumber = 0,
                             double start = 0.0, double end = 0.0) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return stackError("TemporaryStorageWriteFailed", "Unable to open stack cache file: " + path.string());
    }
    if (progress != nullptr) {
        progress->report(StackProgressStage::Caching, start, frameNumber, 0, frame.height);
    }
    const auto rowSamples = static_cast<std::size_t>(frame.width) * frame.channels;
    const auto rowsPerChunk = std::max<std::uint32_t>(1, frame.height / 100);
    for (std::uint32_t startRow = 0; startRow < frame.height; startRow += rowsPerChunk) {
        const auto rowCount = std::min(rowsPerChunk, frame.height - startRow);
        const auto sampleOffset = static_cast<std::size_t>(startRow) * rowSamples;
        const auto sampleCount = static_cast<std::size_t>(rowCount) * rowSamples;
        output.write(reinterpret_cast<const char*>(frame.pixels.data() + sampleOffset),
                     static_cast<std::streamsize>(sampleCount * sizeof(float)));
        if (!output) {
            break;
        }
        if (progress != nullptr) {
            progress->reportRow(StackProgressStage::Caching, startRow + rowCount - 1, frame.height,
                                start, end, frameNumber);
        }
    }
    output.close();
    if (!output) {
        return stackError("TemporaryStorageWriteFailed", "Unable to write stack cache file: " + path.string());
    }
    StackResult result;
    result.ok = true;
    return result;
}

StackResult writeCachedAlignedFrame(const std::filesystem::path& path, const ImageBuffer& frame,
                                    const AlignmentPlan& alignment, std::vector<float>* luminance,
                                    StackProgressReporter* progress, std::size_t frameNumber,
                                    double start, double end) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return stackError("TemporaryStorageWriteFailed", "Unable to open stack cache file: " + path.string());
    }
    if (luminance != nullptr) {
        luminance->assign(frame.pixelCount(), 0.0F);
    }
    if (progress != nullptr) {
        progress->report(StackProgressStage::Caching, start, frameNumber, 0, frame.height);
    }
    const bool rendered =
        renderAlignmentRows(frame, alignment, [&](std::uint32_t row, const float* samples, std::size_t sampleCount) {
            output.write(reinterpret_cast<const char*>(samples),
                         static_cast<std::streamsize>(sampleCount * sizeof(float)));
            if (luminance != nullptr) {
                const auto rowOffset = static_cast<std::size_t>(row) * frame.width;
                for (std::uint32_t x = 0; x < frame.width; ++x) {
                    const auto sampleOffset = static_cast<std::size_t>(x) * frame.channels;
                    bool valid = true;
                    const auto colorChannels = std::min<std::uint16_t>(3, frame.channels);
                    for (std::uint16_t channel = 0; channel < colorChannels; ++channel) {
                        valid = valid && std::isfinite(samples[sampleOffset + channel]);
                    }
                    if (frame.channels == 4) {
                        valid = valid && std::isfinite(samples[sampleOffset + 3]) &&
                                samples[sampleOffset + 3] > 1.0e-6F;
                    }
                    (*luminance)[rowOffset + x] =
                        !valid
                            ? std::numeric_limits<float>::quiet_NaN()
                            : (frame.channels >= 3
                                   ? samples[sampleOffset] * 0.2126F + samples[sampleOffset + 1] * 0.7152F +
                                         samples[sampleOffset + 2] * 0.0722F
                                   : samples[sampleOffset]);
                }
            }
            const bool written = static_cast<bool>(output);
            if (written && progress != nullptr) {
                progress->reportRow(StackProgressStage::Caching, row, frame.height, start, end, frameNumber);
            }
            return written;
        });
    output.close();
    if (!rendered || !output) {
        return stackError("TemporaryStorageWriteFailed", "Unable to write aligned stack cache file: " + path.string());
    }
    StackResult result;
    result.ok = true;
    return result;
}

StackResult accumulateCachedFrame(const std::filesystem::path& path, ImageBuffer& accumulator, float weight,
                                  StackProgressReporter* progress, std::size_t frameNumber,
                                  double start, double end) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return stackError("TemporaryStorageReadFailed", "Unable to open stack cache file: " + path.string());
    }
    const auto rowSamples = static_cast<std::size_t>(accumulator.width) * accumulator.channels;
    const auto rowBytes = rowSamples * sizeof(float);
    std::vector<float> row(rowSamples, 0.0F);
    if (progress != nullptr) {
        progress->report(StackProgressStage::Accumulating, start, frameNumber, 0, accumulator.height);
    }
    for (std::uint32_t y = 0; y < accumulator.height; ++y) {
        input.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(rowBytes));
        if (input.gcount() != static_cast<std::streamsize>(rowBytes)) {
            return stackError("TemporaryStorageReadFailed", "Unable to read stack cache file: " + path.string());
        }
        if (!accumulateStreamingRow(accumulator, weight, y, row.data(), row.size())) {
            return stackError("ImageBufferInvalid", "Cached alignment row does not match the output image");
        }
        if (progress != nullptr) {
            progress->reportRow(StackProgressStage::Accumulating, y, accumulator.height, start, end, frameNumber);
        }
    }
    StackResult result;
    result.ok = true;
    return result;
}

StackResult validateRobustOptions(const StackOptions& options) {
    if ((options.method == StackMethod::SigmaClip || options.method == StackMethod::WinsorizedSigmaClip) &&
        ((options.method == StackMethod::SigmaClip &&
          (!std::isfinite(options.sigma.sigmaLow) || !std::isfinite(options.sigma.sigmaHigh) ||
           options.sigma.sigmaLow <= 0.0F || options.sigma.sigmaHigh <= 0.0F)) ||
         (options.method == StackMethod::WinsorizedSigmaClip &&
          (!std::isfinite(options.winsorizedSigma.sigmaLow) ||
           !std::isfinite(options.winsorizedSigma.sigmaHigh) ||
           options.winsorizedSigma.sigmaLow <= 0.0F || options.winsorizedSigma.sigmaHigh <= 0.0F)))) {
        return stackError("ArgumentInvalid", "Sigma thresholds must be greater than zero");
    }
    if (options.method == StackMethod::PercentileClip &&
        (!std::isfinite(options.percentile.low) || !std::isfinite(options.percentile.high) ||
         options.percentile.low < 0.0F || options.percentile.high > 1.0F ||
         options.percentile.low >= options.percentile.high)) {
        return stackError("ArgumentInvalid", "Percentile clip bounds must satisfy 0 <= low < high <= 1");
    }
    StackResult result;
    result.ok = true;
    return result;
}

struct WeightedValue {
    float value = 0.0F;
    float weight = 0.0F;
};

double totalValueWeight(const std::vector<WeightedValue>& values) {
    double total = 0.0;
    for (const auto& value : values) {
        total += value.weight;
    }
    return total;
}

float weightedMean(const std::vector<WeightedValue>& values) {
    double sum = 0.0;
    double total = 0.0;
    for (const auto& value : values) {
        sum += static_cast<double>(value.value) * value.weight;
        total += value.weight;
    }
    return total > 0.0 ? static_cast<float>(sum / total) : 0.0F;
}

float weightedQuantile(std::vector<WeightedValue>& values, float quantile) {
    std::sort(values.begin(), values.end(), [](const WeightedValue& left, const WeightedValue& right) {
        return left.value < right.value;
    });
    const double total = totalValueWeight(values);
    const double target = std::clamp(static_cast<double>(quantile), 0.0, 1.0);
    double cumulative = 0.0;
    double previousPosition = 0.0;
    float previousValue = values.front().value;
    bool hasPrevious = false;
    for (const auto& value : values) {
        cumulative += value.weight;
        const double position = (cumulative - static_cast<double>(value.weight) * 0.5) / total;
        if (position >= target) {
            if (!hasPrevious || position <= previousPosition) {
                return value.value;
            }
            const double fraction = std::clamp((target - previousPosition) / (position - previousPosition), 0.0, 1.0);
            return static_cast<float>(static_cast<double>(previousValue) +
                                      (static_cast<double>(value.value) - previousValue) * fraction);
        }
        previousPosition = position;
        previousValue = value.value;
        hasPrevious = true;
    }
    return values.back().value;
}

float robustStackValue(std::vector<WeightedValue>& values, std::vector<WeightedValue>& scratch,
                       const StackOptions& options) {
    switch (options.method) {
    case StackMethod::Median:
        return weightedQuantile(values, 0.5F);
    case StackMethod::SigmaClip: {
        const float mean = weightedMean(values);
        double variance = 0.0;
        const double total = totalValueWeight(values);
        for (const auto& value : values) {
            const double delta = static_cast<double>(value.value) - mean;
            variance += delta * delta * value.weight;
        }
        variance /= total;
        const float standardDeviation = static_cast<float>(std::sqrt(variance));
        const float low = mean - options.sigma.sigmaLow * standardDeviation;
        const float high = mean + options.sigma.sigmaHigh * standardDeviation;
        double sum = 0.0;
        double acceptedWeight = 0.0;
        for (const auto& value : values) {
            if (value.value >= low && value.value <= high) {
                sum += static_cast<double>(value.value) * value.weight;
                acceptedWeight += value.weight;
            }
        }
        return acceptedWeight <= 0.0 ? mean : static_cast<float>(sum / acceptedWeight);
    }
    case StackMethod::WinsorizedSigmaClip: {
        scratch.assign(values.begin(), values.end());
        const float median = weightedQuantile(scratch, 0.5F);
        scratch.assign(values.begin(), values.end());
        for (auto& value : scratch) {
            value.value = std::fabs(value.value - median);
        }
        const float sigma = std::max(1.0e-6F, weightedQuantile(scratch, 0.5F) * 1.4826F);
        const float low = median - options.winsorizedSigma.sigmaLow * sigma;
        const float high = median + options.winsorizedSigma.sigmaHigh * sigma;
        double sum = 0.0;
        const double total = totalValueWeight(values);
        for (const auto& value : values) {
            sum += static_cast<double>(std::clamp(value.value, low, high)) * value.weight;
        }
        return static_cast<float>(sum / total);
    }
    case StackMethod::PercentileClip: {
        scratch.assign(values.begin(), values.end());
        const float low = weightedQuantile(scratch, options.percentile.low);
        scratch.assign(values.begin(), values.end());
        const float high = weightedQuantile(scratch, options.percentile.high);
        double sum = 0.0;
        double acceptedWeight = 0.0;
        for (const auto& value : values) {
            if (value.value >= low && value.value <= high) {
                sum += static_cast<double>(value.value) * value.weight;
                acceptedWeight += value.weight;
            }
        }
        if (acceptedWeight > 0.0) {
            return static_cast<float>(sum / acceptedWeight);
        }
        scratch.assign(values.begin(), values.end());
        return weightedQuantile(scratch, 0.5F);
    }
    case StackMethod::Average:
    case StackMethod::WeightedAverage:
        return 0.0F;
    }
    return 0.0F;
}

void processRobustTile(const std::vector<float>& tileData, std::size_t frameCount, std::size_t tilePixels,
                       std::uint16_t channels, std::size_t outputPixelOffset, const StackOptions& options,
                       ImageBuffer& output) {
    const auto tileSamples = tilePixels * channels;
    std::vector<WeightedValue> values;
    std::vector<WeightedValue> scratch;
    std::vector<float> coverages(frameCount, 0.0F);
    values.reserve(frameCount);
    scratch.reserve(frameCount);

    for (std::size_t pixel = 0; pixel < tilePixels; ++pixel) {
        double coverageSum = 0.0;
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            const auto frameOffset = frame * tileSamples + pixel * channels;
            coverages[frame] = rowPixelCoverage(tileData.data(), channels, frameOffset);
            coverageSum += coverages[frame];
        }

        const auto outputOffset = (outputPixelOffset + pixel) * channels;
        for (std::uint16_t channel = 0; channel < channels; ++channel) {
            if (channels >= 4 && channel == 3) {
                continue;
            }
            values.clear();
            for (std::size_t frame = 0; frame < frameCount; ++frame) {
                const auto frameOffset = frame * tileSamples + pixel * channels;
                const float coverage = coverages[frame];
                const float value = tileData[frameOffset + channel];
                if (coverage <= 1.0e-6F || !std::isfinite(value)) {
                    continue;
                }
                values.push_back({value, coverage});
            }
            if (!values.empty()) {
                output.pixels[outputOffset + channel] = robustStackValue(values, scratch, options);
            }
        }
        setCoverageAlpha(output, outputPixelOffset + pixel,
                         static_cast<float>(coverageSum / static_cast<double>(frameCount)));
    }
}

StackResult stackCachedRobustFrames(const TemporaryFrameCache& cache, std::size_t frameCount,
                                    const StackOptions& options, ImageBuffer output, StackResult metrics,
                                    StackProgressReporter& progress, double start, double end) {
    std::vector<std::ifstream> streams;
    streams.reserve(frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        streams.emplace_back(cache.framePath(frame), std::ios::binary);
        if (!streams.back()) {
            return stackError("TemporaryStorageReadFailed",
                              "Unable to open stack cache file: " + cache.framePath(frame).string());
        }
    }

    constexpr std::size_t tileBudgetBytes = 64ULL * 1024ULL * 1024ULL;
    const auto rowSamples = static_cast<std::size_t>(output.width) * output.channels;
    const auto allFramesRowBytes = rowSamples * sizeof(float) * frameCount;
    const auto tileRows = std::max<std::size_t>(1, tileBudgetBytes / std::max<std::size_t>(1, allFramesRowBytes));
    std::vector<float> tileData;
    progress.report(StackProgressStage::Combining, start, 0, 0, output.height);

    for (std::size_t startY = 0; startY < output.height; startY += tileRows) {
        const auto rowCount = std::min<std::size_t>(tileRows, output.height - startY);
        const auto tileSamples = rowCount * rowSamples;
        tileData.resize(tileSamples * frameCount);
        const auto byteOffset = startY * rowSamples * sizeof(float);
        const auto byteCount = tileSamples * sizeof(float);

        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            auto& stream = streams[frame];
            stream.clear();
            stream.seekg(static_cast<std::streamoff>(byteOffset), std::ios::beg);
            if (!stream) {
                return stackError("TemporaryStorageReadFailed",
                                  "Unable to seek stack cache file: " + cache.framePath(frame).string());
            }
            stream.read(reinterpret_cast<char*>(tileData.data() + frame * tileSamples),
                        static_cast<std::streamsize>(byteCount));
            if (stream.gcount() != static_cast<std::streamsize>(byteCount)) {
                return stackError("TemporaryStorageReadFailed",
                                  "Unable to read stack cache file: " + cache.framePath(frame).string());
            }
        }

        processRobustTile(tileData, frameCount, rowCount * output.width, output.channels,
                          startY * output.width, options, output);
        const auto completedRows = static_cast<std::uint32_t>(startY + rowCount);
        const auto previousRows = static_cast<std::uint32_t>(startY);
        const auto interval = std::max<std::uint32_t>(1, output.height / 100);
        if (completedRows == output.height || completedRows / interval != previousRows / interval) {
            const double fraction = static_cast<double>(completedRows) / output.height;
            progress.report(StackProgressStage::Combining, start + (end - start) * fraction,
                            0, completedRows, output.height);
        }
    }

    metrics.image = std::move(output);
    metrics.ok = true;
    return metrics;
}

StackResult streamRobustFiles(const std::vector<std::filesystem::path>& inputs, const StackOptions& options) {
    if (inputs.empty()) {
        return stackError("InputMissing", "At least one input image is required");
    }
    const auto optionValidation = validateRobustOptions(options);
    if (!optionValidation.ok) {
        return optionValidation;
    }

    StackProgressReporter progress(options, inputs.size());
    constexpr double framePhaseEnd = 0.82;
    const double frameSpan = framePhaseEnd / static_cast<double>(inputs.size());
    std::vector<std::size_t> inputOrder(inputs.size());
    std::iota(inputOrder.begin(), inputOrder.end(), 0);
    if (options.alignDistortion && inputOrder.size() > 2) {
        std::swap(inputOrder.front(), inputOrder[inputOrder.size() / 2]);
    }

    const ImageCodec codec;
    ImageReadOptions readOptions;
    readOptions.raw = options.raw;
    readOptions.fits.mode = options.fitsDecodeMode;
    readOptions.fits.maskNonFinitePixels = true;
    const auto referenceInputIndex = inputOrder.front();
    progress.report(StackProgressStage::Reading, 0.0, 1);
    auto referenceResult = codec.read(inputs[referenceInputIndex], readOptions);
    if (!referenceResult.ok) {
        return stackError(referenceResult.errorCode,
                          "Failed to read " + inputs[referenceInputIndex].string() + ": " + referenceResult.message);
    }
    if (referenceResult.image.empty()) {
        return stackError("ImageBufferInvalid", "Decoded image is empty: " + inputs[referenceInputIndex].string());
    }
    const double referenceReadEnd = frameSpan * 0.08;
    progress.report(StackProgressStage::Reading, referenceReadEnd, 1);
    if (inputs.size() == 1) {
        return normalizeSingleFrameCoverage(std::move(referenceResult.image), progress, referenceReadEnd, 1.0);
    }

    TemporaryFrameCache cache;
    const auto cacheResult = createTemporaryFrameCache(cache);
    if (!cacheResult.ok) {
        return cacheResult;
    }

    const bool requiresAlignment = options.alignDistortion || options.alignAffine || options.alignSimilarity ||
                                   options.alignTranslation;
    ImageBuffer reference = std::move(referenceResult.image);
    const auto referenceWidth = reference.width;
    const auto referenceHeight = reference.height;
    const auto referenceChannels = reference.channels;
    const auto referenceColorEncoding = reference.colorEncoding;
    const auto frameBytes = reference.pixels.size() * sizeof(float);
    const auto storageResult = validateTemporaryStorage(cache.directory, frameBytes, inputs.size());
    if (!storageResult.ok) {
        return storageResult;
    }
    const auto referenceWrite =
        writeCachedFrame(cache.framePath(0), reference, &progress, 1, referenceReadEnd, frameSpan);
    if (!referenceWrite.ok) {
        return referenceWrite;
    }

    StackResult metrics;
    metrics.ok = true;
    if (!requiresAlignment) {
        std::vector<float>().swap(reference.pixels);
    }

    for (std::size_t position = 1; position < inputOrder.size(); ++position) {
        const auto inputIndex = inputOrder[position];
        const auto frameNumber = position + 1;
        const double frameStart = frameSpan * static_cast<double>(position);
        const double frameEnd = frameStart + frameSpan;
        const double readEnd = frameStart + frameSpan * 0.08;
        const double alignmentEnd = frameStart + frameSpan * 0.45;
        progress.report(StackProgressStage::Reading, frameStart, frameNumber);
        auto readResult = codec.read(inputs[inputIndex], readOptions);
        if (!readResult.ok) {
            return stackError(readResult.errorCode,
                              "Failed to read " + inputs[inputIndex].string() + ": " + readResult.message);
        }
        const auto validation = validateStackFrame(referenceWidth, referenceHeight, referenceChannels,
                                                   referenceColorEncoding, readResult.image, inputs[inputIndex]);
        if (!validation.ok) {
            return validation;
        }
        progress.report(StackProgressStage::Reading, readEnd, frameNumber);

        AlignmentPlan alignmentPlan;
        StackResult alignment;
        alignment.ok = true;
        if (requiresAlignment) {
            progress.report(StackProgressStage::Aligning, readEnd, frameNumber);
            alignment = estimateStreamingAlignment(reference, readResult.image, options, position, alignmentPlan);
            progress.report(StackProgressStage::Aligning, alignmentEnd, frameNumber);
        }
        if (!alignment.ok) {
            return alignment;
        }
        const double cacheStart = requiresAlignment ? alignmentEnd : readEnd;
        const auto writeResult = requiresAlignment
                                     ? writeCachedAlignedFrame(cache.framePath(position), readResult.image,
                                                               alignmentPlan, nullptr, &progress, frameNumber,
                                                               cacheStart, frameEnd)
                                     : writeCachedFrame(cache.framePath(position), readResult.image, &progress,
                                                        frameNumber, cacheStart, frameEnd);
        if (!writeResult.ok) {
            return writeResult;
        }

        metrics.alignedFrames += alignment.alignedFrames;
        metrics.alignmentFallbacks += alignment.alignmentFallbacks;
        if (alignment.minimumAlignmentMatches > 0) {
            metrics.minimumAlignmentMatches =
                metrics.minimumAlignmentMatches == 0
                    ? alignment.minimumAlignmentMatches
                    : std::min(metrics.minimumAlignmentMatches, alignment.minimumAlignmentMatches);
        }
    }

    std::vector<float>().swap(reference.pixels);
    ImageBuffer output;
    output.width = referenceWidth;
    output.height = referenceHeight;
    output.channels = referenceChannels;
    output.format = reference.format;
    output.colorEncoding = reference.colorEncoding;
    output.sourceBitsPerChannel = reference.sourceBitsPerChannel;
    output.pixels.assign(output.sampleCount(), 0.0F);
    return stackCachedRobustFrames(cache, inputs.size(), options, std::move(output), metrics, progress,
                                   framePhaseEnd, 1.0);
}

} // namespace

StackResult Stacker::stack(const std::vector<std::filesystem::path>& inputs, const StackOptions& options) const {
    if (!isValidRawDecodeOptions(options.raw)) {
        return stackError("ArgumentInvalid", "Unsupported RAW decode options");
    }
    if (options.fitsDecodeMode != FitsDecodeMode::DisplayNormalized &&
        options.fitsDecodeMode != FitsDecodeMode::Scientific) {
        return stackError("ArgumentInvalid", "Unsupported FITS decode mode");
    }
    switch (options.method) {
    case StackMethod::Average:
        return streamAverageFiles(inputs, options, false);
    case StackMethod::WeightedAverage:
        return streamAverageFiles(inputs, options, true);
    case StackMethod::Median:
    case StackMethod::SigmaClip:
    case StackMethod::WinsorizedSigmaClip:
    case StackMethod::PercentileClip:
        return streamRobustFiles(inputs, options);
    }
    return stackError("StackMethodInvalid", "Unsupported stack method");
}

StackResult Stacker::average(const std::vector<std::filesystem::path>& inputs) const {
    return stack(inputs, {.method = StackMethod::Average});
}

StackResult Stacker::weightedAverage(const std::vector<std::filesystem::path>& inputs) const {
    return stack(inputs, {.method = StackMethod::WeightedAverage});
}

StackResult Stacker::median(const std::vector<std::filesystem::path>& inputs) const {
    return stack(inputs, {.method = StackMethod::Median});
}

StackResult Stacker::sigmaClip(const std::vector<std::filesystem::path>& inputs,
                               const SigmaClipOptions& options) const {
    return stack(inputs, {.method = StackMethod::SigmaClip, .sigma = options});
}

StackResult Stacker::winsorizedSigmaClip(const std::vector<std::filesystem::path>& inputs,
                                         const WinsorizedSigmaClipOptions& options) const {
    StackOptions stackOptions;
    stackOptions.method = StackMethod::WinsorizedSigmaClip;
    stackOptions.winsorizedSigma = options;
    return stack(inputs, stackOptions);
}

StackResult Stacker::percentileClip(const std::vector<std::filesystem::path>& inputs,
                                    const PercentileClipOptions& options) const {
    StackOptions stackOptions;
    stackOptions.method = StackMethod::PercentileClip;
    stackOptions.percentile = options;
    return stack(inputs, stackOptions);
}

} // namespace photonstack
