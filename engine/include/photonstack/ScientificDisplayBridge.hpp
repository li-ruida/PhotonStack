#pragma once

#include <cstddef>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct ScientificDisplayBridgeResult {
    bool ok = false;
    ImageBuffer image;
    std::size_t correctedSamples = 0;
    double displayToScientificScale = 1.0;
    double displayToScientificOffset = 0.0;
    std::string errorCode;
    std::string message;
};

class ScientificDisplayBridge {
  public:
    ScientificDisplayBridgeResult applyCorrections(
        const ImageBuffer& scientificSource,
        const ImageBuffer& displaySource,
        const ImageBuffer& correctedDisplay
    ) const;
};

} // namespace photonstack
