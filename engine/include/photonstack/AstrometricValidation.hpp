#pragma once
#include "photonstack/ImageBuffer.hpp"
#include "photonstack/SkyProjection.hpp"
#include <vector>
namespace photonstack {
struct AstrometricStar { double x,y,flux; bool heldOut; };
std::vector<AstrometricStar> astrometricSources(const ImageBuffer& image);
// Converts a zero-based pixel center to ICRS degrees (x=RA,y=Dec).
SkyPoint pixelToSky(const SolvedPanel& panel,double x,double y);
struct AstrometricResidual { std::size_t source; double predictedX,predictedY,distance; };
struct AstrometricResidualSummary {
    std::size_t count=0,regions=0;double median=0,p95=0,maximum=0,spanX=0,spanY=0;
};
AstrometricResidualSummary summarizeAstrometry(const std::vector<AstrometricStar>& sources,
    const std::vector<AstrometricResidual>& residuals,unsigned width,unsigned height);
// Grid-wide positive Jacobian and round-trip check; rejects folded/excessive SIP.
void validateAstrometricGrid(const SolvedPanel& panel,unsigned width,unsigned height);
}
