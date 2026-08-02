#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

enum class ArtifactTrailKind {
    Airplane,
    Drone,
    Satellite,
    Meteor,
    Unknown,
};

enum class ArtifactTrailProgressStage {
    Analyzing,
    Components,
    Navigation,
    PatternSearch,
    Refining,
    Masking,
    Inpainting,
    Cleaning,
};

struct ArtifactTrailProgress {
    ArtifactTrailProgressStage stage = ArtifactTrailProgressStage::Analyzing;
    double progress = 0.0;
    std::size_t item = 0;
    std::size_t itemCount = 0;
    std::uint32_t row = 0;
    std::uint32_t rowCount = 0;
};

using ArtifactTrailProgressCallback = std::function<void(const ArtifactTrailProgress&)>;

struct ArtifactTrailPathPoint {
    float x = 0.0F;
    float y = 0.0F;
};

struct ArtifactTrail {
    ArtifactTrailKind kind = ArtifactTrailKind::Unknown;
    float confidence = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    float length = 0.0F;
    float width = 0.0F;
    float angleRadians = 0.0F;
    float peakPosition = 0.5F;
    float taperScore = 0.0F;
    float colorVariance = 0.0F;
    float warmEvidence = 0.0F;
    float meanBrightness = 0.0F;
    float weight = 0.0F;
    std::vector<ArtifactTrailPathPoint> path;
};

struct ArtifactTrailOptions {
    float sigmaThreshold = 2.5F;
    float minPeak = 0.08F;
    float minLength = 12.0F;
    float airplaneLength = 40.0F;
    float maxWidth = 8.0F;
    std::uint32_t maskRadius = 2;
    std::uint32_t inpaintRadius = 4;
    bool preserveMeteors = true;
    bool removeAirplanes = true;
    bool removeDrones = true;
    bool removeSatellites = true;
    bool removeMeteors = false;
    std::vector<std::size_t> selectedIndices;
    std::vector<ArtifactTrail> detectedTrails;
    bool useDetectedTrails = false;
    bool includeMeteors = false;
    ArtifactTrailProgressCallback progress;
};

struct ArtifactTrailResult {
    bool ok = false;
    ImageBuffer image;
    std::vector<ArtifactTrail> trails;
    std::size_t removedTrails = 0;
    std::size_t protectedMeteors = 0;
    std::string errorCode;
    std::string message;
};

class ArtifactTrailRemover {
  public:
    ArtifactTrailResult detect(const ImageBuffer& image, const ArtifactTrailOptions& options = {}) const;
    ArtifactTrailResult remove(const ImageBuffer& image, const ArtifactTrailOptions& options = {}) const;
};

const char* toString(ArtifactTrailKind kind);

} // namespace photonstack
