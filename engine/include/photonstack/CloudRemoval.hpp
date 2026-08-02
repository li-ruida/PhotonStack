#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct CloudMaskRect {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct CloudRegion {
    std::size_t index = 0;
    float confidence = 0.0F;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float coverage = 0.0F;
    float meanLuminance = 0.0F;
    float backgroundLuminance = 0.0F;
    std::size_t temporalSupport = 0;
    std::vector<CloudMaskRect> maskRects;
};

struct CloudRemovalOptions {
    std::uint32_t columns = 32;
    std::uint32_t rows = 20;
    float minBrightnessDelta = 0.045F;
    float minCoverage = 0.006F;
    float strength = 0.65F;
    float featherRadius = 24.0F;
    std::vector<std::size_t> selectedIndices;
};

struct CloudRemovalResult {
    bool ok = false;
    ImageBuffer image;
    std::vector<CloudRegion> regions;
    std::size_t removedRegions = 0;
    std::size_t temporalFramesUsed = 0;
    std::size_t temporallyConfirmedRegions = 0;
    bool usedMajorityFallback = false;
    std::string errorCode;
    std::string message;
};

class CloudRemoval {
  public:
    CloudRemovalResult detect(const ImageBuffer& image, const CloudRemovalOptions& options = {}) const;
    CloudRemovalResult detectTemporal(
        const ImageBuffer& image,
        const std::vector<ImageBuffer>& referenceImages,
        const CloudRemovalOptions& options = {}
    ) const;
    // Accept already-detected weak-reference reports so callers that decode
    // large RAW frames can release each reference image immediately.
    CloudRemovalResult detectTemporalFromDetections(
        const ImageBuffer& image,
        const std::vector<CloudRemovalResult>& referenceDetections,
        const CloudRemovalOptions& options = {}
    ) const;
    CloudRemovalResult removeTemporal(
        const ImageBuffer& image,
        const std::vector<ImageBuffer>& referenceImages,
        const CloudRemovalOptions& options = {}
    ) const;
    CloudRemovalResult removeTemporalFromDetections(
        const ImageBuffer& image,
        const std::vector<CloudRemovalResult>& referenceDetections,
        const CloudRemovalOptions& options = {}
    ) const;
    CloudRemovalResult remove(const ImageBuffer& image, const CloudRemovalOptions& options = {}) const;
};

} // namespace photonstack
