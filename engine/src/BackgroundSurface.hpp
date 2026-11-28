#pragma once
#include "MaskedImageSampling.hpp"
#include "photonstack/BackgroundExtractor.hpp"
#include "photonstack/ParallelRanges.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>

namespace photonstack::detail {
BackgroundExtractionResult measureBackgroundSamples(const ImageBuffer&, const BackgroundPolynomialOptions&);

inline BackgroundExtractionResult backgroundSurfaceError(const char* code, const char* message) {
    BackgroundExtractionResult result;
    result.errorCode = code;
    result.message = message;
    return result;
}
inline double backgroundMedian(std::vector<double> values) {
    auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}
// Evaluator uses zero-based pixel centers, returns all channels without allocation.
template <typename Evaluator, typename BlockEvaluator = std::nullptr_t>
BackgroundExtractionResult applyBackgroundSurface(const ImageBuffer& im, const BackgroundPolynomialOptions& opt,
                                                  BackgroundExtractionResult result, Evaluator evaluate, BlockEvaluator block = nullptr) {
    const int channels = std::min<int>(3, im.channels);
    std::array<std::vector<double>, 3> fitted;
    std::array<std::size_t, 3> used{};
    for (auto& m : result.measurements) {
        if (m.status != "sampled")
            continue;
        m.fitted = evaluate(std::lround(m.point.x), std::lround(m.point.y));
        const auto count = std::count(m.used.begin(), m.used.begin() + channels, true);
        m.status = count == channels ? "used" : count ? "partial" : "outlier";
        ++result.sampledGridCells;
        for (int c = 0; c < channels; ++c) {
            if (!std::isfinite(m.fitted[c]))
                return backgroundSurfaceError("ImageValueInvalid", "Nonfinite background model");
            fitted[c].push_back(m.fitted[c]);
            if (m.used[c]) {
                const auto residual = m.measured[c] - m.fitted[c];
                result.residualRMS[c] += residual * residual;
                result.validationRMS[c] += m.validationResidual[c] * m.validationResidual[c];
                ++used[c];
            }
        }
    }
    std::array<double, 3> pedestal{};
    for (int c = 0; c < channels; ++c) {
        if (fitted[c].empty())
            return backgroundSurfaceError("BackgroundSamplesInsufficient", "No usable sky samples");
        pedestal[c] = backgroundMedian(fitted[c]);
        result.residualRMS[c] = std::sqrt(result.residualRMS[c] / std::max<std::size_t>(1, used[c]));
        result.validationRMS[c] = std::sqrt(result.validationRMS[c] / std::max<std::size_t>(1, used[c]));
    }
    result.image = im;
    result.background = float(pedestal[0]);
    result.gridColumns = opt.columns;
    result.gridRows = opt.rows;
    if (opt.diagnostics) {
        result.modelImage = im;
        result.differenceImage = im;
    }
    std::atomic<bool> invalid{false};
    parallelRanges(im.height, 16, [&](std::size_t begin, std::size_t end) noexcept {
        constexpr std::uint32_t blockSize = 64;
        std::array<std::array<double, 3>, blockSize> colors;
        for (auto y = begin; y < end; ++y) {
            for (std::size_t start = 0; start < im.width; start += blockSize) {
                const auto count = std::min<std::size_t>(blockSize, im.width - start);
                if constexpr (!std::is_same_v<BlockEvaluator, std::nullptr_t>) block(start, y, int(count), colors);
                for (auto x = start; x < start + count; ++x) {
                    const auto p = y * im.width + x;
                    if (!pixelHasValidColor(im, p)) {
                        clearMaskedPixel(result.image, p);
                        if (opt.diagnostics) {
                            clearMaskedPixel(result.modelImage, p);
                            clearMaskedPixel(result.differenceImage, p);
                        }
                        continue;
                    }
                    const auto model = [&] {
                        if constexpr (std::is_same_v<BlockEvaluator, std::nullptr_t>) return evaluate(x, y);
                        else return colors[x - start];
                    }();
                    for (int c = 0; c < channels; ++c) {
                        const auto o = p * im.channels + c;
                        const double value =
                            im.pixels[o] -
                            opt.extraction.strength * (model[c] - (opt.extraction.preserveBrightness ? pedestal[c] : 0));
                        const float out = float(opt.extraction.clampOutput ? std::clamp(value, 0.0, 1.0) : value);
                        const double difference = double(im.pixels[o]) - out;
                        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max() ||
                            !std::isfinite(model[c]) || std::abs(model[c]) > std::numeric_limits<float>::max() ||
                            !std::isfinite(difference) || std::abs(difference) > std::numeric_limits<float>::max()) {
                            invalid.store(true, std::memory_order_relaxed);
                            continue;
                        }
                        result.image.pixels[o] = out;
                        if (opt.diagnostics) {
                            result.modelImage.pixels[o] = float(model[c]);
                            result.differenceImage.pixels[o] = float(difference);
                        }
                    }
                }
            }
        }
    });
    if (invalid.load())
        return backgroundSurfaceError("ImageValueInvalid", "Background correction exceeds Float32 range");
    if (!opt.diagnostics)
        result.measurements.clear();
    result.ok = true;
    return result;
}
} // namespace photonstack::detail
