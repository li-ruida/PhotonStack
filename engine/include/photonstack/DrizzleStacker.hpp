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

enum class DrizzleAlignment {
    None,
    Translation,
    Similarity,
    Affine,
    Distortion,
};

enum class DrizzleProgressStage {
    Reading,
    Aligning,
    Accumulating,
    Normalizing,
};

struct DrizzleProgress {
    DrizzleProgressStage stage = DrizzleProgressStage::Reading;
    double progress = 0.0;
    std::size_t frame = 0;
    std::size_t frameCount = 0;
    std::uint32_t row = 0;
    std::uint32_t rowCount = 0;
};

using DrizzleProgressCallback = std::function<void(const DrizzleProgress&)>;

struct DrizzleOptions {
    std::uint32_t scale = 2;
    float pixfrac = 1.0F;
    DrizzleAlignment alignment = DrizzleAlignment::Translation;
    FitsDecodeMode fitsDecodeMode = FitsDecodeMode::Scientific;
    RegistrationOptions registration;
    DrizzleProgressCallback progress;
};

struct DrizzleResult {
    bool ok = false;
    ImageBuffer image;
    std::size_t alignedFrames = 0;
    std::size_t alignmentFallbacks = 0;
    std::size_t minimumAlignmentMatches = 0;
    std::string errorCode;
    std::string message;
};

class DrizzleStacker {
  public:
    DrizzleResult drizzle(const std::vector<std::filesystem::path>& inputs, const DrizzleOptions& options = {}) const;
};

} // namespace photonstack
