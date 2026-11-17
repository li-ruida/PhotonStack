#pragma once

#include <array>
#include <filesystem>
#include <vector>

namespace photonstack {

// FITS reference pixels are one-based; the mapping API uses zero-based sample centers.
struct TanGrid {
    double x, y, ra, dec;
    std::array<double, 4> cd; // degrees/pixel, row major
};
struct SipTerm {
    unsigned i, j;
    double a, b;
};
struct SolvedPanel {
    TanGrid grid;
    std::vector<SipTerm> distortion;
};

// Reads a primary ICRS TAN or TAN-SIP solution. Unsupported conventions and
// incomplete solutions throw; a pointing estimate alone is not a solution.
SolvedPanel readSolvedPanel(const std::filesystem::path& path);

struct SkyPoint {
    double x, y;
};
class SkyProjection {
  public:
    SkyProjection(const SolvedPanel& panel, const TanGrid& output);
    SkyPoint toCanvas(double x, double y) const;
    SkyPoint toPanel(double x, double y) const;

  private:
    TanGrid panel_, output_;
    std::array<double, 9> forward_, inverse_;
    std::vector<SipTerm> terms_;
    SkyPoint distortion(double x, double y) const;
};
} // namespace photonstack
