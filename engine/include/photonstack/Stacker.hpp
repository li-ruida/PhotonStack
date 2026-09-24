#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"
#include "photonstack/ImageCodec.hpp"
#include "photonstack/Registration.hpp"

namespace photonstack {

struct StackResult {
    bool ok = false;
    ImageBuffer image;
    // Optional per-channel fractions of covered input samples rejected low/high.
    // Winsorized mode counts bounded samples; median/average do not reject samples.
    ImageBuffer rejectionLow;
    ImageBuffer rejectionHigh;
    // Optional alternating-input half-difference in master units, scaled by
    // accepted weighted variance moments. RGBA: alpha 1 means >=3 accepted
    // samples in each half for every channel; alpha 0 means unavailable.
    // A diagnostic approximation after clipping, NOT certified signal-free
    // noise: differential PSFs/sky and shared systematics require validation.
    ImageBuffer noiseDiagnostic;
    // Optional two means of the SAME samples accepted by the joint Sigma stack.
    // RGB weights are sums of normalized frame weight * coverage per channel.
    // Missing channels have zero mean/weight; alpha is 1 only if all RGB weights
    // are positive. Pool per channel with these weights, not input frame counts.
    // These are dependent, spatially varying estimates, not independent noise
    // references or automatically valid stationary-PSF frequency inputs.
    std::array<ImageBuffer, 2> acceptedGroupImages;
    std::array<ImageBuffer, 2> acceptedGroupWeights;
    std::vector<std::uint64_t> rejectedLowSamples;
    std::vector<std::uint64_t> rejectedHighSamples;
    std::vector<std::uint64_t> comparedSamples;
    std::size_t alignedFrames = 0;
    std::size_t alignmentFallbacks = 0;
    std::size_t centroidRefinedFrames = 0;
    std::size_t centroidRefinementFallbacks = 0;
    std::size_t centroidAffineFrames = 0;
    std::size_t centroidSimilarityRetainedFrames = 0;
    std::size_t minimumAlignmentMatches = 0;
    std::string errorCode;
    std::string message;
};

struct SigmaClipOptions {
    float sigmaLow = 2.0F;
    float sigmaHigh = 2.0F;
    bool medianMad = false;
    std::uint32_t iterations = 3;
};

struct WinsorizedSigmaClipOptions {
    float sigmaLow = 2.0F;
    float sigmaHigh = 2.0F;
};

struct PercentileClipOptions {
    float low = 0.1F;
    float high = 0.9F;
};

enum class StackMethod {
    Average,
    WeightedAverage,
    Median,
    SigmaClip,
    WinsorizedSigmaClip,
    PercentileClip,
};

enum class StackProgressStage {
    Reading,
    Aligning,
    Caching,
    Accumulating,
    Combining,
    Finalizing,
};

struct StackProgress {
    StackProgressStage stage = StackProgressStage::Reading;
    double progress = 0.0;
    std::size_t frame = 0;
    std::size_t frameCount = 0;
    std::uint32_t row = 0;
    std::uint32_t rowCount = 0;
};

using StackProgressCallback = std::function<void(const StackProgress&)>;

struct StackOptions {
    StackMethod method = StackMethod::Average;
    SigmaClipOptions sigma;
    WinsorizedSigmaClipOptions winsorizedSigma;
    PercentileClipOptions percentile;
    bool alignTranslation = false;
    bool alignSimilarity = false;
    bool alignAffine = false;
    bool alignDistortion = false;
    RawDecodeOptions raw;
    FitsDecodeMode fitsDecodeMode = FitsDecodeMode::Scientific;
    bool debayerFits = true;
    FitsDemosaic fitsDemosaic = FitsDemosaic::Bilinear;
    std::array<float, 3> cfaInterpolationGains = {1, 1, 1};
    // Match additive sky offsets before clipping; keep stellar flux and ADU range.
    bool normalizeBackground = false;
    RegistrationOptions registration;
    RegistrationInterpolation interpolation = RegistrationInterpolation::Bilinear;
    // Relative exposure weights in input order; empty means equal weights.
    // These multiply coverage (and automatic quality for WeightedAverage).
    std::vector<float> frameWeights;
    // Optional coordinate/background reference; not automatically added as an input.
    // Requires alignment. Input weights still correspond only to inputs.
    std::filesystem::path alignmentReference;
    bool collectRejectionMaps = false;
    // SigmaClip only, >=6 distinct inputs. Does not modify the master/rejection
    // result. Splits by ORIGINAL input index parity, regardless of cache order.
    bool collectNoiseDiagnostic = false;
    // Required with collectNoiseDiagnostic, otherwise must be empty. Relative
    // per-frame noise variances in the same linear units AFTER registration/
    // background matching; explicit all-ones asserts equal variance. No automatic
    // exposure/weight inference. One finite positive scalar per original input.
    std::vector<double> frameNoiseVariances;
    // Optional original-input-order labels 0/1; empty disables group output.
    // Requires both labels, distinct inputs, SigmaClip and scientific linear RGB.
    // Grouping does not alter the joint master or its rejection decisions.
    std::vector<std::uint8_t> acceptedGroups;
    // Robust combine only: 0 selects bounded automatic parallelism, 1 is serial.
    // Values above 8 are capped. Changes scheduling, never sample/reduction order.
    std::size_t combineWorkers = 0;
    // Optional owner-managed parent for transient cache directories.
    std::filesystem::path temporaryDirectory;
    StackProgressCallback progress;
    // Optional synchronous notification after an input is safely accumulated or
    // its complete internal cache is closed. Index is in ORIGINAL input order;
    // the input file will not be read again. The caller may release only files
    // it owns. Stacker itself never deletes inputs. With this hook, robust cache
    // space is checked one frame at a time so released storage can be reused.
    // Duplicate canonical input paths are rejected before any notification.
    // A callback exception fails the operation with InputReleaseFailed; earlier
    // inputs may already have been released. External references are not notified
    // unless also explicitly present among the science inputs.
    std::function<void(std::size_t)> inputConsumed;
};

class Stacker {
  public:
    StackResult stack(const std::vector<std::filesystem::path>& inputs, const StackOptions& options) const;
    StackResult average(const std::vector<std::filesystem::path>& inputs) const;
    StackResult weightedAverage(const std::vector<std::filesystem::path>& inputs) const;
    StackResult median(const std::vector<std::filesystem::path>& inputs) const;
    StackResult sigmaClip(const std::vector<std::filesystem::path>& inputs, const SigmaClipOptions& options) const;
    StackResult winsorizedSigmaClip(const std::vector<std::filesystem::path>& inputs,
                                    const WinsorizedSigmaClipOptions& options) const;
    StackResult percentileClip(const std::vector<std::filesystem::path>& inputs,
                               const PercentileClipOptions& options) const;
};

} // namespace photonstack
