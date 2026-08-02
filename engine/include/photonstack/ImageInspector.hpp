#pragma once

#include <filesystem>

#include "photonstack/ImageMetadata.hpp"

namespace photonstack {

class ImageInspector {
  public:
    InspectResult inspect(const std::filesystem::path& path) const;
};

} // namespace photonstack
