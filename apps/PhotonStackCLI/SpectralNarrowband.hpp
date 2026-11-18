#pragma once
#include "ChannelRecipe.hpp"
#include "photonstack/SpectralCalibration.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace photonstack::cli {
struct NarrowbandModel {
    std::string layout, baseSHA;
    std::array<double, 3> centers{}, widths{};
};
inline void validateNarrowband(const NarrowbandModel& m) {
    if (!std::set<std::string>{"mono", "osc-dual", "extracted-hoo"}.contains(m.layout) || !channelSHA(m.baseSHA))
        throw std::invalid_argument("Invalid narrowband layout or base response digest");
    for (unsigned c = 0; c < 3; ++c)
        if (!std::isfinite(m.centers[c]) || !std::isfinite(m.widths[c]) || m.widths[c] < 3 || m.widths[c] > 100 ||
            m.centers[c] - m.widths[c] / 2 < 336 || m.centers[c] + m.widths[c] / 2 > 1020)
            throw std::invalid_argument(
                "Nominal passbands require width 3..100 nm and full support inside 336..1020 nm");
    if (m.layout != "mono" && (m.centers[1] != m.centers[2] || m.widths[1] != m.widths[2] ||
                               std::abs(m.centers[0] - m.centers[1]) <= (m.widths[0] + m.widths[1]) / 2))
        throw std::invalid_argument(
            "Dual-band layouts require two disjoint passbands and identical G/B passband settings");
}
// Explicit approximate transmission: a unit plateau with linear edges of
// width 1e-4 * nominal bandwidth, zero outside. This is not a measured filter.
inline SpectralCurve narrowbandWindow(const NarrowbandModel& m, unsigned channel) {
    validateNarrowband(m);
    std::vector<std::pair<double, double>> bands;
    if (m.layout == "mono")
        bands.emplace_back(m.centers[channel], m.widths[channel]);
    else
        for (unsigned c : {0u, 1u})
            bands.emplace_back(m.centers[c], m.widths[c]);
    std::sort(bands.begin(), bands.end());
    SpectralCurve result;
    for (auto [center, width] : bands) {
        const double low = center - width / 2, high = center + width / 2, edge = width * 1e-4;
        for (double x : {low, low + edge, high - edge, high})
            result.wavelength.push_back(x);
        for (double y : {0., 1., 1., 0.})
            result.value.push_back(y);
    }
    return result;
}
inline bool sameCurve(const SpectralCurve& a, const SpectralCurve& b) {
    return a.wavelength == b.wavelength && a.value == b.value;
}
inline bool sameBand(const SpectralBand& a, const SpectralBand& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), sameCurve);
}
inline void narrowbandJSON(std::ostream& out, const std::optional<NarrowbandModel>& m) {
    if (!m)
        return;
    out << ",\"narrowband\":{\"layout\":" << channelJSON(m->layout) << ",\"baseSHA\":" << channelJSON(m->baseSHA)
        << ",\"model\":\"trapezoid-edge-1e-4\",\"centers\":[";
    for (unsigned c = 0; c < 3; ++c)
        out << (c ? "," : "") << m->centers[c];
    out << "],\"widths\":[";
    for (unsigned c = 0; c < 3; ++c)
        out << (c ? "," : "") << m->widths[c];
    out << "]}";
}
} // namespace photonstack::cli
