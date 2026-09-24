#pragma once
#include "photonstack/NonlocalDenoiser.hpp"

namespace photonstack {
enum class MultiscaleNoiseReferenceScope { AllComponents, LuminanceOnly };
struct MultiscaleDenoiseOptions {
    double luminance = 0;
    double chroma = 0;
    unsigned scales = 4;
    // LuminanceOnly keeps the original scene-based chroma solution, including
    // its luminance structure-support mask. Requires a noiseReference to act.
    MultiscaleNoiseReferenceScope noiseReferenceScope = MultiscaleNoiseReferenceScope::AllComponents;
};
// Linear RGB/RGBA with full coverage. Per-band MAD accounts for correlated
// reconstruction noise. The coarsest residual, dimensions and alpha are retained.
// A zero blend weight protects the original pixel exactly; no display clipping.
// An optional signal-free noise realization must have matching geometry and
// the same units, noise amplitude and correlation as the input at this stage.
// Its band statistics replace scene-derived MAD; its pixels are never mixed
// into the image. Raw noise must not be supplied for an already denoised input.
class MultiscaleDenoiser {
public:
    NonlocalDenoiseResult apply(const ImageBuffer& image, const ImageBuffer& blend,
        const MultiscaleDenoiseOptions& options = {}, const ImageBuffer* noiseReference = nullptr) const;
};
}
