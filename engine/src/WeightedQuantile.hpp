#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

namespace photonstack::detail {

// Interpolate between sample weight centers. All callers supply finite values
// and positive weights. Return an empty value for an empty or invalid population.
template <class Sample>
std::optional<float> weightedSampleQuantile(std::vector<Sample>& samples, double q) {
    if (samples.empty()) return {};
    q = std::clamp(q, 0., 1.);
    const auto less = [](const auto& a, const auto& b) { return a.value < b.value; };
    const auto interpolate = [&](float a, float b, double lo, double hi) {
        if (a == b || hi <= lo) return b;
        return float(double(a) + (double(b) - a) * std::clamp((q - lo) / (hi - lo), 0., 1.));
    };
    const auto n = samples.size();
    const auto w = samples.front().weight;
    if (w > 0 && std::all_of(samples.begin(), samples.end(), [&](const auto& v) { return v.weight == w; })) {
        const double rank = std::clamp(std::ceil(q * n - .5), 0., double(n - 1));
        auto right = samples.begin() + static_cast<std::ptrdiff_t>(rank);
        std::nth_element(samples.begin(), right, samples.end(), less);
        if (right == samples.begin()) return right->value;
        const auto left = std::max_element(samples.begin(), right, less);
        return interpolate(left->value, right->value, (rank - .5) / n, (rank + .5) / n);
    }

    double total = 0;
    for (const auto& s : samples) total += s.weight;
    if (!(total > 0) || !std::isfinite(total)) return {};

    if (n >= 128 && n <= std::numeric_limits<std::uint32_t>::max()) {
        // Partition indices to preserve the original ordering for the sorted
        // fallback when equal-valued neighbors have different weights.
        std::vector<std::uint32_t> indices(n);
        std::iota(indices.begin(), indices.end(), std::uint32_t{0});
        auto begin = indices.begin(), end = indices.end();
        double before = 0;
        const double target = q * total;
        while (begin != end) {
            auto middle = begin + (end - begin) / 2;
            std::nth_element(begin, middle, end, [&](auto i, auto j) { return less(samples[i], samples[j]); });
            const float pivot = samples[*middle].value;
            const auto first = std::partition(begin, end, [&](auto i) { return samples[i].value < pivot; });
            const auto last = std::partition(first, end, [&](auto i) { return samples[i].value == pivot; });
            double lower = before;
            for (auto i = begin; i != first; ++i) lower += samples[*i].weight;
            double upper = lower;
            for (auto i = first; i != last; ++i) upper += samples[*i].weight;
            if (target < lower) { end = first; continue; }
            if (target > upper) { begin = last; before = upper; continue; }
            const auto weight = samples[*first].weight;
            if (std::any_of(first, last, [&](auto i) { return samples[i].weight != weight; })) break;
            const double lowCenter = (lower + weight * .5) / total;
            const double highCenter = (upper - weight * .5) / total;
            if (q >= lowCenter && q <= highCenter) return pivot;
            const bool below = q < lowCenter;
            const Sample* neighbor = nullptr;
            bool ambiguous = false;
            for (const auto& sample : samples) {
                if (below ? sample.value >= pivot : sample.value <= pivot) continue;
                if (!neighbor || (below ? sample.value > neighbor->value : sample.value < neighbor->value)) {
                    neighbor = &sample;
                    ambiguous = false;
                } else if (sample.value == neighbor->value && sample.weight != neighbor->weight) {
                    ambiguous = true;
                }
            }
            if (!neighbor) return pivot;
            if (ambiguous) break;
            return below ? interpolate(neighbor->value, pivot, (lower - neighbor->weight * .5) / total, lowCenter)
                         : interpolate(pivot, neighbor->value, highCenter, (upper + neighbor->weight * .5) / total);
        }
    }
    std::sort(samples.begin(), samples.end(), less);
    total = 0;
    for (const auto& sample : samples) total += sample.weight;
    double cumulative = 0, previous = 0;
    float value = samples.front().value;
    for (const auto& sample : samples) {
        cumulative += sample.weight;
        const double position = (cumulative - sample.weight * .5) / total;
        if (position >= q) return interpolate(value, sample.value, previous, position);
        value = sample.value;
        previous = position;
    }
    return samples.back().value;
}
} // namespace photonstack::detail
