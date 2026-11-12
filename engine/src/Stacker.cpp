#ifdef __APPLE__
#include "MetalCompute.hpp"
#endif
#include "RegisteredStackCache.hpp"
#include "WorkingMemory.hpp"
#include <cstring>
#include "photonstack/WorkerPool.hpp"
#include "photonstack/StageProfile.hpp"
#include "photonstack/Stacker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <exception>
#include <fstream>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#endif

#include "photonstack/FrameQualityAnalyzer.hpp"
#include "photonstack/ImageCodec.hpp"
#include "photonstack/Registration.hpp"

#include "StreamingStackAccumulator.hpp"

namespace photonstack {
namespace {

StackResult stackError(std::string code, std::string message) {
    StackResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

StackResult notifyInputConsumed(const StackOptions& options, std::size_t inputIndex) {
    try {
        if (options.inputConsumed) options.inputConsumed(inputIndex);
    } catch (const std::exception& error) {
        return stackError("InputReleaseFailed", "Unable to release consumed input " +
            std::to_string(inputIndex) + ": " + error.what());
    } catch (...) {
        return stackError("InputReleaseFailed", "Unable to release consumed input " + std::to_string(inputIndex));
    }
    StackResult result;
    result.ok = true;
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

std::array<float, 3> sampledBackground(const ImageBuffer& image) {
    const detail::StageScope timer(detail::Stage::Normalize);
    std::array<std::vector<float>, 3> values;
    const auto stride = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::sqrt(static_cast<double>(image.pixelCount()) / 65536.0)));
    const auto channels = std::min<std::uint16_t>(3, image.channels);
    for (std::uint32_t y = 0; y < image.height; y += stride) {
        for (std::uint32_t x = 0; x < image.width; x += stride) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            if (pixelCoverage(image, pixel) < 0.99F)
                continue;
            for (std::uint16_t c = 0; c < channels; ++c) {
                values[c].push_back(image.pixels[pixel * image.channels + c]);
            }
        }
    }
    std::array<float, 3> result{};
    for (std::uint16_t c = 0; c < channels; ++c) {
        if (values[c].empty())
            continue;
        auto middle = values[c].begin() + values[c].size() / 2;
        std::nth_element(values[c].begin(), middle, values[c].end());
        result[c] = *middle;
    }
    return result;
}

void matchBackground(ImageBuffer& image, const std::array<float, 3>& reference) {
    const detail::StageScope timer(detail::Stage::Normalize);
    const auto background = sampledBackground(image);
    const auto channels = std::min<std::uint16_t>(3, image.channels);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        if (pixelCoverage(image, pixel) <= 0.0F)
            continue;
        for (std::uint16_t c = 0; c < channels; ++c) {
            image.pixels[pixel * image.channels + c] += reference[c] - background[c];
        }
    }
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
    RegistrationInterpolation interpolation = RegistrationInterpolation::Bilinear;
    AlignmentKind kind = AlignmentKind::None;
    Translation translation;
    SimilarityTransform similarity;
    AffineTransform affine;
    DistortionTransform distortion;
};

StackResult estimateStreamingAlignment(const ImageBuffer& reference, const ImageBuffer& moving,
                                       const StackOptions& options, std::size_t frameIndex, AlignmentPlan& plan) {
    plan.interpolation = options.interpolation;
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
        result.centroidRefinedFrames = registrationResult.usedCentroidRefinement ? 1 : 0;
        result.centroidRefinementFallbacks = options.registration.refineSimilarityCentroids &&
                                            !registrationResult.usedCentroidRefinement ? 1 : 0;
        result.centroidAffineFrames = registrationResult.usedCentroidAffine ? 1 : 0;
        result.centroidSimilarityRetainedFrames = registrationResult.usedCentroidRefinement &&
                                                 !registrationResult.usedCentroidAffine ? 1 : 0;
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
        result.centroidRefinedFrames = registrationResult.usedCentroidRefinement ? 1 : 0;
        result.centroidRefinementFallbacks = options.registration.refineSimilarityCentroids &&
                                            !registrationResult.usedCentroidRefinement ? 1 : 0;
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
    const Registration registration(plan.interpolation);
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

bool isExtremaMethod(StackMethod method) {
    return method == StackMethod::Maximum || method == StackMethod::Minimum;
}

using detail::StreamingStackAccumulator;

void accumulateStreamingFrame(StreamingStackAccumulator& accumulator, const ImageBuffer& frame, double weight,
                              StackProgressReporter& progress, std::size_t frameNumber,
                              double start, double end) {
    progress.report(StackProgressStage::Accumulating, start, frameNumber, 0, accumulator.height());
    for (std::uint32_t y = 0; y < accumulator.height(); ++y) {
        accumulator.addRow(y, frame.pixels.data() + y * accumulator.rowSamples(), accumulator.rowSamples(), weight);
        progress.reportRow(StackProgressStage::Accumulating, y, accumulator.height(), start, end, frameNumber);
    }
}

void finalizeStreamingAccumulator(StreamingStackAccumulator& accumulator, double totalWeight,
                                  StackProgressReporter& progress, double start, double end) {
    accumulator.beginFinalization();
    progress.report(StackProgressStage::Finalizing, start, 0, 0, accumulator.height());
    for (std::uint32_t y = 0; y < accumulator.height(); ++y) {
        accumulator.finalizeRow(y, totalWeight);
        progress.reportRow(StackProgressStage::Finalizing, y, accumulator.height(), start, end);
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

StackResult createTemporaryFrameCache(TemporaryFrameCache& cache, const std::filesystem::path& parent);
StackResult validateTemporaryStorage(const std::filesystem::path& directory, std::size_t frameBytes,
                                     std::size_t frameCount);
StackResult writeCachedAlignedFrame(const std::filesystem::path& path, const ImageBuffer& frame,
                                    const AlignmentPlan& alignment, std::vector<float>* luminance = nullptr,
                                    StackProgressReporter* progress = nullptr, std::size_t frameNumber = 0,
                                    double start = 0.0, double end = 0.0);
StackResult accumulateCachedFrame(const std::filesystem::path& path, StreamingStackAccumulator& accumulator, double weight,
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
    const bool externalReference = !options.alignmentReference.empty();
    const auto& referencePath = externalReference ? options.alignmentReference : inputs[referenceIndex];
    const ImageCodec codec;
    ImageReadOptions readOptions;
    readOptions.raw = options.raw;
    readOptions.fits.mode = options.fitsDecodeMode;
    readOptions.fits.maskNonFinitePixels = true;
    readOptions.fits.debayer = options.debayerFits;
    readOptions.fits.demosaic = options.fitsDemosaic;
    readOptions.fits.cfaInterpolationGains = options.cfaInterpolationGains;
    progress.report(StackProgressStage::Reading, 0.0, 1);
    auto referenceResult = codec.read(referencePath, readOptions);
    if (!referenceResult.ok) {
        return stackError(referenceResult.errorCode,
                          "Failed to read " + referencePath.string() + ": " + referenceResult.message);
    }
    if (referenceResult.image.empty()) {
        return stackError("ImageBufferInvalid", "Decoded image is empty: " + referencePath.string());
    }
    const double referenceReadEnd = externalReference ? 0.0 : frameSpan * 0.08;
    progress.report(StackProgressStage::Reading, referenceReadEnd, 1);
    if (inputs.size() == 1 && !externalReference) {
        auto result = normalizeSingleFrameCoverage(std::move(referenceResult.image), progress, referenceReadEnd, 1.0);
        if (result.ok) {
            const auto released = notifyInputConsumed(options, 0);
            if (!released.ok) return released;
        }
        return result;
    }

    const bool requiresAlignment =
        options.alignDistortion || options.alignAffine || options.alignSimilarity || options.alignTranslation;
    ImageBuffer reference = std::move(referenceResult.image);
    const auto referenceBackground =
        options.normalizeBackground ? sampledBackground(reference) : std::array<float, 3>{};
    const auto referenceWidth = reference.width;
    const auto referenceHeight = reference.height;
    const auto referenceChannels = reference.channels;
    const auto referenceColorEncoding = reference.colorEncoding;
    StreamingStackAccumulator accumulator(reference, options.method);
    TemporaryFrameCache weightedAlignmentCache;
    if (weighted && requiresAlignment) {
        const auto cacheResult = createTemporaryFrameCache(weightedAlignmentCache, options.temporaryDirectory);
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
    const auto frameWeight = [&](const ImageBuffer& frame, std::size_t index) {
        const double explicitWeight = options.frameWeights.empty() ? 1.0F : options.frameWeights[index];
        if (!weighted) {
            return explicitWeight;
        }
        const auto quality = analyzer.analyze(frame);
        return explicitWeight * (quality.ok ? std::max(0.01F, quality.score) : 1.0F);
    };

    double totalWeight = 0;
    if (!externalReference) {
        totalWeight = frameWeight(reference, referenceIndex);
        accumulateStreamingFrame(accumulator, reference, totalWeight, progress, 1, referenceReadEnd, frameSpan);
        const auto released = notifyInputConsumed(options, referenceIndex);
        if (!released.ok) return released;
    }
    if (!requiresAlignment) {
        std::vector<float>().swap(reference.pixels);
    }
    StackResult metrics;
    metrics.ok = true;
    std::size_t processedFrames = externalReference ? 0 : 1;

    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (!externalReference && index == referenceIndex) {
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
        if (options.normalizeBackground)
            matchBackground(readResult.image, referenceBackground);

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
            const float weight = options.frameWeights.empty() ? 1.0F : options.frameWeights[index];
            progress.report(StackProgressStage::Accumulating, alignmentEnd, processedFrames, 0,
                            readResult.image.height);
            const bool rendered = renderAlignmentRows(
                readResult.image, alignmentPlan, [&](std::uint32_t row, const float* samples, std::size_t sampleCount) {
                    const bool accumulated = accumulator.addRow(row, samples, sampleCount, weight);
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
            const double weight = static_cast<double>(quality.ok ? std::max(0.01F, quality.score) : 1.0F) *
                                 (options.frameWeights.empty() ? 1.0F : options.frameWeights[index]);
            std::vector<float>().swap(luminance);
            const auto accumulationResult = accumulateCachedFrame(cachePath, accumulator, weight, &progress,
                                                                  processedFrames, accumulationStart, frameEnd);
            if (!accumulationResult.ok) {
                return accumulationResult;
            }
            totalWeight += weight;
        } else {
            const double weight = frameWeight(readResult.image, index);
            totalWeight += weight;
            accumulateStreamingFrame(accumulator, readResult.image, weight, progress, processedFrames,
                                     readEnd, frameEnd);
        }
        const auto released = notifyInputConsumed(options, index);
        if (!released.ok) return released;
        metrics.alignedFrames += alignment.alignedFrames;
        metrics.alignmentFallbacks += alignment.alignmentFallbacks;
        metrics.centroidRefinedFrames += alignment.centroidRefinedFrames;
        metrics.centroidRefinementFallbacks += alignment.centroidRefinementFallbacks;
        metrics.centroidAffineFrames += alignment.centroidAffineFrames;
        metrics.centroidSimilarityRetainedFrames += alignment.centroidSimilarityRetainedFrames;
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
    // Alignment no longer needs its reference. Release it before allocating
    // the Float32 output alongside the double-precision accumulator.
    std::vector<float>().swap(reference.pixels);
    finalizeStreamingAccumulator(accumulator, totalWeight, progress, framePhaseEnd, 1.0);
    metrics.image = accumulator.takeImage();
    return metrics;
}

StackResult createTemporaryFrameCache(TemporaryFrameCache& cache, const std::filesystem::path& parent) {
    std::error_code error;
    const auto root = parent.empty() ? std::filesystem::temp_directory_path(error) : parent;
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
    const detail::StageScope timer(detail::Stage::Write);
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

StackResult accumulateCachedFrame(const std::filesystem::path& path, StreamingStackAccumulator& accumulator, double weight,
                                  StackProgressReporter* progress, std::size_t frameNumber,
                                  double start, double end) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return stackError("TemporaryStorageReadFailed", "Unable to open stack cache file: " + path.string());
    }
    const auto rowSamples = accumulator.rowSamples();
    const auto rowBytes = rowSamples * sizeof(float);
    std::vector<float> row(rowSamples, 0.0F);
    if (progress != nullptr) {
        progress->report(StackProgressStage::Accumulating, start, frameNumber, 0, accumulator.height());
    }
    for (std::uint32_t y = 0; y < accumulator.height(); ++y) {
        input.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(rowBytes));
        if (input.gcount() != static_cast<std::streamsize>(rowBytes)) {
            return stackError("TemporaryStorageReadFailed", "Unable to read stack cache file: " + path.string());
        }
        if (!accumulator.addRow(y, row.data(), row.size(), weight)) {
            return stackError("ImageBufferInvalid", "Cached alignment row does not match the output image");
        }
        if (progress != nullptr) {
            progress->reportRow(StackProgressStage::Accumulating, y, accumulator.height(), start, end, frameNumber);
        }
    }
    StackResult result;
    result.ok = true;
    return result;
}

StackResult validateRobustOptions(const StackOptions& options) {
    if (options.sigma.iterations < 1 || options.sigma.iterations > 10) {
        return stackError("ArgumentInvalid", "Sigma rejection iterations must be between 1 and 10");
    }
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
    std::size_t frame = 0;
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

// Equal unit weights are common in fully covered scientific frames. Only the
// two central order statistics are needed. Match the weighted interpolator's
// arithmetic exactly, including its float subtraction before promotion.
float unitMedian(std::vector<float>& samples) {
    auto upper = samples.begin() + samples.size() / 2;
    std::nth_element(samples.begin(), upper, samples.end());
    if (samples.size() == 1) return *upper;
    const float lower = *std::max_element(samples.begin(), upper);
    const double n = double(samples.size());
    const double before = (double(samples.size() / 2) - .5) / n;
    const double after = (double(samples.size() / 2) + .5) / n;
    return float(double(lower) + double(*upper - lower) * ((.5 - before) / (after - before)));
}

struct PixelRejections {
    std::size_t low = 0, high = 0;
    double lowerBound = -std::numeric_limits<double>::infinity();
    double upperBound = std::numeric_limits<double>::infinity();
    StackResult* report = nullptr;
    void count(const std::vector<WeightedValue>& values, double lower, double upper) {
        lowerBound = lower;
        upperBound = upper;
        if (!report) return;
        for (const auto& v : values) {
            low += v.value < lower;
            high += v.value > upper;
            if (report) {
                ++report->comparedSamples[v.frame];
                report->rejectedLowSamples[v.frame] += v.value < lower;
                report->rejectedHighSamples[v.frame] += v.value > upper;
            }
        }
    }
};

float robustStackValue(std::vector<WeightedValue>& values, std::vector<WeightedValue>& scratch,
                       std::vector<WeightedValue>& retained, std::vector<float>& unitSamples, const StackOptions& options, PixelRejections& rejections, const float* centers = nullptr) {
    switch (options.method) {
    case StackMethod::Median:
        rejections.count(values, -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
        return weightedQuantile(values, 0.5F);
    case StackMethod::SigmaClip: {
        if (options.sigma.medianMad) {
            // A sparse contaminant must not inflate the center/scale used to
            // judge itself. Fewer than five covered samples lack enough robust
            // evidence: retain them instead of inventing a zero-MAD rejection.
            // Reuse worker-local storage across pixels/channels (and optional
            // half-stacks); neither ordering nor rejection arithmetic changes.
            retained.assign(values.begin(), values.end());
            double low = -std::numeric_limits<double>::infinity();
            double high = std::numeric_limits<double>::infinity();
            double scale = 0;
            for (std::uint32_t iteration = 0; iteration < options.sigma.iterations && retained.size() >= 5; ++iteration) {
                scratch = retained;
                const double center = weightedQuantile(scratch, .5F);
                if (iteration == 0) {
                    // Estimate scatter before cutting off the tails. Re-estimating
                    // it from the survivors can progressively discard ordinary
                    // noise on one side, shifting just one RGB channel. Later
                    // passes may refine the center using the original scatter.
                    for (auto& v : scratch) v.value = float(std::abs(double(v.value) - center));
                    scale = std::max({double(weightedQuantile(scratch, .5F)) * 1.4826022185,
                        std::abs(center) * double(std::numeric_limits<float>::epsilon()) * 2, 1.e-6});
                }
                const double candidateLow = std::max(low, center - options.sigma.sigmaLow * scale);
                const double candidateHigh = std::min(high, center + options.sigma.sigmaHigh * scale);
                scratch.clear();
                for (const auto& v : retained)
                    if (v.value >= candidateLow && v.value <= candidateHigh) scratch.push_back(v);
                if (scratch.size() < 3) break;
                low = candidateLow; high = candidateHigh;
                if (scratch.size() == retained.size()) break;
                retained = scratch;
            }
            rejections.count(values, low, high);
            return weightedMean(retained);
        }
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
        rejections.count(values, low, high);
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
        float median, mad;
        if (centers && std::isfinite(centers[0]) && std::isfinite(centers[1])) {
            median = centers[0];
            mad = centers[1];
        } else if (std::all_of(values.begin(), values.end(), [](const auto& v) { return v.weight == 1; })) {
            unitSamples.resize(values.size());
            std::transform(values.begin(), values.end(), unitSamples.begin(), [](const auto& v) { return v.value; });
            median = unitMedian(unitSamples);
            for (auto& v : unitSamples) v = std::fabs(v - median);
            mad = unitMedian(unitSamples);
        } else {
            scratch = values;
            median = weightedQuantile(scratch, .5F);
            scratch = values;
            for (auto& v : scratch) v.value = std::fabs(v.value - median);
            mad = weightedQuantile(scratch, .5F);
        }
        const float sigma = std::max(1.0e-6F, mad * 1.4826F);
        const float low = median - options.winsorizedSigma.sigmaLow * sigma;
        const float high = median + options.winsorizedSigma.sigmaHigh * sigma;
        rejections.count(values, low, high);
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
        rejections.count(values, low, high);
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
    case StackMethod::Maximum:
    case StackMethod::Minimum:
        return 0.0F;
    }
    return 0.0F;
}

struct AcceptedVariance {
    double weight = 0, squaredNoiseWeight = 0;
    std::size_t count = 0;
    double variance() const { return weight > 0 ? squaredNoiseWeight / (weight * weight) : 0; }
};

AcceptedVariance acceptedVariance(const std::vector<WeightedValue>& values, const PixelRejections& bounds,
                                   const std::vector<double>& variances) {
    AcceptedVariance result;
    for (const auto& v : values) {
        if (v.value < bounds.lowerBound || v.value > bounds.upperBound) continue;
        result.weight += v.weight;
        result.squaredNoiseWeight += double(v.weight) * v.weight * variances[v.frame];
        ++result.count;
    }
    // The mean/std sigma implementation falls back to the full mean if every
    // sample is rejected. Mirror that behavior without changing its diagnostics.
    if (result.count == 0 && !values.empty()) return acceptedVariance(values, PixelRejections{}, variances);
    return result;
}

void recordAcceptedGroups(const std::vector<WeightedValue>& values, const PixelRejections& bounds,
                          const StackOptions& options, const std::vector<std::size_t>& inputOrder,
                          StackResult& groups, std::size_t offset) {
    // Mean/std Sigma falls back to the full mean if its bounds reject every
    // sample. Use that same decision for BOTH groups, never a per-group fallback.
    const bool anyAccepted = std::any_of(values.begin(), values.end(), [&](const auto& v) {
        return v.value >= bounds.lowerBound && v.value <= bounds.upperBound;
    });
    std::array<double, 2> sum{}, weight{};
    for (const auto& v : values) {
        if (anyAccepted && (v.value < bounds.lowerBound || v.value > bounds.upperBound)) continue;
        const auto group = options.acceptedGroups[inputOrder[v.frame]];
        sum[group] += double(v.value) * v.weight;
        weight[group] += v.weight;
    }
    for (unsigned group = 0; group < 2; ++group) {
        groups.acceptedGroupImages[group].pixels[offset] = weight[group] > 0 ? float(sum[group] / weight[group]) : 0;
        groups.acceptedGroupWeights[group].pixels[offset] = float(weight[group]);
    }
}

void processRobustRange(const std::vector<float>& tileData, std::size_t frameCount, std::size_t tilePixels,
                       std::uint16_t channels, std::size_t outputPixelOffset, const StackOptions& options,
                       const std::vector<std::size_t>& inputOrder, ImageBuffer& output,
                       StackResult* report, StackResult* counters, ImageBuffer* noise, StackResult* groups,
                       std::size_t begin, std::size_t end, const float* centers) {
    const auto tileSamples = tilePixels * channels;
    std::vector<WeightedValue> values;
    std::vector<WeightedValue> scratch;
    std::vector<WeightedValue> retained;
    std::vector<WeightedValue> halves[2];
    if (noise) for (auto& half : halves) half.reserve((frameCount + 1) / 2);
    std::vector<float> coverages(frameCount, 0.0F);
    std::vector<float> unitSamples;
    values.reserve(frameCount);
    scratch.reserve(frameCount);
    if (options.method == StackMethod::SigmaClip && options.sigma.medianMad) retained.reserve(frameCount);
    const double totalFrameWeight = options.frameWeights.empty() ? static_cast<double>(frameCount)
        : std::accumulate(options.frameWeights.begin(), options.frameWeights.end(), 0.0);

    for (std::size_t pixel = begin; pixel < end; ++pixel) {
        bool noiseValid = noise != nullptr;
        const auto noiseOffset = (outputPixelOffset + pixel) * 4;
        double coverageSum = 0.0;
        for (std::size_t frame = 0; frame < frameCount; ++frame) {
            const auto frameOffset = frame * tileSamples + pixel * channels;
            coverages[frame] = rowPixelCoverage(tileData.data(), channels, frameOffset);
            coverageSum += coverages[frame] * (options.frameWeights.empty() ? 1.0F : options.frameWeights[frame]);
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
                const float weight = coverage * (options.frameWeights.empty() ? 1.0F : options.frameWeights[frame]);
                const float value = tileData[frameOffset + channel];
                if (coverage <= 1.0e-6F || weight <= 0.0F || !std::isfinite(value)) {
                    continue;
                }
                values.push_back({value, weight, frame});
            }
            if (!values.empty()) {
                PixelRejections rejected;
                rejected.report = counters;
                output.pixels[outputOffset + channel] = robustStackValue(values, scratch, retained, unitSamples, options, rejected,
                    centers ? centers + 2 * (pixel * 3 + channel) : nullptr);
                if (groups) recordAcceptedGroups(values, rejected, options, inputOrder, *groups, noiseOffset + channel);
                if (noise) {
                    halves[0].clear(); halves[1].clear();
                    for (const auto& v : values) halves[inputOrder[v.frame] % 2].push_back(v);
                    AcceptedVariance halfVariance[2];
                    double means[2] = {};
                    for (unsigned h = 0; h < 2; ++h) {
                        if (halves[h].empty()) continue;
                        PixelRejections halfBounds;
                        means[h] = robustStackValue(halves[h], scratch, retained, unitSamples, options, halfBounds);
                        halfVariance[h] = acceptedVariance(halves[h], halfBounds, options.frameNoiseVariances);
                    }
                    const double fullVariance = acceptedVariance(values, rejected, options.frameNoiseVariances).variance();
                    const double differenceVariance = halfVariance[0].variance() + halfVariance[1].variance();
                    const double value = differenceVariance > 0
                        ? (means[0] - means[1]) * std::sqrt(fullVariance / differenceVariance) : 0;
                    const bool valid = halfVariance[0].count >= 3 && halfVariance[1].count >= 3 &&
                        fullVariance > 0 && differenceVariance > 0 && std::isfinite(value) &&
                        std::abs(value) <= std::numeric_limits<float>::max();
                    noiseValid &= valid;
                    noise->pixels[noiseOffset + channel] = valid ? float(value) : 0;
                }
                if (report) {
                    report->rejectionLow.pixels[outputOffset + channel] = float(rejected.low) / values.size();
                    report->rejectionHigh.pixels[outputOffset + channel] = float(rejected.high) / values.size();
                }
            } else noiseValid = false;
        }
        if (noise) {
            if (!noiseValid) for (unsigned c = 0; c < 3; ++c) noise->pixels[noiseOffset + c] = 0;
            noise->pixels[noiseOffset + 3] = noiseValid ? 1 : 0;
        }
        if (groups) for (unsigned group = 0; group < 2; ++group) {
            const auto& weights = groups->acceptedGroupWeights[group].pixels;
            const bool valid = weights[noiseOffset] > 0 && weights[noiseOffset + 1] > 0 && weights[noiseOffset + 2] > 0;
            groups->acceptedGroupImages[group].pixels[noiseOffset + 3] = valid ? 1 : 0;
            groups->acceptedGroupWeights[group].pixels[noiseOffset + 3] = valid ? 1 : 0;
        }
        setCoverageAlpha(output, outputPixelOffset + pixel,
                         static_cast<float>(coverageSum / totalFrameWeight));
        if (report) {
            setCoverageAlpha(report->rejectionLow, outputPixelOffset + pixel, coverageSum > 0 ? 1 : 0);
            setCoverageAlpha(report->rejectionHigh, outputPixelOffset + pixel, coverageSum > 0 ? 1 : 0);
        }
    }
}

void processRobustTile(const std::vector<float>& tileData, std::size_t frameCount, std::size_t tilePixels,
                       std::uint16_t channels, std::size_t outputPixelOffset, const StackOptions& options,
                       const std::vector<std::size_t>& inputOrder, ImageBuffer& output,
                       StackResult* report, ImageBuffer* noise, StackResult* groups) {
    constexpr std::size_t automaticWorkerLimit = 8;
    constexpr std::size_t manualWorkerLimit = 16;
    const auto hardware = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    const auto requested = options.combineWorkers == 0
        ? std::min(hardware, automaticWorkerLimit)
        : options.combineWorkers;
    const auto workers = std::max<std::size_t>(1, std::min<std::size_t>({manualWorkerLimit, requested, hardware,
                                                                         tilePixels / 256}));
    std::vector<float> centers;
    if (options.preferMetalCombine) {
#ifdef __APPLE__
        if (options.method == StackMethod::WinsorizedSigmaClip && channels == 4 && frameCount >= 8 &&
            tilePixels >= 256 && !noise && !groups) {
            detail::metalWinsorCenters(tileData, tilePixels, frameCount, options.frameWeights, centers);
            std::vector<float> reduced;
            if (!centers.empty() && detail::metalWinsorReduce(tileData, tilePixels, frameCount, options.frameWeights,
                    centers, options.winsorizedSigma.sigmaLow, options.winsorizedSigma.sigmaHigh, report != nullptr, reduced)) {
                const auto blocks = (tilePixels + 255) / 256;
                if (report) for (std::size_t frame = 0; frame < frameCount; ++frame)
                    for (std::size_t b = 0; b < blocks; ++b) {
                        const auto counters = reduced.data() + 4 * (3 * tilePixels + frame * blocks + b);
                        report->comparedSamples[frame] += std::uint64_t(counters[0]);
                        report->rejectedLowSamples[frame] += std::uint64_t(counters[1]);
                        report->rejectedHighSamples[frame] += std::uint64_t(counters[2]);
                    }
                for (std::size_t pixel = 0; pixel < tilePixels;) {
                    const auto* samples = reduced.data() + 12 * pixel;
                    if (samples[3] < 0) {
                        const auto first = pixel++;
                        while (pixel < tilePixels && reduced[12 * pixel + 3] < 0) ++pixel;
                        processRobustRange(tileData, frameCount, tilePixels, channels, outputPixelOffset, options,
                                           inputOrder, output, report, report, nullptr, nullptr, first, pixel, centers.data());
                        continue;
                    }
                    const auto offset = (outputPixelOffset + pixel) * 4;
                    std::copy_n(samples, 4, output.pixels.data() + offset);
                    if (report) {
                        std::copy_n(samples + 4, 4, report->rejectionLow.pixels.data() + offset);
                        std::copy_n(samples + 8, 4, report->rejectionHigh.pixels.data() + offset);
                    }
                    ++pixel;
                }
                return;
            }
        }
        else detail::recordBackend("cpu", "Metal reduction requires an RGBA Winsor tile with at least 8 frames and no independent noise groups");
#else
        detail::recordBackend("cpu", "Metal unavailable on this platform");
#endif
    }
    const float* bounds = centers.empty() ? nullptr : centers.data();
    if (workers == 1) {
        processRobustRange(tileData, frameCount, tilePixels, channels, outputPixelOffset, options,
                           inputOrder, output, report, report, noise, groups, 0, tilePixels, bounds);
        return;
    }
    // Pixel ranges are disjoint; frame traversal and floating-point reduction
    // order within every pixel stay identical to serial execution. Only integer
    // rejection counters need a merge. Do not copy full diagnostic images.
    std::vector<StackResult> counters(workers);
    std::vector<std::exception_ptr> failures(workers);
    if (report) for (auto& counter : counters) {
        counter.comparedSamples.assign(frameCount, 0);
        counter.rejectedLowSamples.assign(frameCount, 0);
        counter.rejectedHighSamples.assign(frameCount, 0);
    }
    const auto run = [&, ctx = detail::currentStage](std::size_t worker) noexcept {
        const detail::StageScope scope(ctx);
        try {
            processRobustRange(tileData, frameCount, tilePixels, channels, outputPixelOffset, options,
                               inputOrder, output, report, report ? &counters[worker] : nullptr, noise, groups,
                               tilePixels * worker / workers, tilePixels * (worker + 1) / workers, bounds);
        } catch (...) {
            failures[worker] = std::current_exception();
        }
    };
    detail::runWorkers(workers, run);
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    if (report) for (const auto& counter : counters) for (std::size_t frame = 0; frame < frameCount; ++frame) {
        report->comparedSamples[frame] += counter.comparedSamples[frame];
        report->rejectedLowSamples[frame] += counter.rejectedLowSamples[frame];
        report->rejectedHighSamples[frame] += counter.rejectedHighSamples[frame];
    }
}

std::size_t robustTilePixelCount(const ImageBuffer& image, std::size_t frameCount, std::size_t budget) {
    const auto sampleBytesPerPixel = static_cast<std::size_t>(image.channels) * sizeof(float);
    if (sampleBytesPerPixel == 0 || frameCount == 0 ||
        frameCount > std::numeric_limits<std::size_t>::max() / sampleBytesPerPixel) return 0;
    const auto allFramesPixelBytes = sampleBytesPerPixel * frameCount;
    const auto pixelCapacity = std::min(image.pixelCount(), budget / allFramesPixelBytes);
    // Preserve full-row batches when they fit. Very wide/long sequences split
    // rows instead of silently exceeding the shared tile's memory budget.
    return pixelCapacity >= image.width ? (pixelCapacity / image.width) * image.width : pixelCapacity;
}

StackResult stackCachedRobustFrames(const std::vector<std::filesystem::path>& paths,
                                    const std::vector<detail::RegisteredFrame>& registered,
                                    const StackOptions& options, const std::vector<std::size_t>& inputOrder,
                                    ImageBuffer output, StackResult metrics,
                                    StackProgressReporter& progress, double start, double end) {
    const detail::StageScope timer(detail::Stage::Combine);
    const auto frameCount = paths.size();
    const auto tilePixels = robustTilePixelCount(output, frameCount, options.combineTileBytes);
    if (tilePixels == 0)
        return stackError("StackTileBudgetInsufficient", "Robust tile budget must hold at least one pixel from every frame");

    const auto bytesPerFrameTile = tilePixels * output.channels * sizeof(float);
    // Large sequences interleave hundreds of independent reads per tile.
    // Keep several reads in flight, bounded by the same combine worker limit
    // as CPU reduction. Tiny or single-worker jobs retain buffered serial I/O.
    const auto readerCount = frameCount >= 32 && bytesPerFrameTile >= 16384
        ? std::clamp<std::size_t>(options.combineWorkers ? options.combineWorkers : 8, 1, 8) : 1;
    constexpr std::size_t readBufferBudget = 64ULL * 1024ULL * 1024ULL;
    const auto proposedBuffer = std::min<std::size_t>(256 * 1024, readBufferBudget / frameCount) / 4096 * 4096;
    std::size_t readBufferSize = proposedBuffer >= 8192 ? proposedBuffer : 0;
    std::size_t openLimit = 128;
#if defined(__unix__) || defined(__APPLE__)
    struct FileDescriptors {
        std::vector<int> values;
        ~FileDescriptors() { for (const int fd : values) if (fd >= 0) ::close(fd); }
    } descriptors;
    const auto frameBytes = output.sampleCount() * sizeof(float);
    std::size_t blockBytes = 0;
    const bool directReads = bytesPerFrameTile >= 16384;
    if (directReads) {
        descriptors.values.resize(frameCount, -1);
        readBufferSize = 0;
        if (frameCount >= 64 && frameBytes > bytesPerFrameTile) {
            // Interleaving hundreds of short sequential streams can trigger
            // multi-terabyte speculative disk traffic on a single stack.
            // Read adjacent tiles explicitly, sized to current available RAM.
            const auto budget = detail::workingMemoryBudget() / 2;
            const auto perFrame = std::min<std::size_t>({2U << 20, budget / frameCount, frameBytes});
            if (perFrame / bytesPerFrameTile >= 2)
                blockBytes = (perFrame / bytesPerFrameTile) * bytesPerFrameTile;
            readBufferSize = blockBytes;
        }
    }
    const auto openDirect = [&](std::size_t frame) {
        const int fd = ::open(paths[frame].c_str(), O_RDONLY | O_CLOEXEC);
#ifdef __APPLE__
        if (fd >= 0 && frameCount >= 64) ::fcntl(fd, F_RDAHEAD, 0);
#endif
        return fd;
    };
    struct rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0)
        openLimit = std::min<std::uint64_t>(frameCount, limit.rlim_cur > 64 ? limit.rlim_cur - 64 : limit.rlim_cur / 2);
#endif
    std::vector<char> readBuffers;
    try {
        readBuffers.resize(readBufferSize * frameCount);
    } catch (const std::bad_alloc&) {
        // Continue with direct reads/default stream buffers under pressure.
        readBufferSize = 0;
#if defined(__unix__) || defined(__APPLE__)
        blockBytes = 0;
#endif
    }
    std::vector<std::ifstream> streams(frameCount);
    for (std::size_t frame = 0; frame < std::min(frameCount, openLimit); ++frame) {
#if defined(__unix__) || defined(__APPLE__)
        if (directReads) {
            descriptors.values[frame] = openDirect(frame);
            if (descriptors.values[frame] >= 0) continue;
            if (errno == EMFILE) break;
            return stackError("TemporaryStorageReadFailed", "Unable to open stack cache file: " + paths[frame].string());
        }
#endif
        if (readBufferSize) streams[frame].rdbuf()->pubsetbuf(readBuffers.data() + frame * readBufferSize,
                                                               static_cast<std::streamsize>(readBufferSize));
        streams[frame].open(paths[frame], std::ios::binary);
        if (!streams[frame]) return stackError("TemporaryStorageReadFailed",
                                               "Unable to open stack cache file: " + paths[frame].string());
    }
    const auto readFrameTile = [&](std::size_t frame, std::size_t offset, char* destination, std::size_t bytes) {
#if defined(__unix__) || defined(__APPLE__)
        if (directReads) {
            const auto inBlock = blockBytes ? offset % blockBytes : 0;
            char* buffer = blockBytes ? readBuffers.data() + frame * blockBytes : destination;
            if (inBlock) {
                std::memcpy(destination, buffer + inBlock, bytes);
                return true;
            }
            const auto readBytes = blockBytes ? std::min(blockBytes, frameBytes - offset) : bytes;
            const bool transient = descriptors.values[frame] < 0;
            const int fd = transient ? openDirect(frame) : descriptors.values[frame];
            if (fd < 0) return false;
            std::size_t done = 0;
            while (done < readBytes) {
                const auto n = ::pread(fd, buffer + done, readBytes - done, static_cast<off_t>(offset + done));
                if (n > 0) done += static_cast<std::size_t>(n);
                else if (n != -1 || errno != EINTR) break;
            }
            if (transient) ::close(fd);
            if (done != readBytes) return false;
            if (blockBytes) std::memcpy(destination, buffer, bytes);
            return true;
        }
#endif
        auto& stream = streams[frame];
        if (stream.is_open()) return bool(stream.read(destination, static_cast<std::streamsize>(bytes)));
        std::ifstream overflow(paths[frame], std::ios::binary);
        return bool(overflow.seekg(static_cast<std::streamoff>(offset)).read(destination,
                     static_cast<std::streamsize>(bytes)));
    };

    std::vector<float> tileData;
    if (!options.acceptedGroups.empty()) {
        if (output.colorEncoding != ColorEncoding::Linear || (output.channels != 3 && output.channels != 4))
            return stackError("ArgumentInvalid", "Accepted groups require linear RGB inputs");
        for (unsigned group = 0; group < 2; ++group) {
            auto image = createOutputLike(output);
            image.channels = 4;
            image.format = PixelFormat::Float32RGBA;
            image.pixels.assign(image.sampleCount(), 0);
            metrics.acceptedGroupImages[group] = image;
            metrics.acceptedGroupWeights[group] = std::move(image);
        }
    }
    if (options.collectNoiseDiagnostic) {
        if (output.colorEncoding != ColorEncoding::Linear || (output.channels != 3 && output.channels != 4))
            return stackError("ArgumentInvalid", "Noise diagnostics require linear RGB inputs");
        metrics.noiseDiagnostic = createOutputLike(output);
        metrics.noiseDiagnostic.channels = 4;
        metrics.noiseDiagnostic.format = PixelFormat::Float32RGBA;
        metrics.noiseDiagnostic.pixels.assign(metrics.noiseDiagnostic.sampleCount(), 0);
    }
    if (options.collectRejectionMaps) {
        metrics.rejectionLow = output;
        metrics.rejectionHigh = output;
        metrics.rejectedLowSamples.assign(frameCount, 0);
        metrics.rejectedHighSamples.assign(frameCount, 0);
        metrics.comparedSamples.assign(frameCount, 0);
    }
    progress.report(StackProgressStage::Combining, start, 0, 0, output.height);

    for (std::size_t startPixel = 0; startPixel < output.pixelCount(); startPixel += tilePixels) {
        const auto pixelCount = std::min(tilePixels, output.pixelCount() - startPixel);
        const auto tileSamples = pixelCount * output.channels;
        tileData.resize(tileSamples * frameCount);
        const auto byteCount = tileSamples * sizeof(float);

        std::array<std::size_t, 8> errors;
        errors.fill(frameCount);
        detail::runWorkers(readerCount, [&](std::size_t worker) {
            for (std::size_t frame = frameCount * worker / readerCount;
                 frame < frameCount * (worker + 1) / readerCount; ++frame) {
                auto* samples = tileData.data() + frame * tileSamples;
                if (!readFrameTile(frame, startPixel * output.channels * sizeof(float),
                                   reinterpret_cast<char*>(samples), byteCount)) {
                    errors[worker] = frame;
                    break;
                }
                if (options.normalizeBackground && !registered.empty() && frame != 0) {
                    std::array<float, 3> delta{};
                    for (unsigned c = 0; c < 3; ++c) delta[c] = registered[0].background[c] - registered[frame].background[c];
                    for (std::size_t offset = 0; offset < tileSamples; offset += 4) {
                        if (rowPixelCoverage(samples, 4, offset) <= 0) continue;
                        for (unsigned c = 0; c < 3; ++c) samples[offset + c] += delta[c];
                    }
                }
            }
        });
        const auto failed = *std::min_element(errors.begin(), errors.end());
        if (failed < frameCount)
            return stackError("TemporaryStorageReadFailed", "Unable to read stack cache file: " + paths[failed].string());

        processRobustTile(tileData, frameCount, pixelCount, output.channels,
                          startPixel, options, inputOrder, output,
                          options.collectRejectionMaps ? &metrics : nullptr,
                          options.collectNoiseDiagnostic ? &metrics.noiseDiagnostic : nullptr,
                          options.acceptedGroups.empty() ? nullptr : &metrics);
        const auto completedRows = static_cast<std::uint32_t>((startPixel + pixelCount) / output.width);
        const auto previousRows = static_cast<std::uint32_t>(startPixel / output.width);
        const auto interval = std::max<std::uint32_t>(1, output.height / 100);
        if (completedRows == output.height || completedRows / interval != previousRows / interval) {
            const double fraction = static_cast<double>(completedRows) / output.height;
            progress.report(StackProgressStage::Combining, start + (end - start) * fraction,
                            0, completedRows, output.height);
        }
    }

    progress.report(StackProgressStage::Finalizing, end);
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
    const bool externalReference = !options.alignmentReference.empty();
    if (!externalReference && options.alignDistortion && inputOrder.size() > 2) {
        std::swap(inputOrder.front(), inputOrder[inputOrder.size() / 2]);
    }

    const ImageCodec codec;
    ImageReadOptions readOptions;
    readOptions.raw = options.raw;
    readOptions.fits.mode = options.fitsDecodeMode;
    readOptions.fits.maskNonFinitePixels = true;
    readOptions.fits.debayer = options.debayerFits;
    readOptions.fits.demosaic = options.fitsDemosaic;
    readOptions.fits.cfaInterpolationGains = options.cfaInterpolationGains;
    const auto referenceInputIndex = inputOrder.front();
    const auto& referencePath = externalReference ? options.alignmentReference : inputs[referenceInputIndex];
    progress.report(StackProgressStage::Reading, 0.0, 1);
    auto referenceResult = codec.read(referencePath, readOptions);
    if (!referenceResult.ok) {
        return stackError(referenceResult.errorCode,
                          "Failed to read " + referencePath.string() + ": " + referenceResult.message);
    }
    if (referenceResult.image.empty()) {
        return stackError("ImageBufferInvalid", "Decoded image is empty: " + referencePath.string());
    }
    const double referenceReadEnd = externalReference ? 0.0 : frameSpan * 0.08;
    progress.report(StackProgressStage::Reading, referenceReadEnd, 1);
    if (inputs.size() == 1 && !externalReference) {
        auto result = normalizeSingleFrameCoverage(std::move(referenceResult.image), progress, referenceReadEnd, 1.0);
        if (result.ok) {
            const auto released = notifyInputConsumed(options, 0);
            if (!released.ok) return released;
        }
        return result;
    }

    if (robustTilePixelCount(referenceResult.image, inputs.size(), options.combineTileBytes) == 0)
        return stackError("StackTileBudgetInsufficient", "Robust tile budget must hold at least one pixel from every frame");

    TemporaryFrameCache cache;
    const auto cacheResult = createTemporaryFrameCache(cache, options.temporaryDirectory);
    if (!cacheResult.ok) {
        return cacheResult;
    }

    const bool requiresAlignment = options.alignDistortion || options.alignAffine || options.alignSimilarity ||
                                   options.alignTranslation;
    ImageBuffer reference = std::move(referenceResult.image);
    const auto referenceBackground =
        options.normalizeBackground ? sampledBackground(reference) : std::array<float, 3>{};
    const auto referenceWidth = reference.width;
    const auto referenceHeight = reference.height;
    const auto referenceChannels = reference.channels;
    const auto referenceColorEncoding = reference.colorEncoding;
    const auto frameBytes = reference.pixels.size() * sizeof(float);
    const auto storageResult = validateTemporaryStorage(cache.directory, frameBytes,
                                                        options.inputConsumed ? 1 : inputs.size());
    if (!storageResult.ok) {
        return storageResult;
    }
    if (!externalReference) {
        const auto referenceWrite =
            writeCachedFrame(cache.framePath(0), reference, &progress, 1, referenceReadEnd, frameSpan);
        if (!referenceWrite.ok) {
            return referenceWrite;
        }
        const auto released = notifyInputConsumed(options, referenceInputIndex);
        if (!released.ok) return released;
    }

    StackResult metrics;
    metrics.ok = true;
    if (!requiresAlignment) {
        std::vector<float>().swap(reference.pixels);
    }

    for (std::size_t position = externalReference ? 0 : 1; position < inputOrder.size(); ++position) {
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
        if (options.normalizeBackground)
            matchBackground(readResult.image, referenceBackground);

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
        if (options.inputConsumed) {
            const auto storage = validateTemporaryStorage(cache.directory, frameBytes, 1);
            if (!storage.ok) return storage;
        }
        const auto writeResult = requiresAlignment
                                     ? writeCachedAlignedFrame(cache.framePath(position), readResult.image,
                                                               alignmentPlan, nullptr, &progress, frameNumber,
                                                               cacheStart, frameEnd)
                                     : writeCachedFrame(cache.framePath(position), readResult.image, &progress,
                                                        frameNumber, cacheStart, frameEnd);
        if (!writeResult.ok) {
            return writeResult;
        }

        const auto released = notifyInputConsumed(options, inputIndex);
        if (!released.ok) return released;

        metrics.alignedFrames += alignment.alignedFrames;
        metrics.alignmentFallbacks += alignment.alignmentFallbacks;
        metrics.centroidRefinedFrames += alignment.centroidRefinedFrames;
        metrics.centroidRefinementFallbacks += alignment.centroidRefinementFallbacks;
        metrics.centroidAffineFrames += alignment.centroidAffineFrames;
        metrics.centroidSimilarityRetainedFrames += alignment.centroidSimilarityRetainedFrames;
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
    StackOptions combineOptions = options;
    if (!options.frameWeights.empty())
        for (std::size_t position=0;position<inputOrder.size();++position)
            combineOptions.frameWeights[position] = options.frameWeights[inputOrder[position]];
    if (options.collectNoiseDiagnostic)
        for (std::size_t position=0;position<inputOrder.size();++position)
            combineOptions.frameNoiseVariances[position] = options.frameNoiseVariances[inputOrder[position]];
    std::vector<std::filesystem::path> paths;
    for (std::size_t i = 0; i < inputs.size(); ++i) paths.push_back(cache.framePath(i));
    auto result = stackCachedRobustFrames(paths, {}, combineOptions, inputOrder, std::move(output), std::move(metrics), progress,
                                         framePhaseEnd, 1.0);
    if (result.ok && options.collectRejectionMaps) {
        const auto low = result.rejectedLowSamples, high = result.rejectedHighSamples, count = result.comparedSamples;
        for (std::size_t position = 0; position < inputOrder.size(); ++position) {
            result.rejectedLowSamples[inputOrder[position]] = low[position];
            result.rejectedHighSamples[inputOrder[position]] = high[position];
            result.comparedSamples[inputOrder[position]] = count[position];
        }
    }
    return result;
}

} // namespace

static StackResult stackInputs(const std::vector<std::filesystem::path>& inputs, const StackOptions& requestedOptions,
                               const std::vector<detail::RegisteredFrame>& registered) {
    StackOptions options = requestedOptions;
    if (!options.acceptedGroups.empty()) {
        if (options.method != StackMethod::SigmaClip || options.fitsDecodeMode != FitsDecodeMode::Scientific ||
            options.acceptedGroups.size() != inputs.size() ||
            std::any_of(options.acceptedGroups.begin(), options.acceptedGroups.end(), [](auto g) { return g > 1; }) ||
            std::find(options.acceptedGroups.begin(), options.acceptedGroups.end(), 0) == options.acceptedGroups.end() ||
            std::find(options.acceptedGroups.begin(), options.acceptedGroups.end(), 1) == options.acceptedGroups.end())
            return stackError("ArgumentInvalid", "Accepted groups require scientific Sigma stacking and original-input labels containing both 0 and 1");
        std::set<std::filesystem::path> distinct;
        for (const auto& path : inputs) {
            std::error_code error;
            const auto canonical = std::filesystem::weakly_canonical(path, error);
            if (error || !distinct.insert(canonical).second)
                return stackError("ArgumentInvalid", "Accepted groups require distinct input paths");
        }
    }
    if (options.inputConsumed) {
        std::set<std::filesystem::path> distinct;
        for (const auto& path : inputs) {
            std::error_code error;
            const auto canonical = std::filesystem::weakly_canonical(path, error);
            if (error || !distinct.insert(canonical).second)
                return stackError("ArgumentInvalid", "Input consumption notifications require distinct input paths");
        }
    }
    if (options.collectNoiseDiagnostic) {
        if (inputs.size() < 6 || options.method != StackMethod::SigmaClip ||
            options.fitsDecodeMode != FitsDecodeMode::Scientific ||
            options.frameNoiseVariances.size() != inputs.size() ||
            std::any_of(options.frameNoiseVariances.begin(), options.frameNoiseVariances.end(),
                [](double v) { return !std::isfinite(v) || v <= 0; }))
            return stackError("ArgumentInvalid", "Noise diagnostics require sigma stacking, six inputs, scientific decoding and one positive variance per input");
        std::set<std::filesystem::path> distinct;
        for (const auto& path : inputs) {
            std::error_code error;
            const auto canonical = std::filesystem::weakly_canonical(path, error);
            if (error || !distinct.insert(canonical).second)
                return stackError("ArgumentInvalid", "Noise diagnostics require distinct input paths");
        }
        const double maximum = *std::max_element(options.frameNoiseVariances.begin(), options.frameNoiseVariances.end());
        for (auto& v : options.frameNoiseVariances) {
            v /= maximum;
            if (v == 0) return stackError("ArgumentInvalid", "Noise variance dynamic range is too large");
        }
    } else if (!options.frameNoiseVariances.empty()) {
        return stackError("ArgumentInvalid", "Frame noise variances require noise diagnostics");
    }
    if (options.collectRejectionMaps && (inputs.size() < 2 ||
        options.method == StackMethod::Average || options.method == StackMethod::WeightedAverage ||
        isExtremaMethod(options.method))) {
        return stackError("ArgumentInvalid", "Rejection maps require at least two inputs and a robust stack method");
    }
    if (options.sigma.medianMad && options.method != StackMethod::SigmaClip) {
        return stackError("ArgumentInvalid", "Median-MAD rejection requires the sigma stack method");
    }
    if (options.registration.refineSimilarityCentroids &&
        (options.alignDistortion || !(options.alignSimilarity || options.alignAffine))) {
        return stackError("ArgumentInvalid", "Centroid refinement requires similarity or affine alignment");
    }
    if (!options.alignmentReference.empty() &&
        !(options.alignTranslation || options.alignSimilarity || options.alignAffine || options.alignDistortion)) {
        return stackError("ArgumentInvalid", "An external alignment reference requires alignment");
    }
    if (!options.frameWeights.empty()) {
        if (isExtremaMethod(options.method))
            return stackError("ArgumentInvalid", "Maximum/minimum stacking does not accept frame weights");
        if (options.frameWeights.size() != inputs.size() ||
            std::any_of(options.frameWeights.begin(),options.frameWeights.end(),[](float w){return !std::isfinite(w) || w<=0;}))
            return stackError("ArgumentInvalid", "Frame weights must contain one finite positive value per input");
        const float maximum = *std::max_element(options.frameWeights.begin(),options.frameWeights.end());
        for (auto& w : options.frameWeights) {
            w /= maximum;
            if (w == 0) return stackError("ArgumentInvalid", "Frame weight dynamic range is too large");
        }
    }
    if (!isValidRawDecodeOptions(options.raw)) {
        return stackError("ArgumentInvalid", "Unsupported RAW decode options");
    }
    if (options.fitsDecodeMode != FitsDecodeMode::DisplayNormalized &&
        options.fitsDecodeMode != FitsDecodeMode::Scientific) {
        return stackError("ArgumentInvalid", "Unsupported FITS decode mode");
    }
    if (!registered.empty()) {
        if (registered.size() != inputs.size()) return stackError("TemporaryStorageInvalid", "Missing registered frame metadata");
        auto validation = validateRobustOptions(options);
        if (!validation.ok) return validation;
        const auto& shape = registered.front();
        ImageBuffer output;
        output.width = shape.width; output.height = shape.height; output.channels = 4;
        output.format = PixelFormat::Float32RGBA; output.colorEncoding = ColorEncoding::Linear;
        output.sourceBitsPerChannel = 32;
        for (const auto& frame : registered)
            if (frame.width != shape.width || frame.height != shape.height)
                return stackError("ImageSizeMismatch", "Registered frames have different geometry");
        output.pixels.resize(output.sampleCount());
        if (inputs.size() == 1) {
            std::ifstream frame(inputs.front(), std::ios::binary);
            const auto bytes = static_cast<std::streamsize>(output.sampleCount() * sizeof(float));
            if (!frame.read(reinterpret_cast<char*>(output.pixels.data()), bytes))
                return stackError("TemporaryStorageReadFailed", "Unable to read the registered frame");
            StackProgressReporter progress(options, inputs.size());
            return normalizeSingleFrameCoverage(std::move(output), progress, 0, 1);
        }
        std::vector<std::size_t> order(inputs.size());
        std::iota(order.begin(), order.end(), 0);
        StackProgressReporter progress(options, inputs.size());
        return stackCachedRobustFrames(inputs, registered, options, order, std::move(output), {}, progress, 0, 1);
    }
    switch (options.method) {
    case StackMethod::Average:
    case StackMethod::Maximum:
    case StackMethod::Minimum:
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

StackResult Stacker::stack(const std::vector<std::filesystem::path>& inputs, const StackOptions& options) const {
    return stackInputs(inputs, options, {});
}

StackResult detail::writeRegisteredFrame(const std::filesystem::path& path, const ImageBuffer& image,
                                          bool normalize, RegisteredFrame& metadata) {
    if (image.channels != 4 || image.colorEncoding != ColorEncoding::Linear || image.pixels.size() != image.sampleCount())
        return stackError("TemporaryStorageInvalid", "Expected registered linear RGBA samples");
    metadata = {image.width, image.height, normalize ? sampledBackground(image) : std::array<float,3>{}};
    return writeCachedFrame(path, image);
}

StackResult detail::combineRegisteredFrames(const std::vector<std::filesystem::path>& inputs,
                                             const std::vector<RegisteredFrame>& frames, const StackOptions& options) {
    return stackInputs(inputs, options, frames);
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
