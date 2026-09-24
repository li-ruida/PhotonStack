#pragma once
#include "photonstack/ImageBuffer.hpp"
#include <array>
#include <string>

namespace photonstack {
struct ChannelAlignmentResult {
    bool ok = false;
    ImageBuffer image;
    // Measured source displacement relative to green, in pixels (R, G, B).
    std::array<double, 3> dx{}, dy{}, scatter{};
    std::array<unsigned, 3> stars{};
    std::array<bool, 3> applied{};
    std::uint32_t sourceWidth = 0, sourceHeight = 0;
    std::string errorCode, message;
};
// Conservative global translation from compact stellar Gaussian fits. Sparse,
// spatially inconsistent, already aligned or >1.5px shifts are left untouched.
// Full-coverage linear RGB only. Green and alpha remain exactly unchanged.
class ChannelAlignment {
  public:
    ChannelAlignmentResult apply(const ImageBuffer&) const;
    // Replay measured transforms without detecting stars in the supplied image.
    // Matching dimensions and full coverage are required. Useful for propagating
    // a co-registered noise realization through exactly the same linear kernel.
    ChannelAlignmentResult applyMeasured(const ImageBuffer&, const ChannelAlignmentResult&) const;
};
} // namespace photonstack
