#pragma once
#include <array>
#include <cstddef>
#include "photonstack/ImageBuffer.hpp"

namespace photonstack {
struct GeneralizedStretchOptions {
    double logStretch = 3; // ln(1+D), 0...10
    double localIntensity = 1; // b, -5...15
    double symmetry = 0, shadowProtection = 0, highlightProtection = 1;
    double black = 0, white = 1; // Input physical units, shared by RGB.
    bool inverse = false, saturation = false;
    bool luminance = true; // Rec.709 scaling with bounded RGB blending.
};
class GeneralizedStretchCurve {
    GeneralizedStretchOptions options_;
    double d_,q0_,span_,lowValue_,highValue_,lowSlope_,highSlope_;
    double base(double distance) const;
    double derivative(double distance) const;
    double signedBase(double x) const;
    double forward(double x) const;
public:
    explicit GeneralizedStretchCurve(const GeneralizedStretchOptions&);
    double evaluate(double x) const; // Normalized x in [0,1]; exact formula, not LUT.
};
struct GeneralizedStretchResult {
    ImageBuffer image, normalized;
    std::size_t coveredPixels=0, belowBlack=0, aboveWhite=0, gamutBlendedPixels=0;
    std::array<std::size_t,256> beforeHistogram{},afterHistogram{};
};
// Produces display-encoded values. Neither output is a linear science master.
GeneralizedStretchResult generalizedStretch(const ImageBuffer&,const GeneralizedStretchOptions&);
}
