#pragma once

#include "photonstack/ImageBuffer.hpp"
#include <array>
#include <span>
#include <vector>

namespace photonstack {

enum class BayerPattern { RGGB, GRBG, GBRG, BGGR };

struct CfaDrizzleOptions {
    std::uint32_t width = 0, height = 0; // Output pixels, including output scale.
    double originX = 0, originY = 0; // Reference-grid center of native pixel (0, 0).
    double scale = 1, pixfrac = 1;
};

struct CfaDrizzleFrame {
    std::uint32_t width = 0, height = 0;
    std::span<const float> samples; // One calibrated sensor sample per pixel, stored row order.
    std::span<const float> validity; // Optional [0,1] sensor validity/weight; empty means one.
    BayerPattern pattern = BayerPattern::RGGB;
    int xOffset = 0, yOffset = 0; // Same stored-row convention as FITS XBAYROFF/YBAYROFF.
    // Source integer pixel centers -> reference integer pixel centers: ax+by+tx, cx+dy+ty.
    std::array<double, 6> transform{1, 0, 0, 1, 0, 0};
    std::array<double, 3> background{0, 0, 0};
    std::array<double, 3> gain{1, 1, 1}; // (sample - background[color]) * gain[color].
    double weight = 1;
};

// Experimental scientific accumulator. Each transformed square is clipped exactly
// against output pixel squares. Colors have independent sums and coverage weights.
// The result is coverage-normalized surface brightness, not summed detector counts.
// No demosaicing, rejection, sharpening, registration estimation, or hole filling.
class CfaDrizzleAccumulator {
public:
    explicit CfaDrizzleAccumulator(CfaDrizzleOptions options);
    void add(const CfaDrizzleFrame& frame); // Invalid geometry throws before accumulation.
    [[nodiscard]] ImageBuffer image() const; // Missing channel: NaN; alpha=1 only if all RGB covered.
    [[nodiscard]] std::span<const double> weights() const { return weights_; } // Interleaved RGB.
    [[nodiscard]] std::span<const double> weightedSums() const { return sums_; }
    [[nodiscard]] static unsigned colorAt(BayerPattern pattern, int x, int y,
                                          int xOffset = 0, int yOffset = 0);
private:
    CfaDrizzleOptions options_;
    std::vector<double> sums_, weights_;
};

} // namespace photonstack
