#include "photonstack/Registration.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "StraightAlphaSampling.hpp"

namespace photonstack {
namespace {

constexpr std::size_t kPolynomialFitTermCount = 6;

RegistrationResult registrationError(std::string code, std::string message) {
    RegistrationResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool validRegistrationOptions(const RegistrationOptions& options) {
    return std::isfinite(options.matchTolerance) && options.matchTolerance > 0.0F &&
           options.minimumMatches > 0;
}

bool validImageStructure(const ImageBuffer& image) {
    return !image.empty() && image.channels > 0 && image.pixels.size() == image.sampleCount();
}

bool validImageBuffer(const ImageBuffer& image) {
    return validImageStructure(image) &&
           std::all_of(image.pixels.begin(), image.pixels.end(), [](float value) {
               return std::isfinite(value);
           });
}

bool finiteSimilarityTransform(const SimilarityTransform& transform) {
    return std::isfinite(transform.scale) && std::isfinite(transform.rotationRadians) &&
           std::isfinite(transform.dx) && std::isfinite(transform.dy);
}

bool finiteAffineTransform(const AffineTransform& transform) {
    return std::isfinite(transform.a) && std::isfinite(transform.b) && std::isfinite(transform.c) &&
           std::isfinite(transform.d) && std::isfinite(transform.dx) && std::isfinite(transform.dy);
}

bool finiteProjectiveTransform(const ProjectiveTransform& transform) {
    return std::isfinite(transform.h00) && std::isfinite(transform.h01) && std::isfinite(transform.h02) &&
           std::isfinite(transform.h10) && std::isfinite(transform.h11) && std::isfinite(transform.h12) &&
           std::isfinite(transform.h20) && std::isfinite(transform.h21) && std::isfinite(transform.h22);
}

bool finiteDistortionTransform(const DistortionTransform& transform) {
    if (!finiteSimilarityTransform(transform.global) || !finiteProjectiveTransform(transform.projective) ||
        !std::isfinite(transform.influenceRadius)) {
        return false;
    }
    const auto finiteValue = [](float value) { return std::isfinite(value); };
    if (!std::all_of(transform.polynomialDx.begin(), transform.polynomialDx.end(), finiteValue) ||
        !std::all_of(transform.polynomialDy.begin(), transform.polynomialDy.end(), finiteValue)) {
        return false;
    }
    return std::all_of(transform.controlPoints.begin(), transform.controlPoints.end(), [](const auto& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.dx) &&
               std::isfinite(point.dy);
    });
}

float distanceSquared(const Star& left, const Star& right) {
    const float dx = left.x - right.x;
    const float dy = left.y - right.y;
    return dx * dx + dy * dy;
}

float sampleNearest(const ImageBuffer& image, int x, int y, std::uint16_t channel) {
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) || y >= static_cast<int>(image.height)) {
        return 0.0F;
    }
    return image
        .pixels[(static_cast<std::size_t>(y) * image.width + static_cast<std::uint32_t>(x)) * image.channels + channel];
}

float sampleBilinear(const ImageBuffer& image, float x, float y, std::uint16_t channel) {
    return detail::sampleBilinearStraightAlpha(image, x, y, channel, false);
}

struct MatchedStarPair {
    Star reference;
    Star moving;
};

struct OffsetBin {
    std::vector<Translation> offsets;
};

struct StarDescriptor {
    std::size_t index = 0;
    std::vector<float> distances;
};

struct AffineFit {
    bool ok = false;
    AffineTransform transform;
    float rms = 0.0F;
};

struct SimilarityFit {
    bool ok = false;
    SimilarityTransform transform;
    AffineTransform affine;
    float rms = 0.0F;
};

struct ProjectiveFit {
    bool ok = false;
    ProjectiveTransform transform;
    float rms = 0.0F;
};

struct PolynomialCorrectionFit {
    bool ok = false;
    std::array<float, DistortionTransform::polynomialTermCount> dx = {};
    std::array<float, DistortionTransform::polynomialTermCount> dy = {};
    float rms = 0.0F;
};

struct LocalGridControl {
    float distanceSquared = 0.0F;
    float x = 0.0F;
    float y = 0.0F;
    float dx = 0.0F;
    float dy = 0.0F;
};

struct LocalCorrection {
    bool ok = false;
    float dxDu = 0.0F;
    float dxDv = 0.0F;
    float dxOffset = 0.0F;
    float dyDu = 0.0F;
    float dyDv = 0.0F;
    float dyOffset = 0.0F;
};

struct IndexedPoint {
    std::size_t index = 0;
    float x = 0.0F;
    float y = 0.0F;
};

struct NearestPoint {
    bool found = false;
    std::size_t index = 0;
    float distanceSquared = std::numeric_limits<float>::max();
};

class SpatialPointIndex {
  public:
    SpatialPointIndex(float width, float height, float radius, std::vector<IndexedPoint> points)
        : cellSize_(std::max(1.0F, radius)), points_(std::move(points)) {
        columns_ = std::max(1, static_cast<int>(std::ceil(width / cellSize_)) + 2);
        rows_ = std::max(1, static_cast<int>(std::ceil(height / cellSize_)) + 2);
        cells_.resize(static_cast<std::size_t>(columns_ * rows_));
        for (std::size_t pointIndex = 0; pointIndex < points_.size(); ++pointIndex) {
            const int cellX = clampCellX(static_cast<int>(std::floor(points_[pointIndex].x / cellSize_)));
            const int cellY = clampCellY(static_cast<int>(std::floor(points_[pointIndex].y / cellSize_)));
            cells_[cellIndex(cellX, cellY)].push_back(pointIndex);
        }
    }

    NearestPoint nearest(float x, float y, float radius) const {
        NearestPoint nearestPoint;
        const float radiusSquared = radius * radius;
        const int centerX = clampCellX(static_cast<int>(std::floor(x / cellSize_)));
        const int centerY = clampCellY(static_cast<int>(std::floor(y / cellSize_)));
        const int cellRadius = std::max(1, static_cast<int>(std::ceil(radius / cellSize_)));
        const int minX = std::max(0, centerX - cellRadius);
        const int maxX = std::min(columns_ - 1, centerX + cellRadius);
        const int minY = std::max(0, centerY - cellRadius);
        const int maxY = std::min(rows_ - 1, centerY + cellRadius);

        for (int cellY = minY; cellY <= maxY; ++cellY) {
            for (int cellX = minX; cellX <= maxX; ++cellX) {
                for (const auto pointIndex : cells_[cellIndex(cellX, cellY)]) {
                    const auto& point = points_[pointIndex];
                    const float dx = point.x - x;
                    const float dy = point.y - y;
                    const float distance = dx * dx + dy * dy;
                    if (distance <= radiusSquared && distance < nearestPoint.distanceSquared) {
                        nearestPoint = {.found = true, .index = point.index, .distanceSquared = distance};
                    }
                }
            }
        }

        return nearestPoint;
    }

  private:
    std::size_t cellIndex(int x, int y) const {
        return static_cast<std::size_t>(y * columns_ + x);
    }

    int clampCellX(int x) const {
        return std::clamp(x + 1, 0, columns_ - 1);
    }

    int clampCellY(int y) const {
        return std::clamp(y + 1, 0, rows_ - 1);
    }

    float cellSize_ = 1.0F;
    int columns_ = 1;
    int rows_ = 1;
    std::vector<IndexedPoint> points_;
    std::vector<std::vector<std::size_t>> cells_;
};

bool solve3x3(double matrix[3][4], double out[3]) {
    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row) {
            if (std::fabs(matrix[row][column]) > std::fabs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (std::fabs(matrix[pivot][column]) < 1.0e-9) {
            return false;
        }
        if (pivot != column) {
            for (int k = column; k < 4; ++k) {
                std::swap(matrix[column][k], matrix[pivot][k]);
            }
        }

        const double divisor = matrix[column][column];
        for (int k = column; k < 4; ++k) {
            matrix[column][k] /= divisor;
        }
        for (int row = 0; row < 3; ++row) {
            if (row == column) {
                continue;
            }
            const double factor = matrix[row][column];
            for (int k = column; k < 4; ++k) {
                matrix[row][k] -= factor * matrix[column][k];
            }
        }
    }

    out[0] = matrix[0][3];
    out[1] = matrix[1][3];
    out[2] = matrix[2][3];
    return true;
}

bool solveLinearSystem(std::vector<std::vector<double>>& matrix, std::vector<double>& out) {
    const std::size_t size = matrix.size();
    if (size == 0) {
        return false;
    }
    for (const auto& row : matrix) {
        if (row.size() != size + 1) {
            return false;
        }
    }

    for (std::size_t column = 0; column < size; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < size; ++row) {
            if (std::fabs(matrix[row][column]) > std::fabs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (std::fabs(matrix[pivot][column]) < 1.0e-12) {
            return false;
        }
        if (pivot != column) {
            std::swap(matrix[column], matrix[pivot]);
        }

        const double divisor = matrix[column][column];
        for (std::size_t k = column; k <= size; ++k) {
            matrix[column][k] /= divisor;
        }
        for (std::size_t row = 0; row < size; ++row) {
            if (row == column) {
                continue;
            }
            const double factor = matrix[row][column];
            for (std::size_t k = column; k <= size; ++k) {
                matrix[row][k] -= factor * matrix[column][k];
            }
        }
    }

    out.assign(size, 0.0);
    for (std::size_t row = 0; row < size; ++row) {
        out[row] = matrix[row][size];
    }
    return true;
}

float affineResidual(const MatchedStarPair& pair, const AffineTransform& transform) {
    const float x = transform.a * pair.moving.x + transform.b * pair.moving.y + transform.dx;
    const float y = transform.c * pair.moving.x + transform.d * pair.moving.y + transform.dy;
    const float residualX = x - pair.reference.x;
    const float residualY = y - pair.reference.y;
    return std::sqrt(residualX * residualX + residualY * residualY);
}

float similarityResidual(const MatchedStarPair& pair, const SimilarityTransform& transform) {
    const float cosTheta = std::cos(transform.rotationRadians);
    const float sinTheta = std::sin(transform.rotationRadians);
    const float x = transform.scale * (cosTheta * pair.moving.x - sinTheta * pair.moving.y) + transform.dx;
    const float y = transform.scale * (sinTheta * pair.moving.x + cosTheta * pair.moving.y) + transform.dy;
    const float residualX = x - pair.reference.x;
    const float residualY = y - pair.reference.y;
    return std::sqrt(residualX * residualX + residualY * residualY);
}

Star applySimilarityToStar(const Star& star, SimilarityTransform transform) {
    const float cosTheta = std::cos(transform.rotationRadians);
    const float sinTheta = std::sin(transform.rotationRadians);
    return {
        .x = transform.scale * (cosTheta * star.x - sinTheta * star.y) + transform.dx,
        .y = transform.scale * (sinTheta * star.x + cosTheta * star.y) + transform.dy,
        .flux = star.flux,
        .peak = star.peak,
        .fwhm = star.fwhm,
        .eccentricity = star.eccentricity,
    };
}

std::pair<float, float> inverseSimilarityPoint(float x, float y, SimilarityTransform transform) {
    const float scale = std::max(1.0e-6F, transform.scale);
    const float cosTheta = std::cos(transform.rotationRadians);
    const float sinTheta = std::sin(transform.rotationRadians);
    const float targetX = x - transform.dx;
    const float targetY = y - transform.dy;
    return {
        (cosTheta * targetX + sinTheta * targetY) / scale,
        (-sinTheta * targetX + cosTheta * targetY) / scale,
    };
}

std::pair<float, float> applyProjectivePoint(float x, float y, ProjectiveTransform transform) {
    const double denominator =
        static_cast<double>(transform.h20) * x + static_cast<double>(transform.h21) * y + transform.h22;
    if (std::fabs(denominator) < 1.0e-9) {
        const float quietNaN = std::numeric_limits<float>::quiet_NaN();
        return {quietNaN, quietNaN};
    }
    return {
        static_cast<float>((static_cast<double>(transform.h00) * x + static_cast<double>(transform.h01) * y +
                            transform.h02) /
                           denominator),
        static_cast<float>((static_cast<double>(transform.h10) * x + static_cast<double>(transform.h11) * y +
                            transform.h12) /
                           denominator),
    };
}

Star applyProjectiveToStar(const Star& star, ProjectiveTransform transform) {
    const auto [x, y] = applyProjectivePoint(star.x, star.y, transform);
    return {
        .x = x,
        .y = y,
        .flux = star.flux,
        .peak = star.peak,
        .fwhm = star.fwhm,
        .eccentricity = star.eccentricity,
    };
}

float projectiveResidual(const MatchedStarPair& pair, const ProjectiveTransform& transform) {
    const auto [x, y] = applyProjectivePoint(pair.moving.x, pair.moving.y, transform);
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return std::numeric_limits<float>::max();
    }
    const float residualX = x - pair.reference.x;
    const float residualY = y - pair.reference.y;
    return std::sqrt(residualX * residualX + residualY * residualY);
}

std::pair<float, float> inverseProjectivePoint(float x, float y, ProjectiveTransform transform) {
    const double h00 = transform.h00;
    const double h01 = transform.h01;
    const double h02 = transform.h02;
    const double h10 = transform.h10;
    const double h11 = transform.h11;
    const double h12 = transform.h12;
    const double h20 = transform.h20;
    const double h21 = transform.h21;
    const double h22 = transform.h22;

    const double c00 = h11 * h22 - h12 * h21;
    const double c01 = h02 * h21 - h01 * h22;
    const double c02 = h01 * h12 - h02 * h11;
    const double c10 = h12 * h20 - h10 * h22;
    const double c11 = h00 * h22 - h02 * h20;
    const double c12 = h02 * h10 - h00 * h12;
    const double c20 = h10 * h21 - h11 * h20;
    const double c21 = h01 * h20 - h00 * h21;
    const double c22 = h00 * h11 - h01 * h10;
    const double determinant = h00 * c00 + h01 * c10 + h02 * c20;
    if (std::fabs(determinant) < 1.0e-12) {
        return {x, y};
    }

    const double inv00 = c00 / determinant;
    const double inv01 = c01 / determinant;
    const double inv02 = c02 / determinant;
    const double inv10 = c10 / determinant;
    const double inv11 = c11 / determinant;
    const double inv12 = c12 / determinant;
    const double inv20 = c20 / determinant;
    const double inv21 = c21 / determinant;
    const double inv22 = c22 / determinant;
    const double denominator = inv20 * x + inv21 * y + inv22;
    if (std::fabs(denominator) < 1.0e-9) {
        return {x, y};
    }
    return {
        static_cast<float>((inv00 * x + inv01 * y + inv02) / denominator),
        static_cast<float>((inv10 * x + inv11 * y + inv12) / denominator),
    };
}

void multiply3x3(const double left[3][3], const double right[3][3], double output[3][3]) {
    double result[3][3] = {};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            for (int k = 0; k < 3; ++k) {
                result[row][column] += left[row][k] * right[k][column];
            }
        }
    }
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            output[row][column] = result[row][column];
        }
    }
}

std::array<double, DistortionTransform::polynomialTermCount> polynomialTerms(float x, float y,
                                                                             std::uint32_t width,
                                                                             std::uint32_t height) {
    const double centerX = static_cast<double>(width - 1) * 0.5;
    const double centerY = static_cast<double>(height - 1) * 0.5;
    const double normalization = std::max<double>(1.0, std::max(width, height));
    const double u = (static_cast<double>(x) - centerX) / normalization;
    const double v = (static_cast<double>(y) - centerY) / normalization;
    return {
        1.0,
        u,
        v,
        u * u,
        u * v,
        v * v,
        u * u * u,
        u * u * v,
        u * v * v,
        v * v * v,
    };
}

std::pair<float, float> evaluatePolynomialCorrection(
    float x, float y, std::uint32_t width, std::uint32_t height,
    const std::array<float, DistortionTransform::polynomialTermCount>& coefficientsX,
    const std::array<float, DistortionTransform::polynomialTermCount>& coefficientsY) {
    const auto terms = polynomialTerms(x, y, width, height);
    double dx = 0.0;
    double dy = 0.0;
    for (std::size_t index = 0; index < terms.size(); ++index) {
        dx += static_cast<double>(coefficientsX[index]) * terms[index];
        dy += static_cast<double>(coefficientsY[index]) * terms[index];
    }
    return {static_cast<float>(dx), static_cast<float>(dy)};
}

SimilarityFit fitSimilarityTransform(const std::vector<MatchedStarPair>& pairs, const std::vector<std::size_t>& indices) {
    if (indices.size() < 2) {
        return {};
    }

    float movingCenterX = 0.0F;
    float movingCenterY = 0.0F;
    float referenceCenterX = 0.0F;
    float referenceCenterY = 0.0F;
    for (const auto index : indices) {
        const auto& pair = pairs[index];
        movingCenterX += pair.moving.x;
        movingCenterY += pair.moving.y;
        referenceCenterX += pair.reference.x;
        referenceCenterY += pair.reference.y;
    }
    const float invCount = 1.0F / static_cast<float>(indices.size());
    movingCenterX *= invCount;
    movingCenterY *= invCount;
    referenceCenterX *= invCount;
    referenceCenterY *= invCount;

    float dot = 0.0F;
    float cross = 0.0F;
    float movingEnergy = 0.0F;
    for (const auto index : indices) {
        const auto& pair = pairs[index];
        const float mx = pair.moving.x - movingCenterX;
        const float my = pair.moving.y - movingCenterY;
        const float rx = pair.reference.x - referenceCenterX;
        const float ry = pair.reference.y - referenceCenterY;
        dot += mx * rx + my * ry;
        cross += mx * ry - my * rx;
        movingEnergy += mx * mx + my * my;
    }
    if (movingEnergy <= 0.0F) {
        return {};
    }

    const float scale = std::sqrt(dot * dot + cross * cross) / movingEnergy;
    const float rotation = std::atan2(cross, dot);
    const float cosTheta = std::cos(rotation);
    const float sinTheta = std::sin(rotation);
    const float dx = referenceCenterX - scale * (cosTheta * movingCenterX - sinTheta * movingCenterY);
    const float dy = referenceCenterY - scale * (sinTheta * movingCenterX + cosTheta * movingCenterY);

    SimilarityFit fit;
    fit.ok = true;
    fit.transform = {.scale = scale, .rotationRadians = rotation, .dx = dx, .dy = dy};
    fit.affine = {
        .a = scale * cosTheta,
        .b = -scale * sinTheta,
        .c = scale * sinTheta,
        .d = scale * cosTheta,
        .dx = dx,
        .dy = dy,
    };

    double residualSum = 0.0;
    for (const auto index : indices) {
        const float residual = similarityResidual(pairs[index], fit.transform);
        residualSum += static_cast<double>(residual) * residual;
    }
    fit.rms = std::sqrt(residualSum / static_cast<double>(indices.size()));
    return fit;
}

AffineFit fitAffineTransform(const std::vector<MatchedStarPair>& pairs, const std::vector<std::size_t>& indices) {
    if (indices.size() < 3) {
        return {};
    }

    double normal[3][3] = {};
    double rhsX[3] = {};
    double rhsY[3] = {};
    for (const auto index : indices) {
        const auto& pair = pairs[index];
        const double terms[3] = {pair.moving.x, pair.moving.y, 1.0};
        for (int row = 0; row < 3; ++row) {
            rhsX[row] += terms[row] * pair.reference.x;
            rhsY[row] += terms[row] * pair.reference.y;
            for (int column = 0; column < 3; ++column) {
                normal[row][column] += terms[row] * terms[column];
            }
        }
    }

    double matrixX[3][4] = {};
    double matrixY[3][4] = {};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            matrixX[row][column] = normal[row][column];
            matrixY[row][column] = normal[row][column];
        }
        matrixX[row][3] = rhsX[row];
        matrixY[row][3] = rhsY[row];
    }

    double solutionX[3] = {};
    double solutionY[3] = {};
    if (!solve3x3(matrixX, solutionX) || !solve3x3(matrixY, solutionY)) {
        return {};
    }

    AffineFit fit;
    fit.ok = true;
    fit.transform = {
        .a = static_cast<float>(solutionX[0]),
        .b = static_cast<float>(solutionX[1]),
        .c = static_cast<float>(solutionY[0]),
        .d = static_cast<float>(solutionY[1]),
        .dx = static_cast<float>(solutionX[2]),
        .dy = static_cast<float>(solutionY[2]),
    };

    double residualSum = 0.0;
    for (const auto index : indices) {
        const float residual = affineResidual(pairs[index], fit.transform);
        residualSum += static_cast<double>(residual) * residual;
    }
    fit.rms = std::sqrt(residualSum / static_cast<double>(indices.size()));
    return fit;
}

bool plausibleAffineTransform(const AffineTransform& transform) {
    const float determinant = transform.a * transform.d - transform.b * transform.c;
    const float scaleX = std::hypot(transform.a, transform.c);
    const float scaleY = std::hypot(transform.b, transform.d);
    const float scaleRatio = std::max(scaleX, scaleY) / std::max(1.0e-6F, std::min(scaleX, scaleY));
    const float normalizedDot = (transform.a * transform.b + transform.c * transform.d) /
                                std::max(1.0e-6F, scaleX * scaleY);
    return std::isfinite(determinant) && std::isfinite(scaleX) && std::isfinite(scaleY) &&
           determinant > 0.0F && scaleX >= 0.25F && scaleX <= 4.0F &&
           scaleY >= 0.25F && scaleY <= 4.0F && scaleRatio <= 2.0F &&
           std::fabs(normalizedDot) <= 0.80F;
}

AffineFit fitAffineTransformRansac(
    const std::vector<MatchedStarPair>& pairs,
    std::size_t requiredMatches,
    float matchTolerance,
    std::vector<std::size_t>& inliers
) {
    if (pairs.size() < std::max<std::size_t>(3, requiredMatches)) {
        inliers.clear();
        return {};
    }

    const float residualLimit = std::max(2.0F, matchTolerance);
    std::vector<std::size_t> bestInliers;
    float bestRms = std::numeric_limits<float>::max();
    std::uint32_t state = 0x9e3779b9U ^ static_cast<std::uint32_t>(pairs.size());
    const auto nextIndex = [&]() {
        state = state * 1664525U + 1013904223U;
        return static_cast<std::size_t>(state % static_cast<std::uint32_t>(pairs.size()));
    };
    const std::size_t iterations = std::min<std::size_t>(1200, std::max<std::size_t>(240, pairs.size() * 8));
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        std::array<std::size_t, 3> sample = {nextIndex(), nextIndex(), nextIndex()};
        if (sample[0] == sample[1] || sample[0] == sample[2] || sample[1] == sample[2]) {
            continue;
        }
        const std::vector<std::size_t> sampleIndices(sample.begin(), sample.end());
        const auto candidate = fitAffineTransform(pairs, sampleIndices);
        if (!candidate.ok || !plausibleAffineTransform(candidate.transform)) {
            continue;
        }

        std::vector<std::size_t> candidateInliers;
        candidateInliers.reserve(pairs.size());
        double residualSum = 0.0;
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            const float residual = affineResidual(pairs[index], candidate.transform);
            if (residual <= residualLimit) {
                candidateInliers.push_back(index);
                residualSum += static_cast<double>(residual) * residual;
            }
        }
        if (candidateInliers.size() < std::max<std::size_t>(3, requiredMatches)) {
            continue;
        }
        const float rms = static_cast<float>(std::sqrt(residualSum / static_cast<double>(candidateInliers.size())));
        if (candidateInliers.size() > bestInliers.size() ||
            (candidateInliers.size() == bestInliers.size() && rms < bestRms)) {
            bestInliers = std::move(candidateInliers);
            bestRms = rms;
        }
    }

    if (bestInliers.size() < std::max<std::size_t>(3, requiredMatches)) {
        inliers.clear();
        return {};
    }
    auto fit = fitAffineTransform(pairs, bestInliers);
    if (!fit.ok || !plausibleAffineTransform(fit.transform)) {
        inliers.clear();
        return {};
    }

    for (int iteration = 0; iteration < 4; ++iteration) {
        const float refinedLimit = std::max({residualLimit, fit.rms * 2.0F, 2.0F});
        std::vector<std::size_t> refined;
        refined.reserve(pairs.size());
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            if (affineResidual(pairs[index], fit.transform) <= refinedLimit) {
                refined.push_back(index);
            }
        }
        if (refined.size() < std::max<std::size_t>(3, requiredMatches) || refined == bestInliers) {
            break;
        }
        bestInliers = std::move(refined);
        fit = fitAffineTransform(pairs, bestInliers);
        if (!fit.ok || !plausibleAffineTransform(fit.transform)) {
            inliers.clear();
            return {};
        }
    }

    inliers = std::move(bestInliers);
    return fitAffineTransform(pairs, inliers);
}

ProjectiveFit fitProjectiveTransform(const std::vector<MatchedStarPair>& pairs,
                                     const std::vector<std::size_t>& indices) {
    if (indices.size() < 4) {
        return {};
    }

    double movingCenterX = 0.0;
    double movingCenterY = 0.0;
    double referenceCenterX = 0.0;
    double referenceCenterY = 0.0;
    for (const auto index : indices) {
        const auto& pair = pairs[index];
        movingCenterX += pair.moving.x;
        movingCenterY += pair.moving.y;
        referenceCenterX += pair.reference.x;
        referenceCenterY += pair.reference.y;
    }
    const double invCount = 1.0 / static_cast<double>(indices.size());
    movingCenterX *= invCount;
    movingCenterY *= invCount;
    referenceCenterX *= invCount;
    referenceCenterY *= invCount;

    double movingDistance = 0.0;
    double referenceDistance = 0.0;
    for (const auto index : indices) {
        const auto& pair = pairs[index];
        const double movingDx = pair.moving.x - movingCenterX;
        const double movingDy = pair.moving.y - movingCenterY;
        const double referenceDx = pair.reference.x - referenceCenterX;
        const double referenceDy = pair.reference.y - referenceCenterY;
        movingDistance += std::sqrt(movingDx * movingDx + movingDy * movingDy);
        referenceDistance += std::sqrt(referenceDx * referenceDx + referenceDy * referenceDy);
    }
    movingDistance *= invCount;
    referenceDistance *= invCount;
    if (movingDistance <= 1.0e-6 || referenceDistance <= 1.0e-6) {
        return {};
    }

    const double rootTwo = std::sqrt(2.0);
    const double movingScale = rootTwo / movingDistance;
    const double referenceScale = rootTwo / referenceDistance;
    std::vector<std::vector<double>> matrix(8, std::vector<double>(9, 0.0));
    const auto accumulate = [&](const double terms[8], double rhs) {
        for (int row = 0; row < 8; ++row) {
            matrix[static_cast<std::size_t>(row)][8] += terms[row] * rhs;
            for (int column = 0; column < 8; ++column) {
                matrix[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] +=
                    terms[row] * terms[column];
            }
        }
    };

    for (const auto index : indices) {
        const auto& pair = pairs[index];
        const double x = (pair.moving.x - movingCenterX) * movingScale;
        const double y = (pair.moving.y - movingCenterY) * movingScale;
        const double u = (pair.reference.x - referenceCenterX) * referenceScale;
        const double v = (pair.reference.y - referenceCenterY) * referenceScale;
        const double xTerms[8] = {x, y, 1.0, 0.0, 0.0, 0.0, -u * x, -u * y};
        const double yTerms[8] = {0.0, 0.0, 0.0, x, y, 1.0, -v * x, -v * y};
        accumulate(xTerms, u);
        accumulate(yTerms, v);
    }
    for (int diagonal = 0; diagonal < 8; ++diagonal) {
        matrix[static_cast<std::size_t>(diagonal)][static_cast<std::size_t>(diagonal)] += 1.0e-10;
    }

    std::vector<double> solution;
    if (!solveLinearSystem(matrix, solution) || solution.size() != 8) {
        return {};
    }

    const double normalizedH[3][3] = {
        {solution[0], solution[1], solution[2]},
        {solution[3], solution[4], solution[5]},
        {solution[6], solution[7], 1.0},
    };
    const double sourceNormalization[3][3] = {
        {movingScale, 0.0, -movingScale * movingCenterX},
        {0.0, movingScale, -movingScale * movingCenterY},
        {0.0, 0.0, 1.0},
    };
    const double targetDenormalization[3][3] = {
        {1.0 / referenceScale, 0.0, referenceCenterX},
        {0.0, 1.0 / referenceScale, referenceCenterY},
        {0.0, 0.0, 1.0},
    };
    double normalizedTimesSource[3][3] = {};
    double pixelH[3][3] = {};
    multiply3x3(normalizedH, sourceNormalization, normalizedTimesSource);
    multiply3x3(targetDenormalization, normalizedTimesSource, pixelH);

    const double normalizer = std::fabs(pixelH[2][2]) > 1.0e-12 ? pixelH[2][2] : 1.0;
    ProjectiveTransform transform = {
        .h00 = static_cast<float>(pixelH[0][0] / normalizer),
        .h01 = static_cast<float>(pixelH[0][1] / normalizer),
        .h02 = static_cast<float>(pixelH[0][2] / normalizer),
        .h10 = static_cast<float>(pixelH[1][0] / normalizer),
        .h11 = static_cast<float>(pixelH[1][1] / normalizer),
        .h12 = static_cast<float>(pixelH[1][2] / normalizer),
        .h20 = static_cast<float>(pixelH[2][0] / normalizer),
        .h21 = static_cast<float>(pixelH[2][1] / normalizer),
        .h22 = static_cast<float>(pixelH[2][2] / normalizer),
    };

    double residualSum = 0.0;
    std::size_t finiteResiduals = 0;
    for (const auto index : indices) {
        const float residual = projectiveResidual(pairs[index], transform);
        if (!std::isfinite(residual)) {
            continue;
        }
        residualSum += static_cast<double>(residual) * residual;
        ++finiteResiduals;
    }
    if (finiteResiduals < 4) {
        return {};
    }

    ProjectiveFit fit;
    fit.ok = true;
    fit.transform = transform;
    fit.rms = static_cast<float>(std::sqrt(residualSum / static_cast<double>(finiteResiduals)));
    return fit;
}

ProjectiveFit fitProjectiveTransformRobust(const std::vector<MatchedStarPair>& pairs, std::size_t requiredMatches,
                                           float matchTolerance, std::vector<std::size_t>& inliers) {
    inliers.resize(pairs.size());
    for (std::size_t index = 0; index < inliers.size(); ++index) {
        inliers[index] = index;
    }
    if (inliers.size() < requiredMatches) {
        return {};
    }

    ProjectiveFit fit;
    for (int iteration = 0; iteration < 6; ++iteration) {
        fit = fitProjectiveTransform(pairs, inliers);
        if (!fit.ok) {
            break;
        }

        const float residualLimit = std::max({matchTolerance * 4.0F, fit.rms * 2.2F, 4.0F});
        std::vector<std::size_t> nextInliers;
        nextInliers.reserve(pairs.size());
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            if (projectiveResidual(pairs[index], fit.transform) <= residualLimit) {
                nextInliers.push_back(index);
            }
        }
        if (nextInliers.size() < requiredMatches || nextInliers.size() == inliers.size()) {
            break;
        }
        inliers = std::move(nextInliers);
    }

    fit = fitProjectiveTransform(pairs, inliers);
    if (!fit.ok || inliers.size() < requiredMatches) {
        inliers.clear();
        return {};
    }
    return fit;
}

PolynomialCorrectionFit fitPolynomialCorrection(const std::vector<MatchedStarPair>& pairs,
                                                const std::vector<std::size_t>& indices,
                                                const DistortionTransform& transform,
                                                std::uint32_t width,
                                                std::uint32_t height) {
    if (indices.size() < kPolynomialFitTermCount) {
        return {};
    }

    std::vector<std::vector<double>> matrixX(kPolynomialFitTermCount, std::vector<double>(kPolynomialFitTermCount + 1, 0.0));
    std::vector<std::vector<double>> matrixY(kPolynomialFitTermCount, std::vector<double>(kPolynomialFitTermCount + 1, 0.0));
    const auto baseSource = [&](const MatchedStarPair& pair) {
        return transform.useProjective ? inverseProjectivePoint(pair.reference.x, pair.reference.y, transform.projective)
                                       : inverseSimilarityPoint(pair.reference.x, pair.reference.y, transform.global);
    };

    for (const auto index : indices) {
        const auto& pair = pairs[index];
        const auto [baseSourceX, baseSourceY] = baseSource(pair);
        const double targetDx = static_cast<double>(pair.moving.x - baseSourceX);
        const double targetDy = static_cast<double>(pair.moving.y - baseSourceY);
        const auto terms = polynomialTerms(pair.reference.x, pair.reference.y, width, height);
        for (std::size_t row = 0; row < kPolynomialFitTermCount; ++row) {
            matrixX[row][kPolynomialFitTermCount] += terms[row] * targetDx;
            matrixY[row][kPolynomialFitTermCount] += terms[row] * targetDy;
            for (std::size_t column = 0; column < kPolynomialFitTermCount; ++column) {
                matrixX[row][column] += terms[row] * terms[column];
                matrixY[row][column] += terms[row] * terms[column];
            }
        }
    }

    const double regularization = std::max(1.0e-8, static_cast<double>(indices.size()) * 1.0e-7);
    for (std::size_t diagonal = 0; diagonal < kPolynomialFitTermCount; ++diagonal) {
        const double scale = diagonal == 0 ? regularization * 0.01 : regularization;
        matrixX[diagonal][diagonal] += scale;
        matrixY[diagonal][diagonal] += scale;
    }

    std::vector<double> solutionX;
    std::vector<double> solutionY;
    if (!solveLinearSystem(matrixX, solutionX) || !solveLinearSystem(matrixY, solutionY) ||
        solutionX.size() != kPolynomialFitTermCount || solutionY.size() != kPolynomialFitTermCount) {
        return {};
    }

    PolynomialCorrectionFit fit;
    fit.ok = true;
    for (std::size_t index = 0; index < kPolynomialFitTermCount; ++index) {
        fit.dx[index] = static_cast<float>(solutionX[index]);
        fit.dy[index] = static_cast<float>(solutionY[index]);
    }

    double residualSum = 0.0;
    for (const auto index : indices) {
        const auto& pair = pairs[index];
        const auto [baseSourceX, baseSourceY] = baseSource(pair);
        const auto [correctionX, correctionY] =
            evaluatePolynomialCorrection(pair.reference.x, pair.reference.y, width, height, fit.dx, fit.dy);
        const double residualX = static_cast<double>(baseSourceX + correctionX - pair.moving.x);
        const double residualY = static_cast<double>(baseSourceY + correctionY - pair.moving.y);
        residualSum += residualX * residualX + residualY * residualY;
    }
    fit.rms = static_cast<float>(std::sqrt(residualSum / static_cast<double>(indices.size())));
    return fit;
}

PolynomialCorrectionFit fitPolynomialCorrectionRobust(const std::vector<MatchedStarPair>& pairs,
                                                      const DistortionTransform& transform,
                                                      std::uint32_t width,
                                                      std::uint32_t height,
                                                      std::size_t requiredMatches,
                                                      float matchTolerance,
                                                      std::vector<std::size_t>& inliers) {
    inliers.resize(pairs.size());
    for (std::size_t index = 0; index < inliers.size(); ++index) {
        inliers[index] = index;
    }
    if (inliers.size() < requiredMatches) {
        return {};
    }

    PolynomialCorrectionFit fit;
    for (int iteration = 0; iteration < 5; ++iteration) {
        fit = fitPolynomialCorrection(pairs, inliers, transform, width, height);
        if (!fit.ok) {
            break;
        }

        const float residualLimit = std::max({matchTolerance * 2.0F, fit.rms * 2.2F, 3.0F});
        std::vector<std::size_t> nextInliers;
        nextInliers.reserve(pairs.size());
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            const auto& pair = pairs[index];
            const auto [baseSourceX, baseSourceY] =
                transform.useProjective ? inverseProjectivePoint(pair.reference.x, pair.reference.y, transform.projective)
                                        : inverseSimilarityPoint(pair.reference.x, pair.reference.y, transform.global);
            const auto [correctionX, correctionY] =
                evaluatePolynomialCorrection(pair.reference.x, pair.reference.y, width, height, fit.dx, fit.dy);
            const float residualX = baseSourceX + correctionX - pair.moving.x;
            const float residualY = baseSourceY + correctionY - pair.moving.y;
            if (residualX * residualX + residualY * residualY <= residualLimit * residualLimit) {
                nextInliers.push_back(index);
            }
        }
        if (nextInliers.size() < requiredMatches || nextInliers.size() == inliers.size()) {
            break;
        }
        inliers = std::move(nextInliers);
    }

    fit = fitPolynomialCorrection(pairs, inliers, transform, width, height);
    if (!fit.ok || inliers.size() < requiredMatches) {
        inliers.clear();
        return {};
    }
    return fit;
}

void populateSimilaritySummary(RegistrationResult& result, const AffineTransform& affine) {
    const float sx = std::sqrt(affine.a * affine.a + affine.c * affine.c);
    const float sy = std::sqrt(affine.b * affine.b + affine.d * affine.d);
    result.translation = {.dx = affine.dx, .dy = affine.dy};
    result.transform = {
        .scale = (sx + sy) * 0.5F,
        .rotationRadians = std::atan2(affine.c - affine.b, affine.a + affine.d),
        .dx = affine.dx,
        .dy = affine.dy,
    };
}

RegistrationResult detectMatchedPairs(const ImageBuffer& reference, const ImageBuffer& moving,
                                      const RegistrationOptions& options, Translation prealign,
                                      std::vector<MatchedStarPair>& pairs) {
    if (!validImageStructure(reference) || !validImageStructure(moving) ||
        reference.channels != moving.channels) {
        return registrationError("ImageBufferInvalid",
                                 "Reference and moving images must be finite matching buffers");
    }

    const StarDetector detector;
    const auto referenceStars = detector.detect(reference, options.starDetection);
    if (!referenceStars.ok) {
        return registrationError(referenceStars.errorCode, referenceStars.message);
    }
    const auto movingStars = detector.detect(moving, options.starDetection);
    if (!movingStars.ok) {
        return registrationError(movingStars.errorCode, movingStars.message);
    }
    if (referenceStars.stars.empty() || movingStars.stars.empty()) {
        return registrationError("RegistrationInsufficientStars", "Both images need at least one detected star");
    }

    pairs.clear();
    std::vector<int> referenceAssignments(referenceStars.stars.size(), -1);
    for (std::size_t movingIndex = 0; movingIndex < movingStars.stars.size(); ++movingIndex) {
        const auto& movingStar = movingStars.stars[movingIndex];
        std::size_t bestReferenceIndex = 0;
        float bestDistance = std::numeric_limits<float>::max();
        const Star projectedMoving = {
            .x = movingStar.x + prealign.dx,
            .y = movingStar.y + prealign.dy,
        };
        for (std::size_t referenceIndex = 0; referenceIndex < referenceStars.stars.size(); ++referenceIndex) {
            const auto& referenceStar = referenceStars.stars[referenceIndex];
            const float candidateDistance = distanceSquared(referenceStar, projectedMoving);
            if (candidateDistance < bestDistance) {
                bestDistance = candidateDistance;
                bestReferenceIndex = referenceIndex;
            }
        }

        if (bestDistance > options.matchTolerance * options.matchTolerance) {
            continue;
        }

        const auto& bestReference = referenceStars.stars[bestReferenceIndex];
        std::size_t reciprocalMovingIndex = 0;
        float reciprocalDistance = std::numeric_limits<float>::max();
        for (std::size_t candidateMovingIndex = 0; candidateMovingIndex < movingStars.stars.size(); ++candidateMovingIndex) {
            const auto& candidateMoving = movingStars.stars[candidateMovingIndex];
            const Star projectedCandidate = {
                .x = candidateMoving.x + prealign.dx,
                .y = candidateMoving.y + prealign.dy,
            };
            const float candidateDistance = distanceSquared(bestReference, projectedCandidate);
            if (candidateDistance < reciprocalDistance) {
                reciprocalDistance = candidateDistance;
                reciprocalMovingIndex = candidateMovingIndex;
            }
        }
        if (reciprocalMovingIndex != movingIndex) {
            continue;
        }

        const int currentAssignment = referenceAssignments[bestReferenceIndex];
        if (currentAssignment < 0) {
            referenceAssignments[bestReferenceIndex] = static_cast<int>(pairs.size());
            pairs.push_back({
                .reference = bestReference,
                .moving = movingStar,
            });
            continue;
        }

        const float assignedDistance = distanceSquared(
            pairs[static_cast<std::size_t>(currentAssignment)].reference,
            {
                .x = pairs[static_cast<std::size_t>(currentAssignment)].moving.x + prealign.dx,
                .y = pairs[static_cast<std::size_t>(currentAssignment)].moving.y + prealign.dy,
            });
        if (bestDistance < assignedDistance) {
            pairs[static_cast<std::size_t>(currentAssignment)] = {
                .reference = bestReference,
                .moving = movingStar,
            };
        }
    }

    if (pairs.empty()) {
        return registrationError("RegistrationNoMatches", "No matching stars were found");
    }

    RegistrationResult result;
    result.ok = true;
    result.matches = pairs.size();
    result.detectedReferenceStars = referenceStars.stars.size();
    result.detectedMovingStars = movingStars.stars.size();
    const auto denominator = std::max<std::size_t>(1, std::min(referenceStars.stars.size(), movingStars.stars.size()));
    result.inlierRatio = static_cast<float>(pairs.size()) / static_cast<float>(denominator);
    return result;
}

std::vector<StarDescriptor> buildStarDescriptors(const std::vector<Star>& stars, std::size_t neighborCount) {
    std::vector<StarDescriptor> descriptors;
    descriptors.reserve(stars.size());
    for (std::size_t index = 0; index < stars.size(); ++index) {
        std::vector<float> distances;
        distances.reserve(stars.size() > 0 ? stars.size() - 1 : 0);
        for (std::size_t other = 0; other < stars.size(); ++other) {
            if (other == index) {
                continue;
            }
            distances.push_back(std::sqrt(distanceSquared(stars[index], stars[other])));
        }
        if (distances.size() < neighborCount) {
            continue;
        }
        std::sort(distances.begin(), distances.end());
        distances.resize(neighborCount);
        const float normalizer = std::max(1.0e-6F, distances.back());
        for (auto& distance : distances) {
            distance /= normalizer;
        }
        descriptors.push_back({.index = index, .distances = std::move(distances)});
    }
    return descriptors;
}

float descriptorScore(const StarDescriptor& left, const StarDescriptor& right) {
    if (left.distances.size() != right.distances.size() || left.distances.empty()) {
        return std::numeric_limits<float>::max();
    }
    float score = 0.0F;
    for (std::size_t index = 0; index < left.distances.size(); ++index) {
        score += std::fabs(left.distances[index] - right.distances[index]);
    }
    return score / static_cast<float>(left.distances.size());
}

RegistrationResult detectDescriptorMatchedPairs(const ImageBuffer& reference, const ImageBuffer& moving,
                                                const RegistrationOptions& options,
                                                std::vector<MatchedStarPair>& pairs) {
    if (!validImageStructure(reference) || !validImageStructure(moving) ||
        reference.channels != moving.channels) {
        return registrationError("ImageBufferInvalid",
                                 "Reference and moving images must be finite matching buffers");
    }

    const StarDetector detector;
    auto referenceStars = detector.detect(reference, options.starDetection);
    if (!referenceStars.ok) {
        return registrationError(referenceStars.errorCode, referenceStars.message);
    }
    auto movingStars = detector.detect(moving, options.starDetection);
    if (!movingStars.ok) {
        return registrationError(movingStars.errorCode, movingStars.message);
    }
    if (referenceStars.stars.size() < 6 || movingStars.stars.size() < 6) {
        return registrationError("RegistrationInsufficientStars", "Descriptor matching needs at least six stars");
    }

    const std::size_t starLimit = std::min<std::size_t>(180, std::min(referenceStars.stars.size(), movingStars.stars.size()));
    referenceStars.stars.resize(std::min(referenceStars.stars.size(), starLimit));
    movingStars.stars.resize(std::min(movingStars.stars.size(), starLimit));

    const std::size_t neighborCount = std::min<std::size_t>(6, starLimit - 1);
    const auto referenceDescriptors = buildStarDescriptors(referenceStars.stars, neighborCount);
    const auto movingDescriptors = buildStarDescriptors(movingStars.stars, neighborCount);
    if (referenceDescriptors.empty() || movingDescriptors.empty()) {
        return registrationError("RegistrationInsufficientStars", "Descriptor matching could not build star neighborhoods");
    }

    std::vector<int> movingBestReference(movingDescriptors.size(), -1);
    std::vector<float> movingBestScore(movingDescriptors.size(), std::numeric_limits<float>::max());
    std::vector<int> referenceBestMoving(referenceDescriptors.size(), -1);
    std::vector<float> referenceBestScore(referenceDescriptors.size(), std::numeric_limits<float>::max());

    for (std::size_t movingIndex = 0; movingIndex < movingDescriptors.size(); ++movingIndex) {
        for (std::size_t referenceIndex = 0; referenceIndex < referenceDescriptors.size(); ++referenceIndex) {
            const float score = descriptorScore(movingDescriptors[movingIndex], referenceDescriptors[referenceIndex]);
            if (score < movingBestScore[movingIndex]) {
                movingBestScore[movingIndex] = score;
                movingBestReference[movingIndex] = static_cast<int>(referenceIndex);
            }
            if (score < referenceBestScore[referenceIndex]) {
                referenceBestScore[referenceIndex] = score;
                referenceBestMoving[referenceIndex] = static_cast<int>(movingIndex);
            }
        }
    }

    pairs.clear();
    for (std::size_t movingIndex = 0; movingIndex < movingDescriptors.size(); ++movingIndex) {
        const int referenceIndex = movingBestReference[movingIndex];
        if (referenceIndex < 0 || referenceBestMoving[static_cast<std::size_t>(referenceIndex)] != static_cast<int>(movingIndex)) {
            continue;
        }
        if (movingBestScore[movingIndex] > 0.055F) {
            continue;
        }
        const auto& referenceDescriptor = referenceDescriptors[static_cast<std::size_t>(referenceIndex)];
        const auto& movingDescriptor = movingDescriptors[movingIndex];
        pairs.push_back({
            .reference = referenceStars.stars[referenceDescriptor.index],
            .moving = movingStars.stars[movingDescriptor.index],
        });
    }

    if (pairs.empty()) {
        return registrationError("RegistrationNoMatches", "No descriptor-matched stars were found");
    }

    RegistrationResult result;
    result.ok = true;
    result.matches = pairs.size();
    result.detectedReferenceStars = referenceStars.stars.size();
    result.detectedMovingStars = movingStars.stars.size();
    const auto denominator = std::max<std::size_t>(1, std::min(referenceStars.stars.size(), movingStars.stars.size()));
    result.inlierRatio = static_cast<float>(pairs.size()) / static_cast<float>(denominator);
    return result;
}

RegistrationResult detectSimilarityProjectedPairs(const ImageBuffer& reference, const ImageBuffer& moving,
                                                  const RegistrationOptions& options, SimilarityTransform transform,
                                                  std::vector<MatchedStarPair>& pairs) {
    if (!validImageStructure(reference) || !validImageStructure(moving) ||
        reference.channels != moving.channels) {
        return registrationError("ImageBufferInvalid",
                                 "Reference and moving images must be finite matching buffers");
    }

    const StarDetector detector;
    const auto referenceStars = detector.detect(reference, options.starDetection);
    if (!referenceStars.ok) {
        return registrationError(referenceStars.errorCode, referenceStars.message);
    }
    const auto movingStars = detector.detect(moving, options.starDetection);
    if (!movingStars.ok) {
        return registrationError(movingStars.errorCode, movingStars.message);
    }
    if (referenceStars.stars.empty() || movingStars.stars.empty()) {
        return registrationError("RegistrationInsufficientStars", "Both images need at least one detected star");
    }

    const float toleranceSquared = options.matchTolerance * options.matchTolerance;
    std::vector<IndexedPoint> referencePoints;
    referencePoints.reserve(referenceStars.stars.size());
    for (std::size_t referenceIndex = 0; referenceIndex < referenceStars.stars.size(); ++referenceIndex) {
        referencePoints.push_back({
            .index = referenceIndex,
            .x = referenceStars.stars[referenceIndex].x,
            .y = referenceStars.stars[referenceIndex].y,
        });
    }
    std::vector<IndexedPoint> projectedMovingPoints;
    projectedMovingPoints.reserve(movingStars.stars.size());
    for (std::size_t movingIndex = 0; movingIndex < movingStars.stars.size(); ++movingIndex) {
        const auto projected = applySimilarityToStar(movingStars.stars[movingIndex], transform);
        projectedMovingPoints.push_back({.index = movingIndex, .x = projected.x, .y = projected.y});
    }

    const SpatialPointIndex referencePointIndex(
        static_cast<float>(reference.width), static_cast<float>(reference.height), options.matchTolerance, std::move(referencePoints));
    const SpatialPointIndex movingPointIndex(
        static_cast<float>(reference.width), static_cast<float>(reference.height), options.matchTolerance, std::move(projectedMovingPoints));

    pairs.clear();
    std::vector<int> referenceAssignments(referenceStars.stars.size(), -1);
    for (std::size_t movingIndex = 0; movingIndex < movingStars.stars.size(); ++movingIndex) {
        const auto& movingStar = movingStars.stars[movingIndex];
        const auto projectedMoving = applySimilarityToStar(movingStar, transform);
        const auto bestReference = referencePointIndex.nearest(projectedMoving.x, projectedMoving.y, options.matchTolerance);
        const std::size_t bestReferenceIndex = bestReference.index;
        const float bestDistance = bestReference.distanceSquared;
        if (!bestReference.found || bestDistance > toleranceSquared) {
            continue;
        }

        const auto reciprocalMoving = movingPointIndex.nearest(
            referenceStars.stars[bestReferenceIndex].x, referenceStars.stars[bestReferenceIndex].y, options.matchTolerance);
        if (!reciprocalMoving.found || reciprocalMoving.index != movingIndex) {
            continue;
        }

        const int currentAssignment = referenceAssignments[bestReferenceIndex];
        if (currentAssignment < 0) {
            referenceAssignments[bestReferenceIndex] = static_cast<int>(pairs.size());
            pairs.push_back({.reference = referenceStars.stars[bestReferenceIndex], .moving = movingStar});
            continue;
        }

        const auto assignedProjection = applySimilarityToStar(pairs[static_cast<std::size_t>(currentAssignment)].moving, transform);
        const float assignedDistance = distanceSquared(pairs[static_cast<std::size_t>(currentAssignment)].reference, assignedProjection);
        if (bestDistance < assignedDistance) {
            pairs[static_cast<std::size_t>(currentAssignment)] = {
                .reference = referenceStars.stars[bestReferenceIndex],
                .moving = movingStar,
            };
        }
    }

    if (pairs.empty()) {
        return registrationError("RegistrationNoMatches", "No locally matched stars were found");
    }

    RegistrationResult result;
    result.ok = true;
    result.matches = pairs.size();
    result.detectedReferenceStars = referenceStars.stars.size();
    result.detectedMovingStars = movingStars.stars.size();
    const auto denominator = std::max<std::size_t>(1, std::min(referenceStars.stars.size(), movingStars.stars.size()));
    result.inlierRatio = static_cast<float>(pairs.size()) / static_cast<float>(denominator);
    return result;
}

RegistrationResult detectProjectiveProjectedPairs(const ImageBuffer& reference, const ImageBuffer& moving,
                                                  const RegistrationOptions& options, ProjectiveTransform transform,
                                                  std::vector<MatchedStarPair>& pairs) {
    if (!validImageStructure(reference) || !validImageStructure(moving) ||
        reference.channels != moving.channels) {
        return registrationError("ImageBufferInvalid",
                                 "Reference and moving images must be finite matching buffers");
    }

    const StarDetector detector;
    const auto referenceStars = detector.detect(reference, options.starDetection);
    if (!referenceStars.ok) {
        return registrationError(referenceStars.errorCode, referenceStars.message);
    }
    const auto movingStars = detector.detect(moving, options.starDetection);
    if (!movingStars.ok) {
        return registrationError(movingStars.errorCode, movingStars.message);
    }
    if (referenceStars.stars.empty() || movingStars.stars.empty()) {
        return registrationError("RegistrationInsufficientStars", "Both images need at least one detected star");
    }

    const float toleranceSquared = options.matchTolerance * options.matchTolerance;
    std::vector<IndexedPoint> referencePoints;
    referencePoints.reserve(referenceStars.stars.size());
    for (std::size_t referenceIndex = 0; referenceIndex < referenceStars.stars.size(); ++referenceIndex) {
        referencePoints.push_back({
            .index = referenceIndex,
            .x = referenceStars.stars[referenceIndex].x,
            .y = referenceStars.stars[referenceIndex].y,
        });
    }
    std::vector<IndexedPoint> projectedMovingPoints;
    projectedMovingPoints.reserve(movingStars.stars.size());
    for (std::size_t movingIndex = 0; movingIndex < movingStars.stars.size(); ++movingIndex) {
        const auto projected = applyProjectiveToStar(movingStars.stars[movingIndex], transform);
        if (!std::isfinite(projected.x) || !std::isfinite(projected.y)) {
            continue;
        }
        projectedMovingPoints.push_back({.index = movingIndex, .x = projected.x, .y = projected.y});
    }

    const SpatialPointIndex referencePointIndex(
        static_cast<float>(reference.width), static_cast<float>(reference.height), options.matchTolerance, std::move(referencePoints));
    const SpatialPointIndex movingPointIndex(
        static_cast<float>(reference.width), static_cast<float>(reference.height), options.matchTolerance, std::move(projectedMovingPoints));

    pairs.clear();
    std::vector<int> referenceAssignments(referenceStars.stars.size(), -1);
    for (std::size_t movingIndex = 0; movingIndex < movingStars.stars.size(); ++movingIndex) {
        const auto& movingStar = movingStars.stars[movingIndex];
        const auto projectedMoving = applyProjectiveToStar(movingStar, transform);
        if (!std::isfinite(projectedMoving.x) || !std::isfinite(projectedMoving.y)) {
            continue;
        }
        const auto bestReference = referencePointIndex.nearest(projectedMoving.x, projectedMoving.y, options.matchTolerance);
        const std::size_t bestReferenceIndex = bestReference.index;
        const float bestDistance = bestReference.distanceSquared;
        if (!bestReference.found || bestDistance > toleranceSquared) {
            continue;
        }

        const auto reciprocalMoving = movingPointIndex.nearest(
            referenceStars.stars[bestReferenceIndex].x, referenceStars.stars[bestReferenceIndex].y, options.matchTolerance);
        if (!reciprocalMoving.found || reciprocalMoving.index != movingIndex) {
            continue;
        }

        const int currentAssignment = referenceAssignments[bestReferenceIndex];
        if (currentAssignment < 0) {
            referenceAssignments[bestReferenceIndex] = static_cast<int>(pairs.size());
            pairs.push_back({.reference = referenceStars.stars[bestReferenceIndex], .moving = movingStar});
            continue;
        }

        const auto assignedProjection = applyProjectiveToStar(pairs[static_cast<std::size_t>(currentAssignment)].moving, transform);
        const float assignedDistance =
            distanceSquared(pairs[static_cast<std::size_t>(currentAssignment)].reference, assignedProjection);
        if (bestDistance < assignedDistance) {
            pairs[static_cast<std::size_t>(currentAssignment)] = {
                .reference = referenceStars.stars[bestReferenceIndex],
                .moving = movingStar,
            };
        }
    }

    if (pairs.empty()) {
        return registrationError("RegistrationNoMatches", "No projective-matched stars were found");
    }

    RegistrationResult result;
    result.ok = true;
    result.matches = pairs.size();
    result.detectedReferenceStars = referenceStars.stars.size();
    result.detectedMovingStars = movingStars.stars.size();
    const auto denominator = std::max<std::size_t>(1, std::min(referenceStars.stars.size(), movingStars.stars.size()));
    result.inlierRatio = static_cast<float>(pairs.size()) / static_cast<float>(denominator);
    return result;
}

LocalCorrection weightedAverageCorrection(const std::vector<LocalGridControl>& controls, float sigmaSquared) {
    float weightSum = 0.0F;
    float dxSum = 0.0F;
    float dySum = 0.0F;
    for (const auto& point : controls) {
        const float weight = std::exp(-point.distanceSquared / (2.0F * sigmaSquared)) /
                             std::max(16.0F, point.distanceSquared);
        weightSum += weight;
        dxSum += weight * point.dx;
        dySum += weight * point.dy;
    }
    if (weightSum <= 1.0e-8F) {
        return {};
    }
    return {.ok = true, .dxOffset = dxSum / weightSum, .dyOffset = dySum / weightSum};
}

LocalCorrection fitLocalAffineCorrection(const std::vector<LocalGridControl>& controls, float x, float y,
                                         float imageCenterX, float imageCenterY, float normalization,
                                         float sigmaSquared) {
    if (controls.size() < 6) {
        return weightedAverageCorrection(controls, sigmaSquared);
    }

    double normal[3][3] = {};
    double rhsX[3] = {};
    double rhsY[3] = {};
    double totalWeight = 0.0;
    for (const auto& point : controls) {
        const double terms[3] = {
            static_cast<double>((point.x - x) / normalization),
            static_cast<double>((point.y - y) / normalization),
            1.0,
        };
        const double weight =
            static_cast<double>(std::exp(-point.distanceSquared / (2.0F * sigmaSquared)) /
                                std::max(36.0F, point.distanceSquared));
        totalWeight += weight;
        for (int row = 0; row < 3; ++row) {
            rhsX[row] += weight * terms[row] * point.dx;
            rhsY[row] += weight * terms[row] * point.dy;
            for (int column = 0; column < 3; ++column) {
                normal[row][column] += weight * terms[row] * terms[column];
            }
        }
    }
    if (totalWeight <= 1.0e-9) {
        return {};
    }

    const double slopeRegularization = std::max(1.0e-6, totalWeight * 2.5e-3);
    const double offsetRegularization = std::max(1.0e-6, totalWeight * 1.0e-6);
    normal[0][0] += slopeRegularization;
    normal[1][1] += slopeRegularization;
    normal[2][2] += offsetRegularization;

    double matrixX[3][4] = {};
    double matrixY[3][4] = {};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            matrixX[row][column] = normal[row][column];
            matrixY[row][column] = normal[row][column];
        }
        matrixX[row][3] = rhsX[row];
        matrixY[row][3] = rhsY[row];
    }

    double solutionX[3] = {};
    double solutionY[3] = {};
    if (!solve3x3(matrixX, solutionX) || !solve3x3(matrixY, solutionY)) {
        return weightedAverageCorrection(controls, sigmaSquared);
    }

    const double centerOffsetX = static_cast<double>((imageCenterX - x) / normalization);
    const double centerOffsetY = static_cast<double>((imageCenterY - y) / normalization);
    return {
        .ok = true,
        .dxDu = static_cast<float>(solutionX[0]),
        .dxDv = static_cast<float>(solutionX[1]),
        .dxOffset = static_cast<float>(solutionX[2] + solutionX[0] * centerOffsetX + solutionX[1] * centerOffsetY),
        .dyDu = static_cast<float>(solutionY[0]),
        .dyDv = static_cast<float>(solutionY[1]),
        .dyOffset = static_cast<float>(solutionY[2] + solutionY[0] * centerOffsetX + solutionY[1] * centerOffsetY),
    };
}

} // namespace

RegistrationResult Registration::estimateTranslation(const ImageBuffer& reference, const ImageBuffer& moving,
                                                     const RegistrationOptions& options) const {
    if (!validImageBuffer(reference) || !validImageBuffer(moving) || reference.channels != moving.channels) {
        return registrationError("ImageBufferInvalid", "Reference and moving images must be finite matching buffers");
    }
    if (!validRegistrationOptions(options)) {
        return registrationError("ArgumentInvalid", "Registration options are outside valid ranges");
    }

    const StarDetector detector;
    const auto referenceStars = detector.detect(reference, options.starDetection);
    if (!referenceStars.ok) {
        return registrationError(referenceStars.errorCode, referenceStars.message);
    }
    const auto movingStars = detector.detect(moving, options.starDetection);
    if (!movingStars.ok) {
        return registrationError(movingStars.errorCode, movingStars.message);
    }
    if (referenceStars.stars.empty() || movingStars.stars.empty()) {
        return registrationError("RegistrationInsufficientStars", "Both images need at least one detected star");
    }

    const float tolerance = std::max(0.25F, options.matchTolerance);
    std::unordered_map<std::string, OffsetBin> bins;
    bins.reserve(referenceStars.stars.size() * movingStars.stars.size());
    for (const auto& movingStar : movingStars.stars) {
        for (const auto& referenceStar : referenceStars.stars) {
            const float dx = referenceStar.x - movingStar.x;
            const float dy = referenceStar.y - movingStar.y;
            const int bx = static_cast<int>(std::lround(dx / tolerance));
            const int by = static_cast<int>(std::lround(dy / tolerance));
            bins[std::to_string(bx) + ":" + std::to_string(by)].offsets.push_back({.dx = dx, .dy = dy});
        }
    }

    std::vector<Translation> offsets;
    for (const auto& [_, bin] : bins) {
        if (bin.offsets.size() > offsets.size()) {
            offsets = bin.offsets;
        }
    }
    if (offsets.empty()) {
        return registrationError("RegistrationNoMatches", "No matching star offset was found");
    }

    std::vector<float> dxs;
    std::vector<float> dys;
    dxs.reserve(offsets.size());
    dys.reserve(offsets.size());
    for (const auto& offset : offsets) {
        dxs.push_back(offset.dx);
        dys.push_back(offset.dy);
    }
    std::sort(dxs.begin(), dxs.end());
    std::sort(dys.begin(), dys.end());
    const Translation medianOffset = {.dx = dxs[dxs.size() / 2], .dy = dys[dys.size() / 2]};

    std::size_t inliers = 0;
    for (const auto& movingStar : movingStars.stars) {
        const Star projected = {
            .x = movingStar.x + medianOffset.dx,
            .y = movingStar.y + medianOffset.dy,
        };
        const bool matched = std::any_of(referenceStars.stars.begin(), referenceStars.stars.end(), [&](const Star& referenceStar) {
            return distanceSquared(referenceStar, projected) <= tolerance * tolerance;
        });
        if (matched) {
            ++inliers;
        }
    }

    if (inliers == 0) {
        return registrationError("RegistrationNoMatches", "No RANSAC inliers were found");
    }

    std::vector<Translation> inlierOffsets;
    for (const auto& movingStar : movingStars.stars) {
        for (const auto& referenceStar : referenceStars.stars) {
            const Translation candidate = {.dx = referenceStar.x - movingStar.x, .dy = referenceStar.y - movingStar.y};
            const float dx = candidate.dx - medianOffset.dx;
            const float dy = candidate.dy - medianOffset.dy;
            if (dx * dx + dy * dy <= tolerance * tolerance) {
                inlierOffsets.push_back(candidate);
            }
        }
    }
    std::sort(inlierOffsets.begin(), inlierOffsets.end(), [](const Translation& left, const Translation& right) {
        return left.dx == right.dx ? left.dy < right.dy : left.dx < right.dx;
    });
    const auto refinedOffset = inlierOffsets[inlierOffsets.size() / 2];

    RegistrationResult result;
    result.ok = true;
    result.translation = refinedOffset;
    result.transform = {.scale = 1.0F, .rotationRadians = 0.0F, .dx = refinedOffset.dx, .dy = refinedOffset.dy};
    result.affine = {.a = 1.0F, .b = 0.0F, .c = 0.0F, .d = 1.0F, .dx = refinedOffset.dx, .dy = refinedOffset.dy};
    result.matches = inliers;
    result.detectedReferenceStars = referenceStars.stars.size();
    result.detectedMovingStars = movingStars.stars.size();
    const auto denominator = std::max<std::size_t>(1, std::min(referenceStars.stars.size(), movingStars.stars.size()));
    result.inlierRatio = static_cast<float>(inliers) / static_cast<float>(denominator);
    return result;
}

RegistrationResult Registration::estimateSimilarity(const ImageBuffer& reference, const ImageBuffer& moving,
                                                    const RegistrationOptions& options) const {
    if (!validRegistrationOptions(options)) {
        return registrationError("ArgumentInvalid", "Registration options are outside valid ranges");
    }
    const auto translationResult = estimateTranslation(reference, moving, options);
    if (!translationResult.ok && translationResult.errorCode == "ImageBufferInvalid") {
        return translationResult;
    }
    const Translation prealign = translationResult.ok ? translationResult.translation : Translation{};
    RegistrationOptions similarityOptions = options;
    similarityOptions.matchTolerance = std::max(options.matchTolerance * 10.0F, 30.0F);

    std::vector<MatchedStarPair> pairs;
    auto matchResult = detectMatchedPairs(reference, moving, similarityOptions, prealign, pairs);
    std::vector<MatchedStarPair> descriptorPairs;
    const auto descriptorMatchResult = detectDescriptorMatchedPairs(reference, moving, similarityOptions, descriptorPairs);
    if (descriptorMatchResult.ok && descriptorPairs.size() >= std::max<std::size_t>(options.minimumMatches, 2) &&
        (!matchResult.ok || descriptorPairs.size() > pairs.size())) {
        pairs = std::move(descriptorPairs);
        matchResult = descriptorMatchResult;
    }
    if (!matchResult.ok) {
        if (translationResult.ok && options.similarityFallbackToTranslation) {
            auto fallback = translationResult;
            fallback.usedFallback = true;
            fallback.message = "Similarity matching failed; using translation fallback";
            return fallback;
        }
        return matchResult;
    }

    const auto requiredMatches = std::max<std::size_t>(2, options.minimumMatches);
    if (pairs.size() < requiredMatches) {
        if (translationResult.ok && options.similarityFallbackToTranslation) {
            auto fallback = translationResult;
            fallback.usedFallback = true;
            fallback.message = "Similarity matching had too few matches; using translation fallback";
            return fallback;
        }
        return registrationError("RegistrationInsufficientMatches",
                                 "Similarity registration needs more matched stars");
    }

    std::vector<std::size_t> indices(pairs.size());
    for (std::size_t index = 0; index < indices.size(); ++index) {
        indices[index] = index;
    }

    SimilarityFit fit;
    std::vector<std::size_t> inliers = indices;
    for (int iteration = 0; iteration < 5; ++iteration) {
        fit = fitSimilarityTransform(pairs, inliers);
        if (!fit.ok) {
            break;
        }

        const float residualLimit = std::max({options.matchTolerance * 2.0F, fit.rms * 2.0F, 2.5F});
        std::vector<std::size_t> nextInliers;
        nextInliers.reserve(indices.size());
        for (const auto index : indices) {
            if (similarityResidual(pairs[index], fit.transform) <= residualLimit) {
                nextInliers.push_back(index);
            }
        }
        if (nextInliers.size() < requiredMatches || nextInliers.size() == inliers.size()) {
            break;
        }
        inliers = std::move(nextInliers);
    }

    fit = fitSimilarityTransform(pairs, inliers);
    if (!fit.ok || inliers.size() < requiredMatches) {
        if (translationResult.ok && options.similarityFallbackToTranslation) {
            auto fallback = translationResult;
            fallback.usedFallback = true;
            fallback.message = "Similarity fit failed; using translation fallback";
            return fallback;
        }
        return registrationError("RegistrationInsufficientMatches", "Similarity registration could not solve a stable transform");
    }

    RegistrationResult result;
    result.ok = true;
    result.translation = {.dx = fit.transform.dx, .dy = fit.transform.dy};
    result.transform = fit.transform;
    result.affine = fit.affine;
    result.matches = inliers.size();
    result.detectedReferenceStars = matchResult.detectedReferenceStars;
    result.detectedMovingStars = matchResult.detectedMovingStars;
    const auto denominator = std::max<std::size_t>(1, std::min(matchResult.detectedReferenceStars, matchResult.detectedMovingStars));
    result.inlierRatio = static_cast<float>(inliers.size()) / static_cast<float>(denominator);
    return result;
}

RegistrationResult Registration::estimateAffine(const ImageBuffer& reference, const ImageBuffer& moving,
                                                const RegistrationOptions& options) const {
    if (!validRegistrationOptions(options)) {
        return registrationError("ArgumentInvalid", "Registration options are outside valid ranges");
    }
    const auto translationResult = estimateTranslation(reference, moving, options);
    if (!translationResult.ok && translationResult.errorCode == "ImageBufferInvalid") {
        return translationResult;
    }
    const Translation prealign = translationResult.ok ? translationResult.translation : Translation{};
    RegistrationOptions affineOptions = options;
    affineOptions.matchTolerance = std::max(options.matchTolerance * 3.0F, 8.0F);

    std::vector<MatchedStarPair> pairs;
    auto matchResult = detectMatchedPairs(reference, moving, affineOptions, prealign, pairs);
    std::vector<MatchedStarPair> descriptorPairs;
    const auto descriptorMatchResult = detectDescriptorMatchedPairs(reference, moving, affineOptions, descriptorPairs);
    const auto requiredMatches = std::max<std::size_t>(3, options.minimumMatches);
    std::vector<std::size_t> descriptorInliers;
    const auto descriptorRequiredMatches = std::max<std::size_t>(6, requiredMatches);
    const auto descriptorFit = descriptorMatchResult.ok
        ? fitAffineTransformRansac(
              descriptorPairs, descriptorRequiredMatches, options.matchTolerance, descriptorInliers)
        : AffineFit{};
    if (descriptorFit.ok) {
        RegistrationResult result;
        result.ok = true;
        result.affine = descriptorFit.transform;
        populateSimilaritySummary(result, result.affine);
        result.matches = descriptorInliers.size();
        result.detectedReferenceStars = descriptorMatchResult.detectedReferenceStars;
        result.detectedMovingStars = descriptorMatchResult.detectedMovingStars;
        const auto denominator = std::max<std::size_t>(
            1, std::min(result.detectedReferenceStars, result.detectedMovingStars));
        result.inlierRatio = static_cast<float>(result.matches) / static_cast<float>(denominator);
        return result;
    }
    if (!matchResult.ok || pairs.size() < 3) {
        const auto similarityResult = estimateSimilarity(reference, moving, options);
        if (similarityResult.ok && options.similarityFallbackToTranslation) {
            auto fallback = similarityResult;
            const float cosTheta = std::cos(fallback.transform.rotationRadians);
            const float sinTheta = std::sin(fallback.transform.rotationRadians);
            fallback.affine = {
                .a = fallback.transform.scale * cosTheta,
                .b = -fallback.transform.scale * sinTheta,
                .c = fallback.transform.scale * sinTheta,
                .d = fallback.transform.scale * cosTheta,
                .dx = fallback.transform.dx,
                .dy = fallback.transform.dy,
            };
            fallback.usedFallback = true;
            fallback.message = "Affine matching failed; using similarity fallback";
            return fallback;
        }
        return matchResult.ok ? registrationError("RegistrationInsufficientMatches",
                                                  "Affine registration needs at least three matched stars")
                              : matchResult;
    }

    std::vector<std::size_t> indices(pairs.size());
    for (std::size_t index = 0; index < indices.size(); ++index) {
        indices[index] = index;
    }

    AffineFit fit;
    std::vector<std::size_t> inliers = indices;
    for (int iteration = 0; iteration < 4; ++iteration) {
        fit = fitAffineTransform(pairs, inliers);
        if (!fit.ok) {
            break;
        }

        const float residualLimit = std::max({options.matchTolerance * 2.0F, fit.rms * 2.2F, 2.5F});
        std::vector<std::size_t> nextInliers;
        nextInliers.reserve(indices.size());
        for (const auto index : indices) {
            if (affineResidual(pairs[index], fit.transform) <= residualLimit) {
                nextInliers.push_back(index);
            }
        }
        if (nextInliers.size() < 3 || nextInliers.size() == inliers.size()) {
            break;
        }
        inliers = std::move(nextInliers);
    }

    fit = fitAffineTransform(pairs, inliers);
    if (!fit.ok || inliers.size() < 3) {
        const auto similarityResult = estimateSimilarity(reference, moving, options);
        if (similarityResult.ok && options.similarityFallbackToTranslation) {
            auto fallback = similarityResult;
            const float cosTheta = std::cos(fallback.transform.rotationRadians);
            const float sinTheta = std::sin(fallback.transform.rotationRadians);
            fallback.affine = {
                .a = fallback.transform.scale * cosTheta,
                .b = -fallback.transform.scale * sinTheta,
                .c = fallback.transform.scale * sinTheta,
                .d = fallback.transform.scale * cosTheta,
                .dx = fallback.transform.dx,
                .dy = fallback.transform.dy,
            };
            fallback.usedFallback = true;
            fallback.message = "Affine fit failed; using similarity fallback";
            return fallback;
        }
        return registrationError("RegistrationInsufficientMatches", "Affine registration could not solve a stable transform");
    }

    RegistrationResult result;
    result.ok = true;
    result.affine = fit.transform;
    populateSimilaritySummary(result, result.affine);
    result.matches = inliers.size();
    result.detectedReferenceStars = matchResult.detectedReferenceStars;
    result.detectedMovingStars = matchResult.detectedMovingStars;
    const auto denominator = std::max<std::size_t>(1, std::min(matchResult.detectedReferenceStars, matchResult.detectedMovingStars));
    result.inlierRatio = static_cast<float>(inliers.size()) / static_cast<float>(denominator);
    return result;
}

RegistrationResult Registration::estimateDistortion(const ImageBuffer& reference, const ImageBuffer& moving,
                                                    DistortionTransform& transform,
                                                    const RegistrationOptions& options) const {
    if (!validRegistrationOptions(options)) {
        return registrationError("ArgumentInvalid", "Registration options are outside valid ranges");
    }
    const auto globalResult = estimateSimilarity(reference, moving, options);
    if (!globalResult.ok) {
        return globalResult;
    }

    transform.global = globalResult.transform;
    const float cosTheta = std::cos(transform.global.rotationRadians);
    const float sinTheta = std::sin(transform.global.rotationRadians);
    transform.projective = {
        .h00 = transform.global.scale * cosTheta,
        .h01 = -transform.global.scale * sinTheta,
        .h02 = transform.global.dx,
        .h10 = transform.global.scale * sinTheta,
        .h11 = transform.global.scale * cosTheta,
        .h12 = transform.global.dy,
        .h20 = 0.0F,
        .h21 = 0.0F,
        .h22 = 1.0F,
    };
    transform.useProjective = false;
    transform.polynomialDx = {};
    transform.polynomialDy = {};
    transform.usePolynomial = false;
    transform.controlPoints.clear();
    transform.influenceRadius =
        std::clamp(static_cast<float>(std::max(reference.width, reference.height)) * 0.026F, 72.0F, 168.0F);

    RegistrationOptions localOptions = options;
    localOptions.matchTolerance = std::max(options.matchTolerance * 12.0F, 36.0F);
    localOptions.starDetection.maxStars = std::max<std::size_t>(localOptions.starDetection.maxStars, 6000);
    std::vector<MatchedStarPair> pairs;
    auto matchResult = detectSimilarityProjectedPairs(reference, moving, localOptions, transform.global, pairs);
    if (!matchResult.ok || pairs.size() < std::max<std::size_t>(options.minimumMatches, 8)) {
        std::vector<MatchedStarPair> descriptorPairs;
        const auto descriptorResult = detectDescriptorMatchedPairs(reference, moving, localOptions, descriptorPairs);
        if (descriptorResult.ok && descriptorPairs.size() > pairs.size()) {
            pairs = std::move(descriptorPairs);
            matchResult = descriptorResult;
        }
    }

    if (!matchResult.ok || pairs.size() < 2) {
        auto fallback = globalResult;
        fallback.usedFallback = true;
        fallback.message = "Distortion matching failed; using global similarity";
        return fallback;
    }

    double similarityResidualSum = 0.0;
    for (const auto& pair : pairs) {
        const float residual = similarityResidual(pair, transform.global);
        similarityResidualSum += static_cast<double>(residual) * residual;
    }
    const float similarityRms =
        static_cast<float>(std::sqrt(similarityResidualSum / static_cast<double>(std::max<std::size_t>(1, pairs.size()))));
    const std::size_t requiredProjectiveMatches = std::max<std::size_t>(options.minimumMatches, 12);
    std::vector<std::size_t> projectiveInliers;
    auto projectiveFit =
        fitProjectiveTransformRobust(pairs, requiredProjectiveMatches, options.matchTolerance, projectiveInliers);
    if (projectiveFit.ok &&
        (projectiveFit.rms <= similarityRms * 1.08F || projectiveFit.rms <= std::max(options.matchTolerance * 3.0F, 6.0F))) {
        transform.projective = projectiveFit.transform;
        transform.useProjective = true;

        RegistrationOptions projectiveOptions = localOptions;
        projectiveOptions.matchTolerance = std::max(options.matchTolerance * 4.0F, 12.0F);
        std::vector<MatchedStarPair> projectivePairs;
        const auto projectiveMatchResult =
            detectProjectiveProjectedPairs(reference, moving, projectiveOptions, transform.projective, projectivePairs);
        if (projectiveMatchResult.ok && projectivePairs.size() >= requiredProjectiveMatches) {
            std::vector<std::size_t> refinedInliers;
            const auto refinedProjectiveFit =
                fitProjectiveTransformRobust(projectivePairs, requiredProjectiveMatches, options.matchTolerance, refinedInliers);
            if (refinedProjectiveFit.ok && refinedProjectiveFit.rms <= std::max(projectiveFit.rms * 1.15F, options.matchTolerance * 2.5F)) {
                transform.projective = refinedProjectiveFit.transform;
                pairs = std::move(projectivePairs);
                matchResult = projectiveMatchResult;
                projectiveFit = refinedProjectiveFit;
                projectiveInliers = std::move(refinedInliers);
            }
        }
    }

    double sourceResidualSum = 0.0;
    for (const auto& pair : pairs) {
        const auto [baseSourceX, baseSourceY] =
            transform.useProjective ? inverseProjectivePoint(pair.reference.x, pair.reference.y, transform.projective)
                                    : inverseSimilarityPoint(pair.reference.x, pair.reference.y, transform.global);
        const double residualX = static_cast<double>(baseSourceX - pair.moving.x);
        const double residualY = static_cast<double>(baseSourceY - pair.moving.y);
        sourceResidualSum += residualX * residualX + residualY * residualY;
    }
    const float sourceRms =
        static_cast<float>(std::sqrt(sourceResidualSum / static_cast<double>(std::max<std::size_t>(1, pairs.size()))));
    const std::size_t requiredPolynomialMatches =
        std::max<std::size_t>({options.minimumMatches, kPolynomialFitTermCount * 3, 30});
    std::vector<std::size_t> polynomialInliers;
    const auto polynomialFit = fitPolynomialCorrectionRobust(
        pairs, transform, reference.width, reference.height, requiredPolynomialMatches, options.matchTolerance, polynomialInliers);
    if (polynomialFit.ok && polynomialFit.rms <= sourceRms * 0.88F) {
        transform.polynomialDx = polynomialFit.dx;
        transform.polynomialDy = polynomialFit.dy;
        transform.usePolynomial = true;
    }

    const float residualLimit = transform.usePolynomial ? std::max(options.matchTolerance * 2.0F, 6.0F)
                                : transform.useProjective ? std::max(options.matchTolerance * 4.0F, 12.0F)
                                                          : std::max(options.matchTolerance * 16.0F, 48.0F);
    for (const auto& pair : pairs) {
        const auto [baseSourceX, baseSourceY] =
            transform.useProjective ? inverseProjectivePoint(pair.reference.x, pair.reference.y, transform.projective)
                                    : inverseSimilarityPoint(pair.reference.x, pair.reference.y, transform.global);
        const auto [polynomialDx, polynomialDy] =
            transform.usePolynomial
                ? evaluatePolynomialCorrection(pair.reference.x, pair.reference.y, reference.width, reference.height,
                                               transform.polynomialDx, transform.polynomialDy)
                : std::pair<float, float>{0.0F, 0.0F};
        if (!std::isfinite(baseSourceX) || !std::isfinite(baseSourceY) || !std::isfinite(polynomialDx) ||
            !std::isfinite(polynomialDy)) {
            continue;
        }
        const float residualX = pair.moving.x - (baseSourceX + polynomialDx);
        const float residualY = pair.moving.y - (baseSourceY + polynomialDy);
        if (residualX * residualX + residualY * residualY <= residualLimit * residualLimit) {
            transform.controlPoints.push_back({
                .x = pair.reference.x,
                .y = pair.reference.y,
                .dx = residualX,
                .dy = residualY,
            });
        }
    }

    RegistrationResult result = globalResult;
    result.matches = transform.controlPoints.size();
    result.detectedReferenceStars = matchResult.detectedReferenceStars;
    result.detectedMovingStars = matchResult.detectedMovingStars;
    const auto denominator = std::max<std::size_t>(1, std::min(matchResult.detectedReferenceStars, matchResult.detectedMovingStars));
    result.inlierRatio = static_cast<float>(transform.controlPoints.size()) / static_cast<float>(denominator);
    if (transform.controlPoints.size() < std::max<std::size_t>(options.minimumMatches, 8)) {
        result.usedFallback = true;
        result.message = transform.useProjective ? "Distortion had too few local control points; using projective transform"
                                                 : "Distortion had too few local control points; using global similarity";
    } else if (transform.useProjective) {
        result.message = transform.usePolynomial
                             ? "Distortion used projective and polynomial baselines with local residual control points"
                             : "Distortion used projective baseline with local residual control points";
    }
    return result;
}

namespace {

template <typename Mapper, typename Sampler>
bool renderMappedRows(const ImageBuffer& image, const RegistrationRowConsumer& consumer, Mapper mapper,
                      Sampler sampler) {
    if (!consumer || !validImageBuffer(image)) {
        return false;
    }
    std::vector<float> row(static_cast<std::size_t>(image.width) * image.channels, 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto offset = static_cast<std::size_t>(x) * image.channels;
            const auto source = mapper(x, y);
            for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                row[offset + channel] = sampler(source.first, source.second, channel);
            }
        }
        if (!consumer(y, row.data(), row.size())) {
            return false;
        }
    }
    return true;
}

RegistrationRowConsumer outputRowConsumer(ImageBuffer& output) {
    return [&output](std::uint32_t row, const float* samples, std::size_t sampleCount) {
        const auto offset = static_cast<std::size_t>(row) * output.width * output.channels;
        std::copy(samples, samples + sampleCount, output.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    };
}

class DistortionCoordinateMapper {
  public:
    DistortionCoordinateMapper(const DistortionTransform& transform, std::uint32_t width, std::uint32_t height)
        : transform_(transform), width_(width), height_(height),
          imageCenterX_(static_cast<float>(width - 1) * 0.5F),
          imageCenterY_(static_cast<float>(height - 1) * 0.5F),
          normalization_(std::max<float>(1.0F, std::max(width, height))) {
        if (transform_.controlPoints.empty()) {
            return;
        }

        gridWidth_ = width_ / gridSpacing_ + 2;
        gridHeight_ = height_ / gridSpacing_ + 2;
        const auto gridSize = static_cast<std::size_t>(gridWidth_) * gridHeight_;
        gridDxDu_.assign(gridSize, 0.0F);
        gridDxDv_.assign(gridSize, 0.0F);
        gridDxOffset_.assign(gridSize, 0.0F);
        gridDyDu_.assign(gridSize, 0.0F);
        gridDyDv_.assign(gridSize, 0.0F);
        gridDyOffset_.assign(gridSize, 0.0F);

        const float sigma = std::max(1.0F, transform_.influenceRadius);
        const float sigmaSquared = sigma * sigma;
        for (std::uint32_t gy = 0; gy < gridHeight_; ++gy) {
            for (std::uint32_t gx = 0; gx < gridWidth_; ++gx) {
                const float x = static_cast<float>(std::min(gx * gridSpacing_, width_ - 1));
                const float y = static_cast<float>(std::min(gy * gridSpacing_, height_ - 1));
                std::vector<LocalGridControl> nearby;
                nearby.reserve(transform_.controlPoints.size());
                for (const auto& point : transform_.controlPoints) {
                    const float dx = x - point.x;
                    const float dy = y - point.y;
                    nearby.push_back({
                        .distanceSquared = dx * dx + dy * dy,
                        .x = point.x,
                        .y = point.y,
                        .dx = point.dx,
                        .dy = point.dy,
                    });
                }

                const std::size_t keepCount = std::min<std::size_t>(nearby.size(), 28);
                if (keepCount == 0) {
                    continue;
                }
                if (keepCount < nearby.size()) {
                    std::nth_element(nearby.begin(), nearby.begin() + static_cast<std::ptrdiff_t>(keepCount),
                                     nearby.end(), [](const LocalGridControl& left, const LocalGridControl& right) {
                                         return left.distanceSquared < right.distanceSquared;
                                     });
                }
                nearby.resize(keepCount);

                std::vector<float> localDx;
                std::vector<float> localDy;
                localDx.reserve(nearby.size());
                localDy.reserve(nearby.size());
                for (const auto& point : nearby) {
                    localDx.push_back(point.dx);
                    localDy.push_back(point.dy);
                }
                const auto median = [](std::vector<float>& values) {
                    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
                    std::nth_element(values.begin(), middle, values.end());
                    return *middle;
                };
                const float medianDx = median(localDx);
                const float medianDy = median(localDy);

                std::vector<LocalGridControl> filtered;
                filtered.reserve(nearby.size());
                const float correctionLimit = std::max(8.0F, sigma * 0.14F);
                for (const auto& point : nearby) {
                    const float correctionDx = point.dx - medianDx;
                    const float correctionDy = point.dy - medianDy;
                    if (correctionDx * correctionDx + correctionDy * correctionDy <=
                        correctionLimit * correctionLimit) {
                        filtered.push_back(point);
                    }
                }
                if (filtered.size() < 6) {
                    filtered = std::move(nearby);
                }

                const auto correction = fitLocalAffineCorrection(filtered, x, y, imageCenterX_, imageCenterY_,
                                                                  normalization_, sigmaSquared);
                if (!correction.ok) {
                    continue;
                }
                const auto index = static_cast<std::size_t>(gy) * gridWidth_ + gx;
                gridDxDu_[index] = correction.dxDu;
                gridDxDv_[index] = correction.dxDv;
                gridDxOffset_[index] = correction.dxOffset;
                gridDyDu_[index] = correction.dyDu;
                gridDyDv_[index] = correction.dyDv;
                gridDyOffset_[index] = correction.dyOffset;
            }
        }
    }

    std::pair<float, float> sourceForTarget(float targetX, float targetY) const {
        const auto base = baselineSourceForTarget(targetX, targetY);
        const auto correction = correctionForTarget(targetX, targetY);
        return {base.first + correction.first, base.second + correction.second};
    }

    std::pair<float, float> targetForSource(float sourceX, float sourceY) const {
        auto target = baselineTargetForSource(sourceX, sourceY);
        if (!transform_.usePolynomial && transform_.controlPoints.empty()) {
            return target;
        }

        for (int iteration = 0; iteration < 5; ++iteration) {
            const auto correction = correctionForTarget(target.first, target.second);
            const auto next = baselineTargetForSource(sourceX - correction.first, sourceY - correction.second);
            if (!std::isfinite(next.first) || !std::isfinite(next.second)) {
                break;
            }
            const float dx = next.first - target.first;
            const float dy = next.second - target.second;
            target = next;
            if (dx * dx + dy * dy < 1.0e-6F) {
                break;
            }
        }
        return target;
    }

  private:
    static constexpr std::uint32_t gridSpacing_ = 32;

    std::pair<float, float> baselineSourceForTarget(float targetX, float targetY) const {
        return transform_.useProjective ? inverseProjectivePoint(targetX, targetY, transform_.projective)
                                        : inverseSimilarityPoint(targetX, targetY, transform_.global);
    }

    std::pair<float, float> baselineTargetForSource(float sourceX, float sourceY) const {
        if (transform_.useProjective) {
            return applyProjectivePoint(sourceX, sourceY, transform_.projective);
        }
        const float cosTheta = std::cos(transform_.global.rotationRadians);
        const float sinTheta = std::sin(transform_.global.rotationRadians);
        return {
            transform_.global.scale * (cosTheta * sourceX - sinTheta * sourceY) + transform_.global.dx,
            transform_.global.scale * (sinTheta * sourceX + cosTheta * sourceY) + transform_.global.dy,
        };
    }

    float sampleGrid(const std::vector<float>& grid, float x, float y) const {
        if (grid.empty()) {
            return 0.0F;
        }
        const float gx = std::clamp(x / static_cast<float>(gridSpacing_), 0.0F,
                                    static_cast<float>(gridWidth_ - 1));
        const float gy = std::clamp(y / static_cast<float>(gridSpacing_), 0.0F,
                                    static_cast<float>(gridHeight_ - 1));
        const auto x0 = static_cast<std::uint32_t>(std::floor(gx));
        const auto y0 = static_cast<std::uint32_t>(std::floor(gy));
        const auto x1 = std::min<std::uint32_t>(x0 + 1, gridWidth_ - 1);
        const auto y1 = std::min<std::uint32_t>(y0 + 1, gridHeight_ - 1);
        const float tx = gx - static_cast<float>(x0);
        const float ty = gy - static_cast<float>(y0);
        const auto index = [&](std::uint32_t px, std::uint32_t py) {
            return static_cast<std::size_t>(py) * gridWidth_ + px;
        };
        const float top = grid[index(x0, y0)] + (grid[index(x1, y0)] - grid[index(x0, y0)]) * tx;
        const float bottom = grid[index(x0, y1)] + (grid[index(x1, y1)] - grid[index(x0, y1)]) * tx;
        return top + (bottom - top) * ty;
    }

    std::pair<float, float> correctionForTarget(float targetX, float targetY) const {
        const auto polynomial = transform_.usePolynomial
                                    ? evaluatePolynomialCorrection(targetX, targetY, width_, height_,
                                                                   transform_.polynomialDx, transform_.polynomialDy)
                                    : std::pair<float, float>{0.0F, 0.0F};
        if (transform_.controlPoints.empty()) {
            return polynomial;
        }

        const float normalizedX = (targetX - imageCenterX_) / normalization_;
        const float normalizedY = (targetY - imageCenterY_) / normalization_;
        const float localX = sampleGrid(gridDxDu_, targetX, targetY) * normalizedX +
                             sampleGrid(gridDxDv_, targetX, targetY) * normalizedY +
                             sampleGrid(gridDxOffset_, targetX, targetY);
        const float localY = sampleGrid(gridDyDu_, targetX, targetY) * normalizedX +
                             sampleGrid(gridDyDv_, targetX, targetY) * normalizedY +
                             sampleGrid(gridDyOffset_, targetX, targetY);
        return {polynomial.first + localX, polynomial.second + localY};
    }

    const DistortionTransform& transform_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t gridWidth_ = 0;
    std::uint32_t gridHeight_ = 0;
    float imageCenterX_ = 0.0F;
    float imageCenterY_ = 0.0F;
    float normalization_ = 1.0F;
    std::vector<float> gridDxDu_;
    std::vector<float> gridDxDv_;
    std::vector<float> gridDxOffset_;
    std::vector<float> gridDyDu_;
    std::vector<float> gridDyDv_;
    std::vector<float> gridDyOffset_;
};

} // namespace

bool Registration::renderTranslationRows(const ImageBuffer& image, Translation translation,
                                         const RegistrationRowConsumer& consumer) const {
    if (!std::isfinite(translation.dx) || !std::isfinite(translation.dy)) {
        return false;
    }
    return renderMappedRows(
        image, consumer,
        [&](std::uint32_t x, std::uint32_t y) {
            return std::pair<int, int>{
                static_cast<int>(std::lround(static_cast<float>(x) - translation.dx)),
                static_cast<int>(std::lround(static_cast<float>(y) - translation.dy)),
            };
        },
        [&](int sourceX, int sourceY, std::uint16_t channel) {
            return sampleNearest(image, sourceX, sourceY, channel);
        });
}

bool Registration::renderSimilarityRows(const ImageBuffer& image, SimilarityTransform transform,
                                        const RegistrationRowConsumer& consumer) const {
    if (!finiteSimilarityTransform(transform)) {
        return false;
    }
    const float scale = std::max(1.0e-6F, transform.scale);
    const float cosTheta = std::cos(transform.rotationRadians);
    const float sinTheta = std::sin(transform.rotationRadians);
    return renderMappedRows(
        image, consumer,
        [&](std::uint32_t x, std::uint32_t y) {
            const float targetX = static_cast<float>(x) - transform.dx;
            const float targetY = static_cast<float>(y) - transform.dy;
            return std::pair<float, float>{
                (cosTheta * targetX + sinTheta * targetY) / scale,
                (-sinTheta * targetX + cosTheta * targetY) / scale,
            };
        },
        [&](float sourceX, float sourceY, std::uint16_t channel) {
            return sampleBilinear(image, sourceX, sourceY, channel);
        });
}

bool Registration::renderAffineRows(const ImageBuffer& image, AffineTransform transform,
                                    const RegistrationRowConsumer& consumer) const {
    if (!finiteAffineTransform(transform)) {
        return false;
    }
    const float determinant = transform.a * transform.d - transform.b * transform.c;
    if (std::fabs(determinant) < 1.0e-8F) {
        return renderMappedRows(
            image, consumer, [](std::uint32_t, std::uint32_t) { return std::pair<float, float>{0.0F, 0.0F}; },
            [](float, float, std::uint16_t) { return 0.0F; });
    }
    const float invA = transform.d / determinant;
    const float invB = -transform.b / determinant;
    const float invC = -transform.c / determinant;
    const float invD = transform.a / determinant;
    return renderMappedRows(
        image, consumer,
        [&](std::uint32_t x, std::uint32_t y) {
            const float targetX = static_cast<float>(x) - transform.dx;
            const float targetY = static_cast<float>(y) - transform.dy;
            return std::pair<float, float>{invA * targetX + invB * targetY, invC * targetX + invD * targetY};
        },
        [&](float sourceX, float sourceY, std::uint16_t channel) {
            return sampleBilinear(image, sourceX, sourceY, channel);
        });
}

ImageBuffer Registration::applyTranslation(const ImageBuffer& image, Translation translation) const {
    if (!validImageBuffer(image) || !std::isfinite(translation.dx) || !std::isfinite(translation.dy)) {
        return {};
    }
    ImageBuffer output;
    output.width = image.width;
    output.height = image.height;
    output.channels = image.channels;
    output.format = image.format;
    output.colorEncoding = image.colorEncoding;
    output.sourceBitsPerChannel = image.sourceBitsPerChannel;
    output.pixels.assign(image.sampleCount(), 0.0F);

    for (std::uint32_t y = 0; y < output.height; ++y) {
        for (std::uint32_t x = 0; x < output.width; ++x) {
            const int sourceX = static_cast<int>(std::lround(static_cast<float>(x) - translation.dx));
            const int sourceY = static_cast<int>(std::lround(static_cast<float>(y) - translation.dy));
            const auto outputOffset = (static_cast<std::size_t>(y) * output.width + x) * output.channels;
            for (std::uint16_t c = 0; c < output.channels; ++c) {
                output.pixels[outputOffset + c] = sampleNearest(image, sourceX, sourceY, c);
            }
        }
    }

    return output;
}

ImageBuffer Registration::applySimilarity(const ImageBuffer& image, SimilarityTransform transform) const {
    if (!validImageBuffer(image) || !finiteSimilarityTransform(transform)) {
        return {};
    }
    ImageBuffer output;
    output.width = image.width;
    output.height = image.height;
    output.channels = image.channels;
    output.format = image.format;
    output.colorEncoding = image.colorEncoding;
    output.sourceBitsPerChannel = image.sourceBitsPerChannel;
    output.pixels.assign(image.sampleCount(), 0.0F);

    const float scale = std::max(1.0e-6F, transform.scale);
    const float cosTheta = std::cos(transform.rotationRadians);
    const float sinTheta = std::sin(transform.rotationRadians);
    for (std::uint32_t y = 0; y < output.height; ++y) {
        for (std::uint32_t x = 0; x < output.width; ++x) {
            const float targetX = static_cast<float>(x) - transform.dx;
            const float targetY = static_cast<float>(y) - transform.dy;
            const float sourceX = (cosTheta * targetX + sinTheta * targetY) / scale;
            const float sourceY = (-sinTheta * targetX + cosTheta * targetY) / scale;
            const auto outputOffset = (static_cast<std::size_t>(y) * output.width + x) * output.channels;
            for (std::uint16_t c = 0; c < output.channels; ++c) {
                output.pixels[outputOffset + c] = sampleBilinear(image, sourceX, sourceY, c);
            }
        }
    }

    return output;
}

ImageBuffer Registration::applyAffine(const ImageBuffer& image, AffineTransform transform) const {
    if (!validImageBuffer(image) || !finiteAffineTransform(transform)) {
        return {};
    }
    ImageBuffer output;
    output.width = image.width;
    output.height = image.height;
    output.channels = image.channels;
    output.format = image.format;
    output.colorEncoding = image.colorEncoding;
    output.sourceBitsPerChannel = image.sourceBitsPerChannel;
    output.pixels.assign(image.sampleCount(), 0.0F);

    const float determinant = transform.a * transform.d - transform.b * transform.c;
    if (std::fabs(determinant) < 1.0e-8F) {
        return output;
    }
    const float invA = transform.d / determinant;
    const float invB = -transform.b / determinant;
    const float invC = -transform.c / determinant;
    const float invD = transform.a / determinant;

    for (std::uint32_t y = 0; y < output.height; ++y) {
        for (std::uint32_t x = 0; x < output.width; ++x) {
            const float targetX = static_cast<float>(x) - transform.dx;
            const float targetY = static_cast<float>(y) - transform.dy;
            const float sourceX = invA * targetX + invB * targetY;
            const float sourceY = invC * targetX + invD * targetY;
            const auto outputOffset = (static_cast<std::size_t>(y) * output.width + x) * output.channels;
            for (std::uint16_t channel = 0; channel < output.channels; ++channel) {
                output.pixels[outputOffset + channel] = sampleBilinear(image, sourceX, sourceY, channel);
            }
        }
    }

    return output;
}

bool Registration::renderDistortionRows(const ImageBuffer& image, const DistortionTransform& transform,
                                        const RegistrationRowConsumer& consumer) const {
    if (!validImageBuffer(image) || !finiteDistortionTransform(transform)) {
        return false;
    }
    if (transform.controlPoints.empty()) {
        if (!transform.useProjective) {
            return renderSimilarityRows(image, transform.global, consumer);
        }

        return renderMappedRows(
            image, consumer,
            [&](std::uint32_t x, std::uint32_t y) {
                const auto [baseSourceX, baseSourceY] =
                    inverseProjectivePoint(static_cast<float>(x), static_cast<float>(y), transform.projective);
                const auto [polynomialDx, polynomialDy] =
                    transform.usePolynomial
                        ? evaluatePolynomialCorrection(static_cast<float>(x), static_cast<float>(y), image.width,
                                                       image.height, transform.polynomialDx, transform.polynomialDy)
                        : std::pair<float, float>{0.0F, 0.0F};
                return std::pair<float, float>{baseSourceX + polynomialDx, baseSourceY + polynomialDy};
            },
            [&](float sourceX, float sourceY, std::uint16_t channel) {
                return sampleBilinear(image, sourceX, sourceY, channel);
            });
    }

    constexpr std::uint32_t gridSpacing = 32;
    const std::uint32_t gridWidth = image.width / gridSpacing + 2;
    const std::uint32_t gridHeight = image.height / gridSpacing + 2;
    const auto gridSize = static_cast<std::size_t>(gridWidth) * gridHeight;
    std::vector<float> gridDxDu(gridSize, 0.0F);
    std::vector<float> gridDxDv(gridSize, 0.0F);
    std::vector<float> gridDxOffset(gridSize, 0.0F);
    std::vector<float> gridDyDu(gridSize, 0.0F);
    std::vector<float> gridDyDv(gridSize, 0.0F);
    std::vector<float> gridDyOffset(gridSize, 0.0F);
    const float sigma = std::max(1.0F, transform.influenceRadius);
    const float sigmaSquared = sigma * sigma;
    const float imageCenterX = static_cast<float>(image.width - 1) * 0.5F;
    const float imageCenterY = static_cast<float>(image.height - 1) * 0.5F;
    const float normalization = std::max<float>(1.0F, std::max(image.width, image.height));

    for (std::uint32_t gy = 0; gy < gridHeight; ++gy) {
        for (std::uint32_t gx = 0; gx < gridWidth; ++gx) {
            const float x = static_cast<float>(std::min(gx * gridSpacing, image.width - 1));
            const float y = static_cast<float>(std::min(gy * gridSpacing, image.height - 1));
            std::vector<LocalGridControl> nearby;
            nearby.reserve(transform.controlPoints.size());
            for (const auto& point : transform.controlPoints) {
                const float dx = x - point.x;
                const float dy = y - point.y;
                const float distanceSquared = dx * dx + dy * dy;
                nearby.push_back({
                    .distanceSquared = distanceSquared,
                    .x = point.x,
                    .y = point.y,
                    .dx = point.dx,
                    .dy = point.dy,
                });
            }
            const auto index = static_cast<std::size_t>(gy) * gridWidth + gx;
            const std::size_t keepCount = std::min<std::size_t>(nearby.size(), 28);
            if (keepCount == 0) {
                continue;
            }
            if (keepCount < nearby.size()) {
                std::nth_element(nearby.begin(), nearby.begin() + static_cast<std::ptrdiff_t>(keepCount), nearby.end(),
                                 [](const LocalGridControl& left, const LocalGridControl& right) {
                                     return left.distanceSquared < right.distanceSquared;
                                 });
            }
            nearby.resize(keepCount);

            std::vector<float> localDx;
            std::vector<float> localDy;
            localDx.reserve(nearby.size());
            localDy.reserve(nearby.size());
            for (const auto& point : nearby) {
                localDx.push_back(point.dx);
                localDy.push_back(point.dy);
            }
            auto median = [](std::vector<float>& values) {
                const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
                std::nth_element(values.begin(), middle, values.end());
                return *middle;
            };
            const float medianDx = median(localDx);
            const float medianDy = median(localDy);

            std::vector<LocalGridControl> filtered;
            filtered.reserve(nearby.size());
            const float correctionLimit = std::max(8.0F, sigma * 0.14F);
            for (const auto& point : nearby) {
                const float correctionDx = point.dx - medianDx;
                const float correctionDy = point.dy - medianDy;
                if (correctionDx * correctionDx + correctionDy * correctionDy > correctionLimit * correctionLimit) {
                    continue;
                }
                filtered.push_back(point);
            }
            if (filtered.size() < 6) {
                filtered = std::move(nearby);
            }

            const auto correction =
                fitLocalAffineCorrection(filtered, x, y, imageCenterX, imageCenterY, normalization, sigmaSquared);
            if (correction.ok) {
                gridDxDu[index] = correction.dxDu;
                gridDxDv[index] = correction.dxDv;
                gridDxOffset[index] = correction.dxOffset;
                gridDyDu[index] = correction.dyDu;
                gridDyDv[index] = correction.dyDv;
                gridDyOffset[index] = correction.dyOffset;
            }
        }
    }

    const auto sampleGrid = [&](const std::vector<float>& grid, float x, float y) {
        const float gx = std::clamp(x / static_cast<float>(gridSpacing), 0.0F, static_cast<float>(gridWidth - 1));
        const float gy = std::clamp(y / static_cast<float>(gridSpacing), 0.0F, static_cast<float>(gridHeight - 1));
        const auto x0 = static_cast<std::uint32_t>(std::floor(gx));
        const auto y0 = static_cast<std::uint32_t>(std::floor(gy));
        const auto x1 = std::min<std::uint32_t>(x0 + 1, gridWidth - 1);
        const auto y1 = std::min<std::uint32_t>(y0 + 1, gridHeight - 1);
        const float tx = gx - static_cast<float>(x0);
        const float ty = gy - static_cast<float>(y0);
        const auto gridIndex = [&](std::uint32_t px, std::uint32_t py) {
            return static_cast<std::size_t>(py) * gridWidth + px;
        };
        const float top = grid[gridIndex(x0, y0)] + (grid[gridIndex(x1, y0)] - grid[gridIndex(x0, y0)]) * tx;
        const float bottom = grid[gridIndex(x0, y1)] + (grid[gridIndex(x1, y1)] - grid[gridIndex(x0, y1)]) * tx;
        return top + (bottom - top) * ty;
    };

    return renderMappedRows(
        image, consumer,
        [&](std::uint32_t x, std::uint32_t y) {
            const auto [baseSourceX, baseSourceY] =
                transform.useProjective
                    ? inverseProjectivePoint(static_cast<float>(x), static_cast<float>(y), transform.projective)
                    : inverseSimilarityPoint(static_cast<float>(x), static_cast<float>(y), transform.global);
            const auto [polynomialDx, polynomialDy] =
                transform.usePolynomial
                    ? evaluatePolynomialCorrection(static_cast<float>(x), static_cast<float>(y), image.width,
                                                   image.height, transform.polynomialDx, transform.polynomialDy)
                    : std::pair<float, float>{0.0F, 0.0F};
            const float normalizedX = (static_cast<float>(x) - imageCenterX) / normalization;
            const float normalizedY = (static_cast<float>(y) - imageCenterY) / normalization;
            const float correctionX = sampleGrid(gridDxDu, static_cast<float>(x), static_cast<float>(y)) * normalizedX +
                                      sampleGrid(gridDxDv, static_cast<float>(x), static_cast<float>(y)) * normalizedY +
                                      sampleGrid(gridDxOffset, static_cast<float>(x), static_cast<float>(y));
            const float correctionY = sampleGrid(gridDyDu, static_cast<float>(x), static_cast<float>(y)) * normalizedX +
                                      sampleGrid(gridDyDv, static_cast<float>(x), static_cast<float>(y)) * normalizedY +
                                      sampleGrid(gridDyOffset, static_cast<float>(x), static_cast<float>(y));
            const float sourceX = baseSourceX + polynomialDx + correctionX;
            const float sourceY = baseSourceY + polynomialDy + correctionY;
            return std::pair<float, float>{sourceX, sourceY};
        },
        [&](float sourceX, float sourceY, std::uint16_t channel) {
            return sampleBilinear(image, sourceX, sourceY, channel);
        });
}

bool Registration::renderDistortionForwardRows(const ImageBuffer& image, const DistortionTransform& transform,
                                               const RegistrationCoordinateConsumer& consumer) const {
    if (!consumer || !validImageBuffer(image) || !finiteDistortionTransform(transform)) {
        return false;
    }

    const DistortionCoordinateMapper mapper(transform, image.width, image.height);
    std::vector<float> coordinates(static_cast<std::size_t>(image.width) * 2, 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto target = mapper.targetForSource(static_cast<float>(x), static_cast<float>(y));
            const auto offset = static_cast<std::size_t>(x) * 2;
            coordinates[offset] = target.first;
            coordinates[offset + 1] = target.second;
        }
        if (!consumer(y, coordinates.data(), coordinates.size())) {
            return false;
        }
    }
    return true;
}

ImageBuffer Registration::applyDistortion(const ImageBuffer& image, const DistortionTransform& transform) const {
    if (!validImageBuffer(image) || !finiteDistortionTransform(transform)) {
        return {};
    }
    ImageBuffer output;
    output.width = image.width;
    output.height = image.height;
    output.channels = image.channels;
    output.format = image.format;
    output.colorEncoding = image.colorEncoding;
    output.sourceBitsPerChannel = image.sourceBitsPerChannel;
    output.pixels.assign(image.sampleCount(), 0.0F);
    if (!renderDistortionRows(image, transform, outputRowConsumer(output))) {
        return {};
    }
    return output;
}

} // namespace photonstack
