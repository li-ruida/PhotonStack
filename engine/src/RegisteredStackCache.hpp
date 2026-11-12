#pragma once
#include "photonstack/Stacker.hpp"
#include <array>

namespace photonstack::detail {
// Run-owned, interleaved linear RGBA float planes. The writer and reader live
// in one process; paths in a user recipe are never interpreted as this format.
struct RegisteredFrame {
    std::uint32_t width = 0, height = 0;
    std::array<float, 3> background{};
};
StackResult writeRegisteredFrame(const std::filesystem::path& path, const ImageBuffer& image, bool measureBackground,
                                 RegisteredFrame& metadata);
StackResult combineRegisteredFrames(const std::vector<std::filesystem::path>& paths,
                                    const std::vector<RegisteredFrame>& frames, const StackOptions& options);
} // namespace photonstack::detail
