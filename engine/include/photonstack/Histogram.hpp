#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct HistogramOptions {
    std::size_t bins = 256;
};

struct HistogramResult {
    bool ok = false;
    std::vector<double> bins;
    float minimum = 0.0F;
    float maximum = 0.0F;
    float mean = 0.0F;
    std::string errorCode;
    std::string message;
};

class Histogram {
  public:
    HistogramResult luminance(const ImageBuffer& image, const HistogramOptions& options = {}) const;
};

} // namespace photonstack
