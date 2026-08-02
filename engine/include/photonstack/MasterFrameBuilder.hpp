#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"
#include "photonstack/ImageCodec.hpp"

namespace photonstack {

enum class MasterFrameMethod {
    Average,
    Median,
};

struct MasterFrameOptions {
    MasterFrameMethod method = MasterFrameMethod::Median;
    RawDecodeOptions raw;
};

struct MasterFrameResult {
    bool ok = false;
    ImageBuffer image;
    std::string errorCode;
    std::string message;
};

class MasterFrameBuilder {
  public:
    MasterFrameResult build(const std::vector<std::filesystem::path>& inputs, const MasterFrameOptions& options) const;
};

} // namespace photonstack
