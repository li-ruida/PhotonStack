#pragma once

#include "photonstack/ParallelRanges.hpp"
#include <array>
#include <cmath>

namespace photonstack::cli {
inline float decodeUnitSRGB(float value) {
    const auto decode = [](float v) { return v <= .04045F ? v / 12.92F : std::pow((v + .055F) / 1.055F, 2.4F); };
    if (!(value >= 0 && value <= 1)) return decode(value);
    static const auto lut = [&] {
        std::array<float, 65536> result;
        for (unsigned n = 0; n < result.size(); ++n) result[n] = decode(float(n) / 65535.F);
        return result;
    }();
    const auto n = static_cast<unsigned>(std::lround(value * 65535.F));
    // Scientific floats and unpremultiplied fractional codes retain their exact transform.
    return value == float(n) / 65535.F ? lut[n] : decode(value);
}

template<class Work>
void processPixels(std::size_t pixels, Work work) {
    const auto workers = std::clamp<std::size_t>(pixels / 65536, 1, 8);
    detail::runWorkers(workers, [&](std::size_t worker) {
        for (auto p = pixels * worker / workers; p < pixels * (worker + 1) / workers; ++p) work(p);
    });
}
} // namespace photonstack::cli
