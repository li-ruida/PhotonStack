#pragma once

#include <filesystem>

#include "photonstack/ImageCodec.hpp"
#include "photonstack/ImageMetadata.hpp"

namespace photonstack {

class FitsCodec {
  public:
    InspectResult inspect(const std::filesystem::path& path) const;
    ImageReadResult read(const std::filesystem::path& path) const;
    ImageReadResult read(const std::filesystem::path& path, const FitsDecodeOptions& options) const;
    ImageWriteResult write(const ImageBuffer& image, const std::filesystem::path& path) const;
};

bool isFitsPath(const std::filesystem::path& path);

} // namespace photonstack
