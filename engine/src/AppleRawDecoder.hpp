#pragma once

#include <filesystem>

#include "photonstack/ImageCodec.hpp"

namespace photonstack {

ImageReadResult decodeAppleRawFloat(const std::filesystem::path& path, const RawDecodeOptions& options);

} // namespace photonstack
