#include "photonstack/PsfHomogenizer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {
PsfHomogenizeResult failure(std::string code, std::string message) {
    PsfHomogenizeResult result;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}
} // namespace

PsfHomogenizeResult PsfHomogenizer::apply(const ImageBuffer& image, const PsfHomogenizeOptions& options) const {
    if (image.empty() || (image.channels != 1 && image.channels != 3 && image.channels != 4) ||
        image.pixels.size() != image.sampleCount() || detail::imageHasInvalidCoveredColor(image)) {
        return failure("ImageBufferInvalid", "Expected a finite mono, RGB or RGBA image");
    }
    const double xx = options.covarianceXX, yy = options.covarianceYY, xy = options.covarianceXY;
    if (!std::isfinite(xx) || !std::isfinite(yy) || !std::isfinite(xy) || xx <= 0 || yy <= 0 || xx > 100 || yy > 100 ||
        std::abs(xy) > 100 || !std::isfinite(options.strength) || options.strength < 0 || options.strength > 1 ||
        xx * yy - xy * xy <= 0) {
        return failure("ArgumentInvalid", "PSF covariance must be positive definite; strength must be in [0,1]");
    }
    // Geometric mean of the eigenvalues preserves the model Gaussian's area.
    const double target = std::sqrt(xx * yy - xy * xy);
    const double dx = (target - xx) * options.strength;
    const double dy = (target - yy) * options.strength;
    const double cross = -xy * options.strength;
    const double radius = std::hypot((dx - dy) * 0.5, cross);
    if (std::abs((dx + dy) * 0.5) + radius > 0.5) {
        return failure("ArgumentInvalid", "Correction exceeds the small-kernel limit of 0.5 pixel squared");
    }
    PsfHomogenizeResult result;
    // Second-order finite-difference approximation: unit DC gain, zero centroid,
    // covariance delta [dx,cross;cross,dy]. Apply one identical signed kernel to RGB.
    result.kernel = {cross / 4, dy / 2, -cross / 4, dx / 2, 1 - dx - dy, dx / 2, -cross / 4, dy / 2, cross / 4};
    double squared = 0;
    for (double value : result.kernel)
        squared += value * value;
    result.whiteNoiseGain = std::sqrt(squared);
    result.image = image;
    const auto channels = detail::colorChannelCount(image);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            if (!detail::pixelHasValidColor(image, pixel)) {
                detail::clearMaskedPixel(result.image, pixel);
                continue;
            }
            if (x == 0 || y == 0 || x + 1 == image.width || y + 1 == image.height)
                continue;
            // Signed kernels cannot be safely renormalized at missing coverage.
            // Keep the original pixel whenever any tap has partial/no coverage.
            bool covered = true;
            for (int j = -1; j <= 1; ++j) {
                for (int i = -1; i <= 1; ++i) {
                    const auto neighbor =
                        static_cast<std::size_t>(static_cast<int>(y) + j) * image.width + static_cast<int>(x) + i;
                    covered = covered && detail::pixelCoverage(image, neighbor) >= 0.999999F;
                }
            }
            if (!covered)
                continue;
            for (std::uint16_t c = 0; c < channels; ++c) {
                const double center = image.pixels[pixel * image.channels + c];
                double value = center;
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i) {
                        const auto neighbor =
                            static_cast<std::size_t>(static_cast<int>(y) + j) * image.width + static_cast<int>(x) + i;
                        value += result.kernel[(j + 1) * 3 + i + 1] *
                                 (static_cast<double>(image.pixels[neighbor * image.channels + c]) - center);
                    }
                }
                if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
                    return failure("ImageValueInvalid", "PSF correction exceeds Float32 range");
                }
                result.image.pixels[pixel * image.channels + c] = static_cast<float>(value);
            }
        }
    }
    result.ok = true;
    return result;
}
} // namespace photonstack
