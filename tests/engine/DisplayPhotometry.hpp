#pragma once

#include <cmath>

// Decode exported sRGB codes independently of the production implementation.
inline double displayLight(double code) {
    return code <= 0.04045 ? code / 12.92 : std::pow((code + 0.055) / 1.055, 2.4);
}
