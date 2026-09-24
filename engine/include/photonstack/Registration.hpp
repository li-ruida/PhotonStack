#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "photonstack/ImageBuffer.hpp"
#include "photonstack/StarDetector.hpp"

namespace photonstack {

struct Translation {
    float dx = 0.0F;
    float dy = 0.0F;
};

struct SimilarityTransform {
    float scale = 1.0F;
    float rotationRadians = 0.0F;
    float dx = 0.0F;
    float dy = 0.0F;
};

struct AffineTransform {
    float a = 1.0F;
    float b = 0.0F;
    float c = 0.0F;
    float d = 1.0F;
    float dx = 0.0F;
    float dy = 0.0F;
};

struct ProjectiveTransform {
    float h00 = 1.0F;
    float h01 = 0.0F;
    float h02 = 0.0F;
    float h10 = 0.0F;
    float h11 = 1.0F;
    float h12 = 0.0F;
    float h20 = 0.0F;
    float h21 = 0.0F;
    float h22 = 1.0F;
};

struct LocalAlignmentControlPoint {
    float x = 0.0F;
    float y = 0.0F;
    float dx = 0.0F;
    float dy = 0.0F;
};

struct DistortionTransform {
    static constexpr std::size_t polynomialTermCount = 10;

    SimilarityTransform global;
    ProjectiveTransform projective;
    bool useProjective = false;
    std::array<float, polynomialTermCount> polynomialDx = {};
    std::array<float, polynomialTermCount> polynomialDy = {};
    bool usePolynomial = false;
    std::vector<LocalAlignmentControlPoint> controlPoints;
    float influenceRadius = 256.0F;
};

enum class RegistrationInterpolation { Bilinear, Bicubic };

struct RegistrationOptions {
    StarDetectionOptions starDetection;
    // Similarity searches may use a wider radius for initial correspondence,
    // but final fit residuals are bounded by max(2 * matchTolerance, 2.5 px).
    float matchTolerance = 3.0F;
    std::size_t minimumMatches = 3;
    bool similarityFallbackToTranslation = true;
    // Refine coarse similarity matches with native-pixel PSF centers. With
    // estimateAffine, adopt affine geometry only after cross-validation.
    bool refineSimilarityCentroids = false;
};

struct RegistrationResult {
    bool ok = false;
    Translation translation;
    SimilarityTransform transform;
    AffineTransform affine;
    std::size_t matches = 0;
    std::size_t detectedReferenceStars = 0;
    std::size_t detectedMovingStars = 0;
    float inlierRatio = 0.0F;
    bool usedFallback = false;
    bool usedCentroidRefinement = false;
    float centroidRefinementRms = 0.0F;
    bool usedCentroidAffine = false;
    // Held-out clipped MSE ratio; -1 means model comparison was unavailable.
    float centroidAffineValidationRatio = -1.0F;
    // All native pairs used during refinement (including fit outliers), in
    // [reference x,y, moving x,y] order, to allow independent validation stars.
    std::vector<std::array<float, 4>> centroidRefinementPairs;
    std::string errorCode;
    std::string message;
};

// The sample pointer remains valid only for the duration of each callback.
using RegistrationRowConsumer = std::function<bool(std::uint32_t row, const float* samples, std::size_t sampleCount)>;
// Coordinates are emitted as interleaved target x/y pairs for each source pixel in the row.
using RegistrationCoordinateConsumer =
    std::function<bool(std::uint32_t row, const float* coordinates, std::size_t coordinateCount)>;

class Registration {
  public:
    explicit Registration(RegistrationInterpolation interpolation = RegistrationInterpolation::Bilinear)
        : interpolation_(interpolation) {}
    RegistrationResult estimateTranslation(const ImageBuffer& reference, const ImageBuffer& moving,
                                           const RegistrationOptions& options = {}) const;
    RegistrationResult estimateSimilarity(const ImageBuffer& reference, const ImageBuffer& moving,
                                          const RegistrationOptions& options = {}) const;
    RegistrationResult estimateAffine(const ImageBuffer& reference, const ImageBuffer& moving,
                                      const RegistrationOptions& options = {}) const;
    RegistrationResult estimateDistortion(const ImageBuffer& reference, const ImageBuffer& moving,
                                          DistortionTransform& transform,
                                          const RegistrationOptions& options = {}) const;
    ImageBuffer applyTranslation(const ImageBuffer& image, Translation translation) const;
    ImageBuffer applySimilarity(const ImageBuffer& image, SimilarityTransform transform) const;
    ImageBuffer applyAffine(const ImageBuffer& image, AffineTransform transform) const;
    ImageBuffer applyDistortion(const ImageBuffer& image, const DistortionTransform& transform) const;
    bool renderTranslationRows(const ImageBuffer& image, Translation translation,
                               const RegistrationRowConsumer& consumer) const;
    bool renderSimilarityRows(const ImageBuffer& image, SimilarityTransform transform,
                              const RegistrationRowConsumer& consumer) const;
    bool renderAffineRows(const ImageBuffer& image, AffineTransform transform,
                          const RegistrationRowConsumer& consumer) const;
    bool renderDistortionRows(const ImageBuffer& image, const DistortionTransform& transform,
                              const RegistrationRowConsumer& consumer) const;
    bool renderDistortionForwardRows(const ImageBuffer& image, const DistortionTransform& transform,
                                     const RegistrationCoordinateConsumer& consumer) const;

  private:
    RegistrationInterpolation interpolation_;
};

} // namespace photonstack
