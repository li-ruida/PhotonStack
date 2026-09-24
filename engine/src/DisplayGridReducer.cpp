#include "photonstack/DisplayGridReducer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "MaskedImageSampling.hpp"

namespace photonstack {
namespace {
constexpr int radius = 64;

DisplayGridReductionResult error(const char* code, const char* message) {
    DisplayGridReductionResult result;
    result.errorCode = code;
    result.message = message;
    return result;
}

// Half-sample symmetry: ... b a | a b ... z | z ... (also valid for size=1).
std::size_t reflected(std::int64_t index, std::uint32_t size) {
    const std::int64_t period = 2 * std::int64_t(size);
    index %= period;
    if (index < 0) index += period;
    return static_cast<std::size_t>(index < size ? index : period - 1 - index);
}

std::array<double, radius + 1> halfKernel() {
    std::array<double, radius + 1> h{};
    long double coefficient = std::ldexp(1.0L, -128);
    for (int k = 0; k <= radius; ++k) {
        h[radius - k] = static_cast<double>((k % 2 ? -1 : 1) * coefficient);
        coefficient *= static_cast<long double>(128 - k) / (k + 1);
    }
    // Symmetric coefficients with exactly zero DC to accumulation precision.
    double sides = 0;
    for (int k = 1; k <= radius; ++k) sides += h[k];
    h[0] = -2 * sides;
    return h;
}
} // namespace

DisplayGridReductionResult DisplayGridReducer::apply(
    const ImageBuffer& image, const DisplayGridReductionOptions& options) const {
    if (image.empty() || image.pixels.size() != image.sampleCount() ||
        (image.channels != 1 && image.channels != 3 && image.channels != 4))
        return error("ImageBufferInvalid", "Grid reduction requires a nonempty gray, RGB or RGBA buffer");
    if (image.colorEncoding != ColorEncoding::SRGB)
        return error("ImageEncodingInvalid", "Grid reduction requires encoded sRGB display values");
    if (!std::isfinite(options.amount) || options.amount < 0 || options.amount > 1)
        return error("ArgumentInvalid", "Grid reduction amount must be finite and in [0,1]");
    const auto channels = detail::colorChannelCount(image);
    bool fullCoverage = true;
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        const float alpha = image.channels == 4 ? image.pixels[offset + 3] : 1;
        if (!std::isfinite(alpha) || alpha < 0 || alpha > 1)
            return error("ImageValueInvalid", "Coverage must be finite and in [0,1]");
        fullCoverage &= alpha == 1;
        if (alpha <= 1.0e-6F) continue;
        for (std::uint16_t c = 0; c < channels; ++c) {
            const float value = image.pixels[offset + c];
            if (!std::isfinite(value) || value < 0 || value > 1)
                return error("ImageValueInvalid", "Covered display samples must be finite and in [0,1]");
        }
    }
    DisplayGridReductionResult result;
    result.image = image;
    result.ok = true;
    if (options.amount == 0) return result;

    const auto h = halfKernel();
    const std::size_t rowSamples = std::size_t(image.width) * channels;
    // Only one double-precision row is needed. Filtering never reads its own
    // output, and auxiliary memory does not grow with the image height.
    std::vector<double> vertical(rowSamples);
    std::vector<unsigned char> valid(image.width);
    constexpr float low = 1.0F / 65535.0F;
    constexpr float high = static_cast<float>(1.0 - 1.0 / 65535.0);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        std::fill(vertical.begin(), vertical.end(), 0);
        std::fill(valid.begin(), valid.end(), 1);
        for (int k = -radius; k <= radius; ++k) {
            const auto row = reflected(std::int64_t(y) + k, image.height) * image.width;
            const double weight = h[std::abs(k)];
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const auto offset = (row + x) * image.channels;
                const bool covered = fullCoverage || image.pixels[offset + 3] == 1;
                if (!covered) { valid[x] = 0; continue; }
                for (std::uint16_t c = 0; c < channels; ++c)
                    vertical[std::size_t(x) * channels + c] += weight * image.pixels[offset + c];
            }
        }
        const auto row = std::size_t(y) * image.width;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto offset = (row + x) * image.channels;
            for (std::uint16_t c = 0; c < channels; ++c) {
                const double base = fullCoverage || image.pixels[offset + 3] == 1 ? image.pixels[offset + c] : 0;
                vertical[std::size_t(x) * channels + c] = base - vertical[std::size_t(x) * channels + c];
            }
        }
        int missing = 0;
        if (!fullCoverage)
            for (int k = -radius; k <= radius; ++k) missing += !valid[reflected(k, image.width)];
        for (std::uint32_t x = 0; x < image.width; ++x) {
            if (!fullCoverage && x > 0) {
                missing -= !valid[reflected(std::int64_t(x) - radius - 1, image.width)];
                missing += !valid[reflected(std::int64_t(x) + radius, image.width)];
            }
            const auto pixel = row + x;
            const auto offset = pixel * image.channels;
            if (missing) {
                if (detail::pixelCoverage(image, pixel) <= 1.0e-6F) detail::clearMaskedPixel(result.image, pixel);
                continue;
            }
            std::array<double, 3> delta{};
            for (int k = -radius; k <= radius; ++k) {
                const auto source = reflected(std::int64_t(x) + k, image.width) * channels;
                for (std::uint16_t c = 0; c < channels; ++c) delta[c] += h[std::abs(k)] * vertical[source + c];
            }
            double ratio = 0;
            for (std::uint16_t c = 0; c < channels; ++c) {
                const float base = image.pixels[offset + c];
                delta[c] = options.amount * (vertical[std::size_t(x) * channels + c] - delta[c] - base);
                if (delta[c] == 0) continue;
                // Use float headroom just like the float32 display buffer.
                const double budget = delta[c] >= 0 ? high - base : base - low;
                if (budget <= 0) { ratio = INFINITY; break; }
                ratio = std::max(ratio, std::abs(delta[c]) / budget);
            }
            // A threatened endpoint protects the entire RGB pixel. In
            // particular, no still-uncomputed channel delta is used here.
            if (!std::isfinite(ratio)) continue;
            // Stable even for very small positive headroom.
            const double factor = ratio <= 1 ? std::pow(1 + std::pow(ratio, 4), -.25)
                : std::pow(1 + std::pow(1 / ratio, 4), -.25) / ratio;
            for (std::uint16_t c = 0; c < channels; ++c)
                result.image.pixels[offset + c] = static_cast<float>(image.pixels[offset + c] + delta[c] * factor);
        }
    }
    return result;
}

} // namespace photonstack
