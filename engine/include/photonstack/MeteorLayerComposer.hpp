#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "photonstack/ArtifactTrailRemover.hpp"
#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct MeteorLayerOptions {
    ArtifactTrailOptions detection;
    float minConfidence = 0.35F;
    float maskRadius = 3.0F;
    float featherRadius = 6.0F;
    float opacity = 1.0F;
};

struct MeteorLayerResult {
    bool ok = false;
    ImageBuffer image;
    ImageBuffer layer;
    std::vector<ArtifactTrail> meteors;
    std::size_t restoredMeteors = 0;
    std::string errorCode;
    std::string message;
};

class MeteorLayerComposer {
  public:
    MeteorLayerResult extract(const ImageBuffer& source, const MeteorLayerOptions& options = {}) const;
    MeteorLayerResult extractUsingDetectionImage(
        const ImageBuffer& source,
        const ImageBuffer& detectionImage,
        const MeteorLayerOptions& options = {}
    ) const;
    MeteorLayerResult compose(const ImageBuffer& base, const ImageBuffer& layer, const MeteorLayerOptions& options = {}) const;
    MeteorLayerResult restore(const ImageBuffer& base, const ImageBuffer& source, const MeteorLayerOptions& options = {}) const;
    MeteorLayerResult restoreUsingDetectionImage(
        const ImageBuffer& base,
        const ImageBuffer& source,
        const ImageBuffer& detectionImage,
        const MeteorLayerOptions& options = {}
    ) const;
};

} // namespace photonstack
