#include "photonstack/MosaicDiagnostics.hpp"
#include <algorithm>
#include <cmath>

namespace photonstack::detail {
namespace {
double median(std::vector<double> values) {
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}
using Samples = std::vector<MosaicSeamSample>;
std::optional<MosaicSeamStatistics> statistics(const Samples& samples, unsigned offset, unsigned channels) {
    if (samples.empty()) return std::nullopt;
    MosaicSeamStatistics result;
    result.samples = samples.size();
    for (unsigned c = 0; c < channels; ++c) {
        std::vector<double> first, second, differences, absolute;
        double squares = 0;
        for (const auto& sample : samples) {
            first.push_back(sample[offset][c]); second.push_back(sample[offset + 1][c]);
            const auto d = sample[offset + 1][c] - sample[offset][c];
            differences.push_back(d); absolute.push_back(std::abs(d)); squares += d * d;
        }
        result.firstMedian[c] = median(first); result.secondMedian[c] = median(second);
        result.medianDifference[c] = median(differences);
        result.rmsDifference[c] = std::sqrt(squares / samples.size());
        for (auto& d : differences) d = std::abs(d - result.medianDifference[c]);
        result.madDifference[c] = median(differences);
        std::sort(absolute.begin(), absolute.end());
        result.p95Absolute[c] = absolute[std::size_t(std::ceil(.95 * absolute.size())) - 1];
    }
    return result;
}
void finish(MosaicSeamRegion& region, const Samples& samples, unsigned channels) {
    region.before = statistics(samples, 0, channels);
    region.after = statistics(samples, 2, channels);
}
}
MosaicSeamPair measureMosaicSeam(std::size_t first, std::size_t second,
    const std::array<double, 4>& bounds, std::uint16_t channels, const MosaicSeamSampler& sample) {
    MosaicSeamPair result;
    result.first = first; result.second = second; result.summary.bounds = bounds;
    const int width = int(bounds[2] - bounds[0]), height = int(bounds[3] - bounds[1]);
    if (width <= 0 || height <= 0) return result;
    result.cells.resize(16);
    std::array<Samples, 16> cells;
    Samples all;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
        result.cells[y * 4 + x].bounds = {bounds[0] + width * x / 4., bounds[1] + height * y / 4.,
                                        bounds[0] + width * (x + 1) / 4., bounds[1] + height * (y + 1) / 4.};
    const int nx = std::min(48, width), ny = std::min(48, height);
    for (int gy = 0; gy < ny; ++gy) for (int gx = 0; gx < nx; ++gx) {
        const int x = int(bounds[0]) + int((gx + .5) * width / nx);
        const int y = int(bounds[1]) + int((gy + .5) * height / ny);
        const int cell = std::min(3, int((y - bounds[1]) * 4 / height)) * 4 +
                         std::min(3, int((x - bounds[0]) * 4 / width));
        ++result.summary.candidates; ++result.cells[cell].candidates;
        std::array<MosaicSeamSample, 9> patch;
        bool valid = true;
        for (int dy = -1; dy <= 1 && valid; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const auto value = sample(x + dx, y + dy);
            if (!value) { valid = false; break; }
            for (const auto& color : *value) for (unsigned c = 0; c < channels; ++c)
                if (!std::isfinite(color[c])) valid = false;
            patch[(dy + 1) * 3 + dx + 1] = *value;
        }
        if (!valid) continue;
        MosaicSeamSample medians{};
        for (int k = 0; k < 4; ++k) for (unsigned c = 0; c < channels; ++c) {
            std::vector<double> values;
            for (const auto& p : patch) values.push_back(p[k][c]);
            medians[k][c] = median(values);
        }
        all.push_back(medians); cells[cell].push_back(medians);
    }
    finish(result.summary, all, channels);
    for (int i = 0; i < 16; ++i) finish(result.cells[i], cells[i], channels);
    return result;
}
}
