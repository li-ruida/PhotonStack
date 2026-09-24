#pragma once

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

struct StarCentroidFit {
    bool ok = false;
    float x = 0;
    float y = 0;
    float relativeResidual = 0;
    // Covariance of the fitted Gaussian in native pixel squared units. These
    // describe this accepted local model, not an optical resolution estimate.
    float covarianceXX = 0;
    float covarianceYY = 0;
    float covarianceXY = 0;
};

// Local elliptical Gaussian plus planar sky on native pixels. Rejects incomplete
// coverage, clipped plateaus and poorly modelled/undersampled sources. Coordinates
// are only returned on success; the input and detection photometry are unchanged.
StarCentroidFit fitStarCentroid(const ImageBuffer& image, float x, float y);

} // namespace photonstack
