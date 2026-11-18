#pragma once
#include "photonstack/FitsCodec.hpp"
#include "photonstack/Registration.hpp"
#include <optional>

namespace photonstack {
enum class MasterRegistrationMode { Stars, Wcs };
struct MasterRegistrationMeasurement {
    SimilarityTransform transform;
    std::size_t fitStars = 0, checkStars = 0;
    double fitRms = 0, median = 0, p95 = 0, validFraction = 0;
};
struct RegisteredMaster {
    ImageBuffer image, validity;
    MasterRegistrationMeasurement measurement;
};
// Output coordinates and dimensions exactly follow the reference. Positive
// bilinear weights retain signed intensities. Missing support stays invalid.
// replay reuses the measured similarity; no matching is performed again.
RegisteredMaster registerMonoMaster(const LinearMonoMaster& reference, const LinearMonoMaster& moving,
    MasterRegistrationMode mode, const MasterRegistrationMeasurement* replay = nullptr, bool isReference = false);
} // namespace photonstack
