#pragma once
#include "photonstack/SkyProjection.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace photonstack {
struct DeepSkyObject {
    std::string id, label, type, commonNames, aliases, magnitudeBand;
    bool invalidPositionAngle = false;
    double ra = 0, dec = 0, majorArcmin = 0, minorArcmin = 0;
    std::optional<double> positionAngle, magnitude;
    std::vector<std::vector<SkyPoint>> outlines; // published RA/Dec degrees
};
std::vector<DeepSkyObject> readOpenNGC(const std::filesystem::path& directory);
struct AnnotationOptions {
    std::string category = "all", query;
    unsigned maxObjects = 200;
    bool includeUnknownMagnitude = true;
    double limitingMagnitude = 18;
};
struct SkyAnnotationObject {
    DeepSkyObject object;
    SkyPoint center;
    bool centerVisible = false;
    std::string outlineKind = "none";
    std::vector<std::vector<SkyPoint>> paths; // clipped open strokes, zero-based pixels
};
struct SkyAnnotationResult {
    std::vector<SkyAnnotationObject> objects;
    std::size_t matched = 0; // before display limit; deterministic catalog ordering
};
SkyAnnotationResult annotateSky(const SolvedPanel& sky, unsigned width, unsigned height,
                                const std::vector<DeepSkyObject>& catalog, const AnnotationOptions& options = {});
// Exact spherical angular offsets; major axis position angle is east of north.
std::vector<SkyPoint> celestialEllipse(const DeepSkyObject& object, unsigned samples = 128);
std::string deepSkyCategory(const std::string& type);
} // namespace photonstack
