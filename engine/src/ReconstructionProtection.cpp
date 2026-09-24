#include "photonstack/ReconstructionProtection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>
#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {
ReconstructionProtectionResult error(std::string code, std::string message) {
    ReconstructionProtectionResult result;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}
bool validShape(const ImageBuffer& im) {
    return !im.empty() && (im.channels == 1 || im.channels == 3 || im.channels == 4) &&
           im.sampleCount() == im.pixels.size();
}
// Half-sample reflection, including dimensions shorter than the kernel radius.
std::size_t reflected(std::int64_t coordinate, std::uint32_t size) {
    const auto period = 2 * static_cast<std::int64_t>(size);
    coordinate %= period;
    if (coordinate < 0) coordinate += period;
    return static_cast<std::size_t>(coordinate < size ? coordinate : period - coordinate - 1);
}
std::vector<double> blur(const std::vector<double>& input, std::uint32_t width,
                         std::uint32_t height, const std::vector<double>& kernel) {
    std::vector<double> temp(input.size()), output(input.size());
    const int radius = static_cast<int>(kernel.size() / 2);
    // Vertical then horizontal matches the reference scientific implementation.
    for (std::uint32_t y = 0; y < height; ++y) {
        for (int k = -radius; k <= radius; ++k) {
            const auto source = reflected(static_cast<std::int64_t>(y) + k, height) * width;
            const auto target = static_cast<std::size_t>(y) * width;
            const double weight = kernel[static_cast<std::size_t>(k + radius)];
            for (std::uint32_t x = 0; x < width; ++x)
                temp[target + x] += weight * input[source + x];
        }
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto row = static_cast<std::size_t>(y) * width;
        for (std::uint32_t x = 0; x < width; ++x) {
            double sum = 0;
            for (int k = -radius; k <= radius; ++k)
                sum += kernel[static_cast<std::size_t>(k + radius)] *
                       temp[row + reflected(static_cast<std::int64_t>(x) + k, width)];
            output[row + x] = sum;
        }
    }
    return output;
}
} // namespace

ReconstructionProtectionResult ReconstructionProtection::apply(
    const ImageBuffer& reconstructed, const ImageBuffer& reference, const ImageBuffer& mask,
    const ReconstructionProtectionOptions& options) const {
    if (!validShape(reconstructed) || !validShape(reference) || !validShape(mask) ||
        reconstructed.width != reference.width || reconstructed.height != reference.height ||
        reconstructed.width != mask.width || reconstructed.height != mask.height ||
        detail::colorChannelCount(reconstructed) != detail::colorChannelCount(reference))
        return error("ImageBufferInvalid", "Protection requires matching image grids and compatible color channels");
    if (reconstructed.colorEncoding != ColorEncoding::Linear || reference.colorEncoding != ColorEncoding::Linear)
        return error("ImageEncodingInvalid", "Reconstruction protection requires linear scientific images");
    if (!std::isfinite(options.amount) || options.amount < 0 || options.amount > 1 ||
        !std::isfinite(options.backgroundSigma) || options.backgroundSigma < .25 || options.backgroundSigma > 64)
        return error("ArgumentInvalid", "Protection amount must be in [0,1] and background sigma in [0.25,64]");
    if (detail::imageHasInvalidCoveredColor(reconstructed) || detail::imageHasInvalidCoveredColor(reference) ||
        detail::imageHasInvalidCoveredColor(mask))
        return error("ImageValueInvalid", "Protection inputs contain invalid covered samples");

    const auto count = reconstructed.pixelCount();
    std::vector<double> weights(count), coverage(count);
    std::size_t protectedPixels = 0;
    bool allCovered = true;
    for (std::size_t p = 0; p < count; ++p) {
        const auto mo = p * mask.channels;
        const double maskCoverage = detail::pixelCoverage(mask, p);
        double weight = 0;
        if (maskCoverage > 1e-6) {
            const float value = mask.pixels[mo];
            if (value < 0 || value > 1)
                return error("MaskValueInvalid", "Protection mask weights must be in [0,1]");
            for (unsigned c = 1; c < detail::colorChannelCount(mask); ++c)
                if (std::abs(mask.pixels[mo + c] - value) > 1e-6F)
                    return error("MaskValueInvalid", "Protection mask must be grayscale");
            weight = value * maskCoverage * options.amount;
        }
        const double sourceCoverage = detail::pixelCoverage(reconstructed, p);
        const double referenceCoverage = detail::pixelCoverage(reference, p);
        coverage[p] = std::min(sourceCoverage, referenceCoverage);
        allCovered = allCovered && coverage[p] == 1;
        if (sourceCoverage <= 1e-6 || weight == 0) continue;
        if (referenceCoverage <= 1e-6)
            return error("ReferenceCoverageMissing", "Reference does not cover an active protection pixel");
        weights[p] = weight;
        ++protectedPixels;
    }
    ReconstructionProtectionResult result;
    result.image = reconstructed;
    result.protectedPixels = protectedPixels;
    if (protectedPixels == 0) { result.ok = true; return result; }

    const int radius = static_cast<int>(std::floor(4 * options.backgroundSigma + .5));
    std::vector<double> kernel(static_cast<std::size_t>(2 * radius + 1));
    double sum = 0;
    for (int k = -radius; k <= radius; ++k) {
        const double value = std::exp(-.5 * k * k / (options.backgroundSigma * options.backgroundSigma));
        kernel[static_cast<std::size_t>(k + radius)] = value;
        sum += value;
    }
    for (auto& value : kernel) value /= sum;
    const auto support = allCovered ? std::vector<double>{}
                                    : blur(coverage, reconstructed.width, reconstructed.height, kernel);
    std::vector<double> difference(count), weighted(count);
    for (unsigned c = 0; c < detail::colorChannelCount(reconstructed); ++c) {
        for (std::size_t p = 0; p < count; ++p) {
            difference[p] = coverage[p] > 1e-6
                ? static_cast<double>(reference.pixels[p * reference.channels + c]) -
                      reconstructed.pixels[p * reconstructed.channels + c]
                : 0;
            weighted[p] = difference[p] * coverage[p];
        }
        const auto smooth = blur(weighted, reconstructed.width, reconstructed.height, kernel);
        for (std::size_t p = 0; p < count; ++p) {
            if (weights[p] == 0) continue; // Preserve every untouched float bit.
            const double background = smooth[p] / (allCovered ? 1.0 : support[p]);
            const auto offset = p * reconstructed.channels + c;
            const double value = reconstructed.pixels[offset] + weights[p] * (difference[p] - background);
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                return error("ImageValueInvalid", "Protected scientific value exceeds float32 range");
            result.image.pixels[offset] = static_cast<float>(value);
        }
    }
    result.ok = true;
    return result;
}
} // namespace photonstack
