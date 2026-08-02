#pragma once

#include <cstdint>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct ResizeResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

class ImageResizer {
  public:
    ResizeResult resizeToWidth(const ImageBuffer& input, std::uint32_t targetWidth) const;
};

} // namespace photonstack
