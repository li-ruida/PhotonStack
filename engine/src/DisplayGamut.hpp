#pragma once

namespace photonstack::detail {

// Unit adjustment with a smooth knee at 80% of the available distance.
// Infinity means no boundary is approached. A rational shoulder retains
// precision near the boundary instead of rapidly rounding onto it.
inline double softGamutScale(double available) {
    if (available >= 1.25)
        return 1;
    const double knee = .8 * available;
    const double shoulder = .2 * available;
    return knee + shoulder * (1 - knee) / (shoulder + 1 - knee);
}

// Preserve one code of headroom in the standard 16-bit display export.
// Existing samples closer to an endpoint are not moved by this constant alone.
inline constexpr double displayCodeStep = 1.0 / 65535;

} // namespace photonstack::detail
