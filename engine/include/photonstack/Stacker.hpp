#pragma once

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
    std::size_t alignedFrames = 0;
    std::size_t alignmentFallbacks = 0;
    std::size_t minimumAlignmentMatches = 0;
    std::string errorCode;
    std::string message;
};

struct SigmaClipOptions {
    float sigmaLow = 2.0F;
    float sigmaHigh = 2.0F;
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
    RegistrationOptions registration;
    StackProgressCallback progress;
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
