#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace photonstack {
struct MosaicExposureCorrection {
    std::array<float, 3> gains{1, 1, 1};
    std::array<float, 3> offsets{};
};
struct MosaicSeamStatistics {
    std::size_t samples = 0;
    std::array<double, 3> firstMedian{}, secondMedian{}, medianDifference{}, rmsDifference{}, p95Absolute{}, madDifference{};
};
struct MosaicSeamRegion {
    // Canvas pixel edges, [left, top, right-exclusive, bottom-exclusive].
    std::array<double, 4> bounds{};
    std::size_t candidates = 0;
    std::optional<MosaicSeamStatistics> before, after;
};
struct MosaicFinalSeamStatistics {
    std::size_t samples=0, backgroundCandidates=0, structuredSamples=0;
    std::array<double,3> backgroundGradientExcessRms{}, backgroundGradientExcessP95{}, structureExcessP95{},
                         structureExcessFraction{}, rangeExcessP95{}, tolerance{};
    std::string status="insufficient-samples";
};
struct MosaicSeamPair {
    std::size_t first = 0, second = 0;
    MosaicSeamRegion summary;
    // Row-major 4 x 4 spatial bins. Empty cells carry null statistics.
    std::vector<MosaicSeamRegion> cells;
    std::optional<MosaicFinalSeamStatistics> final;
};
struct MosaicSeamDiagnostics {
    std::uint32_t width = 0, height = 0;
    std::uint16_t channels = 0;
    bool preview = false;
    std::string encoding;
    std::vector<MosaicExposureCorrection> corrections;
    std::vector<MosaicSeamPair> pairs;
};
namespace detail {
// One common canvas coordinate; raw/corrected values for first/second panel.
using MosaicSeamSample = std::array<std::array<double, 3>, 4>;
using MosaicSeamSampler = std::function<std::optional<MosaicSeamSample>(double, double)>;
// Two corrected input panels and the actual blended image at one location.
using MosaicFinalSample=std::array<std::array<double,3>,3>;
using MosaicFinalSampler=std::function<std::optional<MosaicFinalSample>(double,double)>;
MosaicFinalSeamStatistics measureFinalMosaicSeam(const std::array<double,4>& bounds,
    std::uint16_t channels,const MosaicFinalSampler& sample);
MosaicSeamPair measureMosaicSeam(std::size_t first, std::size_t second,
    const std::array<double, 4>& bounds, std::uint16_t channels, const MosaicSeamSampler& sample);
}
}
