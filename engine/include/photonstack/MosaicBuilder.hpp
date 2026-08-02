#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"
#include "photonstack/ImageCodec.hpp"
#include "photonstack/Registration.hpp"

namespace photonstack {

enum class MosaicProjection {
    Planar,
    Cylindrical,
};

enum class MosaicLayout {
    Horizontal,
    Grid,
};

enum class MosaicAlignment {
    Manual,
    Auto,
};

enum class MosaicBlendMode {
    Average,
    Feather,
    Multiband,
};

enum class MosaicTransformModel {
    Manual,
    Translation,
    Similarity,
    Affine,
};

enum class MosaicProgressStage {
    Reading,
    Resizing,
    Projecting,
    Aligning,
    ExposureMatching,
    Blending,
    Normalizing,
};

struct MosaicProgress {
    MosaicProgressStage stage = MosaicProgressStage::Reading;
    double progress = 0.0;
    std::size_t panel = 0;
    std::size_t panelCount = 0;
    std::uint32_t row = 0;
    std::uint32_t rowCount = 0;
    std::size_t level = 0;
    std::size_t levelCount = 0;
};

using MosaicProgressCallback = std::function<void(const MosaicProgress&)>;

struct MosaicPlacement {
    float x = 0.0F;
    float y = 0.0F;
    float a = 1.0F;
    float b = 0.0F;
    float c = 0.0F;
    float d = 1.0F;
    float dx = 0.0F;
    float dy = 0.0F;
    std::size_t matches = 0;
    std::size_t referenceCount = 0;
    MosaicTransformModel transformModel = MosaicTransformModel::Manual;
    bool autoAligned = false;
    bool usedFallback = false;
    bool usedReducedModel = false;
    bool usedCoarseAlignment = false;
};

struct MosaicOptions {
    std::uint32_t overlapPixels = 0;
    MosaicProjection projection = MosaicProjection::Planar;
    MosaicLayout layout = MosaicLayout::Horizontal;
    MosaicAlignment alignment = MosaicAlignment::Manual;
    MosaicBlendMode blendMode = MosaicBlendMode::Feather;
    bool exposureMatching = true;
    std::uint32_t columns = 1;
    std::uint32_t previewWidth = 0;
    std::uint32_t alignmentWidth = 2000;
    FitsDecodeMode fitsDecodeMode = FitsDecodeMode::Scientific;
    RegistrationOptions registration;
    MosaicProgressCallback progress;
};

struct MosaicResult {
    bool ok = false;
    ImageBuffer image;
    std::vector<MosaicPlacement> placements;
    bool usedAutoAlignment = false;
    std::size_t matchedPairs = 0;
    std::size_t fallbackPanels = 0;
    bool exposureMatched = false;
    MosaicBlendMode blendMode = MosaicBlendMode::Feather;
    std::string errorCode;
    std::string message;
};

class MosaicBuilder {
  public:
    MosaicResult stitchHorizontal(const std::vector<std::filesystem::path>& inputs,
                                  const MosaicOptions& options = {}) const;
    MosaicResult stitchHorizontal(const std::vector<ImageBuffer>& images, const MosaicOptions& options = {}) const;
};

} // namespace photonstack
