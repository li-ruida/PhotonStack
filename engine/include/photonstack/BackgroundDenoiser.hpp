#pragma once
#include "photonstack/NonlocalDenoiser.hpp"

namespace photonstack {
// Residual background smoothing on linear RGB/RGBA. The blend map excludes
// sources from both the background estimate and the final correction. Strong
// residuals are retained; no black clipping, resampling or global color gains.
class BackgroundDenoiser {
public:
    NonlocalDenoiseResult apply(const ImageBuffer& image, const ImageBuffer& blend,
                               double strength) const;
};
}
