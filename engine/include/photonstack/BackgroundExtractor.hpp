#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

enum class BackgroundMode {
    Subtract,
    Divide,
};

struct BackgroundExtractionOptions {
    BackgroundMode mode = BackgroundMode::Subtract;
    float strength = 1.0F;
    float epsilon = 1.0e-6F;
    bool preserveBrightness = true;
    bool clampOutput = true;
};

struct BackgroundGridOptions {
    BackgroundExtractionOptions extraction;
    std::uint32_t columns = 6;
    std::uint32_t rows = 4;
    bool protectBrightTargets = true;
};

struct BackgroundExtractionResult {
    bool ok = false;
    ImageBuffer image;
    float background = 0.0F;
    std::vector<float> backgroundGrid;
    std::uint32_t gridColumns = 0;
    std::uint32_t gridRows = 0;
    std::uint32_t sampledGridCells = 0;
    std::uint32_t filledGridCells = 0;
    std::string errorCode;
    std::string message;
};

class BackgroundExtractor {
  public:
    BackgroundExtractionResult extractGlobal(const ImageBuffer& image,
                                             const BackgroundExtractionOptions& options) const;
    BackgroundExtractionResult extractGrid(const ImageBuffer& image, const BackgroundGridOptions& options) const;
};

} // namespace photonstack
