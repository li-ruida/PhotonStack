#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "photonstack/ImageBuffer.hpp"
#include "photonstack/Stacker.hpp"

namespace photonstack::detail {

// Keep accumulation precision independent of the Float32 input/output format.
// A finite weighted mean can have a sum outside Float32, and rounding every
// frame into Float32 can erase faint residuals before division. Storage remains
// one accumulator, independent of the number of frames; no sequence cache.
class StreamingStackAccumulator {
  public:
    StreamingStackAccumulator(const ImageBuffer& prototype, StackMethod method) : method_(method) {
        image_.width = prototype.width;
        image_.height = prototype.height;
        image_.channels = prototype.channels;
        image_.format = prototype.format;
        image_.colorEncoding = prototype.colorEncoding;
        image_.sourceBitsPerChannel = prototype.sourceBitsPerChannel;
        const double initial = method == StackMethod::Maximum   ? -std::numeric_limits<double>::infinity()
                               : method == StackMethod::Minimum ? std::numeric_limits<double>::infinity()
                                                                : 0.0;
        sums_.assign(image_.sampleCount(), initial);
        if (hasAlpha()) {
            for (std::size_t p = 0; p < image_.pixelCount(); ++p)
                sums_[p * image_.channels + 3] = 0;
        } else {
            weights_.assign(image_.pixelCount(), 0);
        }
    }

    std::uint32_t width() const { return image_.width; }
    std::uint32_t height() const { return image_.height; }
    std::size_t rowSamples() const { return static_cast<std::size_t>(image_.width) * image_.channels; }

    bool addRow(std::uint32_t row, const float* samples, std::size_t sampleCount, double weight) {
        if (row >= height() || samples == nullptr || sampleCount != rowSamples() || !std::isfinite(weight) ||
            weight <= 0)
            return false;
        for (std::size_t x = 0; x < width(); ++x) {
            const auto source = x * image_.channels;
            const auto pixel = static_cast<std::size_t>(row) * width() + x;
            const auto target = pixel * image_.channels;
            double coverage = hasAlpha() ? samples[source + 3] : 1.0;
            if (!std::isfinite(coverage) || coverage <= 1.e-6)
                continue;
            coverage = std::min(1.0, coverage);
            bool valid = true;
            for (std::uint16_t c = 0; c < colorChannels(); ++c)
                valid = valid && std::isfinite(samples[source + c]);
            if (!valid)
                continue;
            const double contribution = weight * coverage;
            for (std::uint16_t c = 0; c < colorChannels(); ++c) {
                const double value = samples[source + c];
                if (method_ == StackMethod::Maximum)
                    sums_[target + c] = std::max(sums_[target + c], value);
                else if (method_ == StackMethod::Minimum)
                    sums_[target + c] = std::min(sums_[target + c], value);
                else
                    sums_[target + c] += value * contribution;
            }
            if (hasAlpha())
                sums_[target + 3] += contribution;
            else
                weights_[pixel] += contribution;
        }
        return true;
    }

    void beginFinalization() { image_.pixels.resize(image_.sampleCount()); }

    void finalizeRow(std::uint32_t row, double totalWeight) {
        const bool extrema = method_ == StackMethod::Maximum || method_ == StackMethod::Minimum;
        for (std::size_t x = 0; x < width(); ++x) {
            const auto pixel = static_cast<std::size_t>(row) * width() + x;
            const auto offset = pixel * image_.channels;
            const double validWeight = hasAlpha() ? sums_[offset + 3] : weights_[pixel];
            for (std::uint16_t c = 0; c < colorChannels(); ++c)
                image_.pixels[offset + c] =
                    validWeight > 0 ? static_cast<float>(extrema ? sums_[offset + c] : sums_[offset + c] / validWeight)
                                    : 0.0F;
            if (hasAlpha())
                image_.pixels[offset + 3] =
                    static_cast<float>(totalWeight > 0 ? std::clamp(validWeight / totalWeight, 0.0, 1.0) : 0.0);
        }
    }

    ImageBuffer takeImage() { return std::move(image_); }

  private:
    bool hasAlpha() const { return image_.channels == 4; }
    std::uint16_t colorChannels() const { return std::min<std::uint16_t>(image_.channels, 3); }
    StackMethod method_;
    ImageBuffer image_;
    std::vector<double> sums_;
    std::vector<double> weights_;
};

} // namespace photonstack::detail
