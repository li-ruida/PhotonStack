#pragma once

#include <filesystem>

#include "photonstack/ImageCodec.hpp"
#include "photonstack/ImageMetadata.hpp"

namespace photonstack {

class RawCodec {
  public:
    InspectResult inspect(const std::filesystem::path& path) const;
    ImageReadResult read(const std::filesystem::path& path) const;
    ImageReadResult read(const std::filesystem::path& path, const RawDecodeOptions& options) const;
};

bool isRawPath(const std::filesystem::path& path);

} // namespace photonstack
