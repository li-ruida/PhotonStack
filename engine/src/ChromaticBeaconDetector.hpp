#pragma once

#include "photonstack/ArtifactTrailRemover.hpp"

namespace photonstack::detail {
// Local chromatic contrast, not the global sky/foreground brightness, supplies
// the evidence. Returned paths are measured in original image coordinates.
std::vector<ArtifactTrail> detectChromaticBeaconChains(const ImageBuffer& image);
bool sameBeaconChain(const ArtifactTrail& a, const ArtifactTrail& b);
void repairChromaticBeaconChains(const ImageBuffer& source, ImageBuffer& output,
                                 const std::vector<ArtifactTrail>& trails,
                                 const std::function<void(double)>& progress = {});
} // namespace photonstack::detail
