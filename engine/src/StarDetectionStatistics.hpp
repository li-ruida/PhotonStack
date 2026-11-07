#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace photonstack::detail {

// Input is nonempty and has no NaNs. MAD can contain positive infinity when
// a finite signed stellar sample has a deviation outside the float range.
inline double stellarMedian(std::span<float> samples) {
    auto candidates = samples;
    std::size_t upperRank = samples.size() / 2;
    const bool even = samples.size() % 2 == 0;
    if (samples.size() >= 65536) {
        const auto first = std::bit_cast<std::uint32_t>(samples.front());
        std::uint32_t differing = 0;
        for (float value : samples) differing |= first ^ std::bit_cast<std::uint32_t>(value);
        if (differing == 0) return samples.front();

        // Nonnegative IEEE binary32 values have the same order as their bit
        // patterns. Count the highest ten varying bits to locate the median
        // ranks exactly; signed data retain the ordinary comparison path.
        const bool nonnegative = ((first | differing) >> 31) == 0;
        if (nonnegative && (std::is_sorted(samples.begin(), samples.end()) ||
                            std::is_sorted(samples.rbegin(), samples.rend()))) {
            const auto lowerRank = (samples.size() - 1) / 2;
            return even ? .5 * (double(samples[upperRank]) + samples[lowerRank]) : samples[upperRank];
        }
        // Two equal probes are common on nearly flat or quantized images;
        // comparison selection there avoids a histogram with one dominant bin.
        const auto quarter = samples.size() / 4;
        if (nonnegative && samples[quarter] != samples[3 * quarter]) {
            constexpr unsigned bits = 10;
            constexpr unsigned mask = (1U << bits) - 1;
            const unsigned shift = std::max(0, int(std::bit_width(differing)) - int(bits));
            const auto bucket = [&](float value) { return (std::bit_cast<std::uint32_t>(value) >> shift) & mask; };
            std::array<std::size_t, mask + 1> counts{};
            for (float value : samples) ++counts[bucket(value)];
            std::size_t below = 0;
            unsigned bin = 0;
            while (below + counts[bin] <= upperRank) below += counts[bin++];

            const auto lowerRank = (samples.size() - 1) / 2;
            if (below <= lowerRank) {
                if (shift == 0) return std::bit_cast<float>((first & ~mask) | bin);
                if (counts[bin] <= samples.size() / 2) {
                    const auto end = std::partition(samples.begin(), samples.end(),
                                                    [&](float value) { return bucket(value) == bin; });
                    candidates = samples.first(std::size_t(end - samples.begin()));
                    upperRank -= below;
                }
            }
        }
    }
    const auto middle = candidates.begin() + upperRank;
    std::nth_element(candidates.begin(), middle, candidates.end());
    if (!even) return *middle;
    return .5 * (double(*middle) + *std::max_element(candidates.begin(), middle));
}

} // namespace photonstack::detail
