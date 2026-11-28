#pragma once

#include "photonstack/ImageBuffer.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace photonstack::detail {

inline bool meteorPixelSample(const ImageBuffer& image, std::uint32_t x, std::uint32_t y,
                              float& luminance, float& alpha) {
    const auto offset = (std::size_t(y) * image.width + x) * image.channels;
    alpha = image.pixels[offset + 3];
    if (!std::isfinite(alpha) || alpha <= 1.e-6F) return false;
    alpha = std::clamp(alpha, 0.F, 1.F);
    const float red = image.pixels[offset], green = image.pixels[offset + 1], blue = image.pixels[offset + 2];
    if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue)) return false;
    luminance = red * .2126F + green * .7152F + blue * .0722F;
    return std::isfinite(luminance);
}

// A tile and its halo bound the two integral planes independently of frame size.
class MeteorBackground {
    std::uint32_t left_, top_, width_;
    std::vector<double> sums_, weights_;
    double sumError_ = 0., weightError_ = 0.;

  public:
    MeteorBackground(const ImageBuffer& image, std::uint32_t left, std::uint32_t top, std::uint32_t right,
                     std::uint32_t bottom)
        : left_(left), top_(top), width_(right - left + 2), sums_(std::size_t(width_) * (bottom - top + 2)),
          weights_(sums_.size()) {
        double absoluteSum = 0.;
        for (auto y = top; y <= bottom; ++y) {
            double sum = 0., weight = 0.;
            for (auto x = left; x <= right; ++x) {
                float luminance = 0.F, alpha = 0.F;
                if (meteorPixelSample(image, x, y, luminance, alpha)) {
                    const double value = double(luminance) * double(alpha);
                    sum += value;
                    weight += double(alpha);
                    absoluteSum += std::abs(value);
                }
                const auto i = std::size_t(y - top + 1) * width_ + x - left + 1;
                sums_[i] = sums_[i - width_] + sum;
                weights_[i] = weights_[i - width_] + weight;
            }
        }
        // Each prefix follows at most width + height additions. Cover errors in
        // all four prefixes, their subtraction and this magnitude estimate.
        // An absolute bound also covers cancellation in signed scientific data.
        const double factor = 8 * std::numeric_limits<double>::epsilon() * (double(width_) + bottom - top + 2);
        sumError_ = absoluteSum * factor;
        weightError_ = weights_.back() * factor;
    }

    float at(const ImageBuffer& image, std::uint32_t x, std::uint32_t y, std::uint32_t radius, float fallback) const {
        const auto minX = x > radius ? x - radius : 0;
        const auto minY = y > radius ? y - radius : 0;
        const auto maxX = std::uint32_t(std::min<std::uint64_t>(image.width - 1, std::uint64_t(x) + radius));
        const auto maxY = std::uint32_t(std::min<std::uint64_t>(image.height - 1, std::uint64_t(y) + radius));
        const auto left = minX - left_, top = minY - top_;
        const auto right = maxX - left_ + 1, bottom = maxY - top_ + 1;
        const auto rectangle = [&](const auto& values) {
            return (values[std::size_t(bottom) * width_ + right] - values[std::size_t(top) * width_ + right]) -
                   (values[std::size_t(bottom) * width_ + left] - values[std::size_t(top) * width_ + left]);
        };
        double sum = rectangle(sums_), weight = rectangle(weights_);
        // Ordinary normalized tiles retain O(1) queries. If HDR values outside
        // this rectangle can swamp its background or coverage, recover the
        // original row-major sum instead of publishing a cancellation artifact.
        if (weightError_ > std::abs(weight) * 1.e-7 ||
            sumError_ > std::max(std::abs(sum) * 1.e-7, std::abs(weight) * 1.e-12)) {
            sum = 0.; weight = 0.;
            for (auto yy = minY; yy <= maxY; ++yy) for (auto xx = minX; xx <= maxX; ++xx) {
                float luminance = 0.F, alpha = 0.F;
                if (meteorPixelSample(image, xx, yy, luminance, alpha)) {
                    sum += double(luminance) * double(alpha);
                    weight += double(alpha);
                }
            }
        }
        return weight <= 1.e-9 ? fallback : float(sum / weight);
    }
};

} // namespace photonstack::detail
