#include "photonstack/NonlocalDenoiser.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace photonstack {
namespace {
NonlocalDenoiseResult error(const char* message) {
    NonlocalDenoiseResult r;
    r.errorCode = "ArgumentInvalid";
    r.message = message;
    return r;
}
std::uint32_t reflect(std::int64_t p, std::uint32_t size) {
    if (size == 1) return 0;
    const std::int64_t period = 2 * (std::int64_t(size) - 1);
    p %= period;
    if (p < 0) p += period;
    return static_cast<std::uint32_t>(p < size ? p : period - p);
}
bool completeFinite(const ImageBuffer& a) {
    if (a.empty() || a.pixels.size() != a.sampleCount()) return false;
    for (std::size_t p = 0; p < a.pixelCount(); ++p) {
        for (std::uint16_t c = 0; c < a.channels; ++c)
            if (!std::isfinite(a.pixels[p * a.channels + c])) return false;
        if (a.channels == 4 && a.pixels[p * 4 + 3] != 1) return false;
    }
    return true;
}

ImageBuffer conservativeRGB(const ImageBuffer& image, const ImageBuffer& blend,
                           const NonlocalDenoiseOptions& options, double weightSum) {
    ImageBuffer output = image;
    constexpr std::size_t tile = 128, halo = 16;
    struct Edge { int dx, dy; std::vector<double> weights; };
    const auto sample = [&image](std::int64_t x, std::int64_t y, int c) {
        return double(image.pixels[(std::size_t(reflect(y, image.height)) * image.width +
                                    reflect(x, image.width)) * image.channels + c]);
    };
    for (std::size_t ty = 0; ty < image.height; ty += tile) {
        for (std::size_t tx = 0; tx < image.width; tx += tile) {
            const auto right = std::min(tx + tile, std::size_t(image.width));
            const auto bottom = std::min(ty + tile, std::size_t(image.height));
            bool active = false;
            for (auto y = ty; y < bottom && !active; ++y)
                for (auto x = tx; x < right; ++x)
                    active |= blend.pixels[(y * image.width + x) * blend.channels] > 0;
            if (!active) continue;
            const auto x0 = tx > halo ? tx - halo : 0, y0 = ty > halo ? ty - halo : 0;
            const auto x1 = std::min(right + halo, std::size_t(image.width));
            const auto y1 = std::min(bottom + halo, std::size_t(image.height));
            const int width = static_cast<int>(x1 - x0), height = static_cast<int>(y1 - y0);
            const auto count = std::size_t(width) * height;
            std::vector<std::array<double, 3>> rgb(count), result(count);
            std::vector<double> mask(count), degree(count, 1);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const auto i = std::size_t(y) * width + x;
                const auto pixel = (y0 + y) * image.width + x0 + x;
                for (int c = 0; c < 3; ++c) rgb[i][c] = image.pixels[pixel * image.channels + c];
                mask[i] = blend.pixels[pixel * blend.channels];
            }
            result = rgb;
            std::vector<Edge> edges;
            edges.reserve(60);
            const auto stride = std::size_t(width) + 7;
            std::vector<double> integral((height + 7) * stride);
            // Each undirected edge is evaluated once. Degrees include all
            // incident edges, even when the blend map blocks their transfer.
            for (int dy = 0; dy <= 5; ++dy) for (int dx = -5; dx <= 5; ++dx) {
                if (dy == 0 && dx <= 0) continue;
                std::fill(integral.begin(), integral.end(), 0);
                for (int y = 0; y < height + 6; ++y) {
                    double row = 0;
                    for (int x = 0; x < width + 6; ++x) {
                        const auto gx = std::int64_t(x0) + x - 3, gy = std::int64_t(y0) + y - 3;
                        double distance = 0;
                        for (int c = 0; c < 3; ++c) {
                            const double d = sample(gx + dx, gy + dy, c) - sample(gx, gy, c);
                            distance += options.luminanceWeights[c] * d * d;
                        }
                        row += weightSum * distance;
                        integral[(y + 1) * stride + x + 1] = integral[y * stride + x + 1] + row;
                    }
                }
                Edge edge{dx, dy, std::vector<double>(count)};
                for (int y = 0; y < height - dy; ++y)
                    for (int x = std::max(0, -dx); x < std::min(width, width - dx); ++x) {
                        const auto i = std::size_t(y) * width + x;
                        const auto j = std::size_t(y + dy) * width + x + dx;
                        const double distance = std::max(0.0, (integral[(y + 7) * stride + x + 7] -
                            integral[y * stride + x + 7] - integral[(y + 7) * stride + x] +
                            integral[y * stride + x]) / 49);
                        const double normalized = std::sqrt(distance) / options.h;
                        const double weight = std::exp(-normalized * normalized);
                        edge.weights[i] = weight;
                        degree[i] += weight;
                        degree[j] += weight;
                    }
                edges.push_back(std::move(edge));
            }
            // q_ij is symmetric and sum_j(q_ij) <= mask_i <= 1. Thus each
            // output is a convex combination, with no transfer into a protected
            // pixel and no channel-sum change before final float32 rounding.
            for (const auto& edge : edges)
                for (int y = 0; y < height - edge.dy; ++y)
                    for (int x = std::max(0, -edge.dx); x < std::min(width, width - edge.dx); ++x) {
                        const auto i = std::size_t(y) * width + x;
                        const auto j = std::size_t(y + edge.dy) * width + x + edge.dx;
                        const double q = std::min(mask[i], mask[j]) * edge.weights[i] / std::max(degree[i], degree[j]);
                        for (int c = 0; c < 3; ++c) {
                            const double delta = q * (rgb[j][c] - rgb[i][c]);
                            result[i][c] += delta;
                            result[j][c] -= delta;
                        }
                    }
            // A degree depends on five pixels beyond the patch/search support;
            // the 16px halo exceeds the required 13px and isolates tile joins.
            for (auto y = ty; y < bottom; ++y) for (auto x = tx; x < right; ++x) {
                const auto i = (y - y0) * width + x - x0;
                if (mask[i] == 0) continue;
                const auto pixel = (y * image.width + x) * image.channels;
                for (int c = 0; c < 3; ++c) output.pixels[pixel + c] = static_cast<float>(result[i][c]);
            }
        }
    }
    return output;
}
}
NonlocalDenoiseResult NonlocalDenoiser::apply(const ImageBuffer& image, const ImageBuffer& blend,
                                            const NonlocalDenoiseOptions& options) const {
    if ((image.channels != 3 && image.channels != 4) || !completeFinite(image))
        return error("Nonlocal denoising requires finite RGB with complete coverage");
    if ((blend.channels != 1 && blend.channels != 3 && blend.channels != 4) || !completeFinite(blend) ||
        image.width != blend.width || image.height != blend.height)
        return error("Blend map must have matching dimensions and complete coverage");
    if (!std::isfinite(options.h) || options.h < 0)
        return error("Nonlocal h must be finite and nonnegative");
    if (options.mode != NonlocalDenoiseMode::Luminance && options.mode != NonlocalDenoiseMode::ConservativeRGB)
        return error("Invalid nonlocal denoising mode");
    double totalWeight = 0;
    for (double weight : options.luminanceWeights) {
        if (!std::isfinite(weight) || weight < 0 || weight > 1024)
            return error("Luminance weights must be finite and in [0,1024]");
        totalWeight += weight;
    }
    if (totalWeight == 0) return error("At least one luminance weight must be positive");
    for (std::size_t p = 0; p < blend.pixelCount(); ++p) {
        const auto offset = p * blend.channels;
        const float m = blend.pixels[offset];
        if (m < 0 || m > 1 || (blend.channels >= 3 &&
            (blend.pixels[offset + 1] != m || blend.pixels[offset + 2] != m)))
            return error("Blend map must be grayscale with weights in [0,1]");
    }
    NonlocalDenoiseResult result;
    result.image = image;
    result.ok = true;
    if (options.h == 0) return result;
    if (options.mode == NonlocalDenoiseMode::ConservativeRGB) {
        result.image = conservativeRGB(image, blend, options, totalWeight);
        return result;
    }
    constexpr std::size_t tile = 128;
    for (std::size_t ty = 0; ty < image.height; ty += tile) {
        for (std::size_t tx = 0; tx < image.width; tx += tile) {
            const auto w = std::min(tile, image.width - tx), h = std::min(tile, image.height - ty);
            bool active = false;
            for (std::size_t y = 0; y < h && !active; ++y)
                for (std::size_t x = 0; x < w; ++x)
                    active |= blend.pixels[((ty + y) * image.width + tx + x) * blend.channels] > 0;
            if (!active) continue;
            const auto ew = w + 16, eh = h + 16;
            std::vector<std::array<double, 3>> rgb(ew * eh), accum(w * h);
            std::vector<double> luminance(ew * eh), norm(w * h, 1);
            for (std::size_t y = 0; y < eh; ++y) {
                for (std::size_t x = 0; x < ew; ++x) {
                    const auto source = (std::size_t(reflect(std::int64_t(ty + y) - 8, image.height)) * image.width +
                        reflect(std::int64_t(tx + x) - 8, image.width)) * image.channels;
                    for (int c = 0; c < 3; ++c) {
                        rgb[y * ew + x][c] = image.pixels[source + c];
                        luminance[y * ew + x] += rgb[y * ew + x][c] * options.luminanceWeights[c];
                    }
                }
            }
            for (std::size_t y = 0; y < h; ++y)
                for (std::size_t x = 0; x < w; ++x) accum[y * w + x] = rgb[(y + 8) * ew + x + 8];
            // Summed-area squared differences make each patch query constant
            // time. Local tiles bound both memory and cancellation error.
            const auto stride = w + 7;
            std::vector<double> integral((h + 7) * stride);
            for (int dy = -5; dy <= 5; ++dy) {
                for (int dx = -5; dx <= 5; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    std::fill(integral.begin(), integral.end(), 0);
                    for (std::size_t y = 0; y < h + 6; ++y) {
                        double rowSum = 0;
                        for (std::size_t x = 0; x < w + 6; ++x) {
                            const double d = luminance[(y + 5) * ew + x + 5] -
                                luminance[(y + 5 + dy) * ew + x + 5 + dx];
                            rowSum += d * d;
                            integral[(y + 1) * stride + x + 1] = integral[y * stride + x + 1] + rowSum;
                        }
                    }
                    for (std::size_t y = 0; y < h; ++y) {
                        for (std::size_t x = 0; x < w; ++x) {
                            const auto pixel = (ty + y) * image.width + tx + x;
                            if (blend.pixels[pixel * blend.channels] == 0) continue;
                            const double distance = std::max(0.0, (integral[(y + 7) * stride + x + 7] -
                                integral[y * stride + x + 7] - integral[(y + 7) * stride + x] +
                                integral[y * stride + x]) / 49);
                            const double normalized = std::sqrt(distance) / options.h;
                            const double weight = std::exp(-normalized * normalized);
                            const auto& sample = rgb[(y + 8 + dy) * ew + x + 8 + dx];
                            for (int c = 0; c < 3; ++c) accum[y * w + x][c] += weight * sample[c];
                            norm[y * w + x] += weight;
                        }
                    }
                }
            }
            for (std::size_t y = 0; y < h; ++y) {
                for (std::size_t x = 0; x < w; ++x) {
                    const auto pixel = (ty + y) * image.width + tx + x;
                    const double m = blend.pixels[pixel * blend.channels];
                    if (m == 0) continue;
                    for (int c = 0; c < 3; ++c) {
                        const double original = image.pixels[pixel * image.channels + c];
                        result.image.pixels[pixel * image.channels + c] = static_cast<float>(original +
                            m * (accum[y * w + x][c] / norm[y * w + x] - original));
                    }
                }
            }
        }
    }
    return result;
}
} // namespace photonstack
