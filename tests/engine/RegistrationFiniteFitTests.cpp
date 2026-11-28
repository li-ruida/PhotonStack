// Exercise the private fitting boundary without publishing an engine API solely for tests.
#include "../../engine/src/Registration.cpp"
#include <stdexcept>
#include <numeric>

int main() {
    using namespace photonstack;
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("Non-finite registration fit regression"); };
    DistortionTransform transform; transform.useProjective = true; transform.projective.h20 = 1;
    std::vector<MatchedStarPair> pairs;
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        const float rx = 10 + 20*x, ry = 10 + 20*y;
        const auto [mx, my] = inverseProjectivePoint(rx, ry, transform.projective);
        pairs.push_back({{.x=rx, .y=ry}, {.x=mx+.25F, .y=my-.5F}});
    }
    pairs.push_back({{.x=1, .y=20}, {.x=10, .y=20}}); // Inverse vanishing line.
    pairs.push_back({{.x=20, .y=20}, {.x=NAN, .y=20}});
    std::vector<std::size_t> indices(pairs.size());
    std::iota(indices.begin(), indices.end(), 0);
    const auto fit = fitPolynomialCorrection(pairs, indices, transform, 256, 256);
    require(fit.ok && std::isfinite(fit.rms) && fit.rms < .001F);
    std::vector<std::size_t> inliers;
    const auto robust = fitPolynomialCorrectionRobust(pairs, transform, 256, 256, 30, 3, inliers);
    require(robust.ok && std::isfinite(robust.rms) && inliers.size() == 64);
    require(std::all_of(inliers.begin(), inliers.end(), [](auto i) { return i < 64; }));
    pairs.assign(40, {{.x=1, .y=20}, {.x=10, .y=20}});
    require(!fitPolynomialCorrectionRobust(pairs, transform, 256, 256, 30, 3, inliers).ok);
    std::vector<std::vector<double>> matrix{{1, NAN}};
    std::vector<double> solution;
    require(!solveLinearSystem(matrix, solution));
    SpatialPointIndex grid(64, 64, 1, {{0, NAN, 0}, {1, 4, 4}, {2, 10, 10}});
    require(!grid.nearest(NAN, 4, 1).found);
    require(!grid.nearest(std::numeric_limits<float>::max(), 4, 1).found);
    require(grid.nearest(4, 4, std::numeric_limits<float>::max()).index == 1);
}
