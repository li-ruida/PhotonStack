#include "MetalCompute.hpp"
#include "photonstack/StageProfile.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <span>
#include <sys/sysctl.h>

namespace photonstack::detail {
namespace {
constexpr const char* shader = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Params {
    uint w, h, cubic;
    float dx, dy, scale, cosine, sine;
};
float4 cubicWeights(float t) {
    return float4(-.5f * t + t * t - .5f * t * t * t, 1 - 2.5f * t * t + 1.5f * t * t * t,
                  .5f * t + 2 * t * t - 1.5f * t * t * t, -.5f * t * t + .5f * t * t * t);
}
kernel void warp(device const float4* input [[buffer(0)]], device float4* output [[buffer(1)]],
                 constant Params& p [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    if (i >= p.w * p.h)
        return;
    float x = float(i % p.w) - p.dx, y = float(i / p.w) - p.dy;
    float sx = fma(p.cosine, x, p.sine * y) / p.scale;
    float sy = fma(-p.sine, x, p.cosine * y) / p.scale;
    if (!(sx >= 0 && sy >= 0 && sx <= float(p.w - 1) && sy <= float(p.h - 1))) {
        output[i] = 0;
        return;
    }
    uint ix = uint(sx), iy = uint(sy), x1 = min(ix + 1, p.w - 1), y1 = min(iy + 1, p.h - 1);
    float tx = sx - float(ix), ty = sy - float(iy);
    float4 w = float4((1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty);
    float4 v[4] = {input[iy * p.w + ix], input[iy * p.w + x1], input[y1 * p.w + ix], input[y1 * p.w + x1]};
    float3 sum = 0;
    // Compensate the sum of four coverages. Its rounded value controls
    // the exact full-coverage crop boundary downstream.
    float a = 0, residual = 0;
    for (uint k = 0; k < 4; ++k) {
        float weight = w[k] * clamp(v[k].a, 0.f, 1.f);
        float next = a + weight, z = next - a;
        residual += (a - (next - z)) + (weight - z);
        a = next;
        sum = fma(float3(weight), v[k].rgb, sum);
    }
    a += residual;
    float3 rgb = a > 1.e-6f ? sum / a : float3(0);
    if (p.cubic && ix >= 1 && iy >= 1 && ix + 2 < p.w && iy + 2 < p.h) {
        float4 wx = cubicWeights(tx), wy = cubicWeights(ty);
        float3 filtered = 0;
        bool covered = true;
        for (uint j = 0; j < 4; ++j)
            for (uint k = 0; k < 4; ++k) {
                float4 s = input[(iy + j - 1) * p.w + ix + k - 1];
                covered &= s.a == 1.f;
                filtered = fma(float3(wx[k] * wy[j]), s.rgb, filtered);
            }
        if (covered)
            rgb = clamp(filtered, min(min(v[0].rgb, v[1].rgb), min(v[2].rgb, v[3].rgb)),
                        max(max(v[0].rgb, v[1].rgb), max(v[2].rgb, v[3].rgb)));
    }
    output[i] = float4(rgb, a);
}

struct CFA {
    uint width, height;
    int colors[4];
};
kernel void demosaic(device const float* src [[buffer(0)]], device float4* output [[buffer(1)]],
                     constant CFA& p [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    int w = int(p.width), h = int(p.height), x = i % w, y = i / w;
    int phase = 2 * (y & 1) + (x & 1), native = p.colors[phase];
    float4 rgb = float4(src[i], src[i], src[i], 1);
    for (int c = 0; c < 3; ++c) {
        if (c == native)
            continue;
        if (x >= 2 && y >= 2 && x + 2 < w && y + 2 < h) {
            float cross = src[i - 2] + src[i + 2] + src[i - 2 * w] + src[i + 2 * w];
            float diag = src[i - w - 1] + src[i - w + 1] + src[i + w - 1] + src[i + w + 1];
            if (c == 1)
                rgb[c] = (4 * src[i] + 2 * (src[i - 1] + src[i + 1] + src[i - w] + src[i + w]) - cross) / 8;
            else if (native != 1)
                rgb[c] = (6 * src[i] + 2 * diag - 1.5f * cross) / 8;
            else {
                bool hor = p.colors[phase ^ 1] == c;
                int step = hor ? 1 : w, ortho = hor ? w : 1;
                rgb[c] = (5 * src[i] + 4 * (src[i - step] + src[i + step]) - (src[i - 2 * step] + src[i + 2 * step]) +
                          .5f * (src[i - 2 * ortho] + src[i + 2 * ortho]) - diag) /
                         8;
            }
        } else {
            float sum = 0;
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int sx = x + dx, sy = y + dy;
                    if (sx >= 0 && sy >= 0 && sx < w && sy < h && p.colors[2 * (sy & 1) + (sx & 1)] == c) {
                        sum += src[sy * w + sx];
                        ++n;
                    }
                }
            rgb[c] = sum / float(n);
        }
    }
    output[i] = rgb;
}
// One SIMD group per pixel/channel. Keep at most eight frame samples per
// lane in registers; frame-major reads keep the CPU tile layout unchanged.
// Q24 represents the near-unit binary32 coverage weights exactly.
float weightedMiddle(thread float* values, thread uint* weights, uint total) {
    float lo = INFINITY, hi = -INFINITY;
    for (uint k = 0; k < 8; ++k)
        if (weights[k]) {
            lo = min(lo, values[k]);
            hi = max(hi, values[k]);
        }
    lo = simd_min(lo);
    hi = simd_max(hi);
    for (uint iter = 0; iter < 32; ++iter) {
        if (lo == hi)
            return lo;
        float pivot = lo * .5f + hi * .5f;
        if (!(pivot < hi))
            pivot = lo;
        uint left = 0;
        float a = -INFINITY, b = INFINITY;
        uint aw = 0, bw = 0;
        for (uint k = 0; k < 8; ++k)
            if (weights[k]) {
                float v = values[k];
                if (v <= pivot) {
                    left += weights[k];
                    if (v > a) {
                        a = v;
                        aw = weights[k];
                    }
                } else if (v < b) {
                    b = v;
                    bw = weights[k];
                }
            }
        left = simd_sum(left);
        float lower = simd_max(a), upper = simd_min(b);
        uint wa = simd_max(a == lower ? aw : 0u), wb = simd_max(b == upper ? bw : 0u);
        // Signed distance from half the total, avoiding cancellation near
        // the midpoint. Limits below keep the subtraction representable.
        long delta = long(total) - 2L * long(left);
        if (delta < -long(wa))
            hi = lower;
        else if (delta > long(wb))
            lo = upper;
        else {
            // std::sort can permute equal values. If their weights differ,
            // only the CPU estimator can reproduce which tied sample sits
            // at the interpolation boundary.
            bool unambiguous = true;
            for (uint k = 0; k < 8; ++k) if (weights[k]) {
                if (values[k] == lower && weights[k] != wa) unambiguous = false;
                if (values[k] == upper && weights[k] != wb) unambiguous = false;
            }
            if (!simd_all(unambiguous)) return NAN;
            float t = float(delta + long(wa)) / float(wa + wb);
            return fma(upper - lower, t, lower);
        }
    }
    return NAN;
}
kernel void winsor(device const float4* input [[buffer(0)]], device float2* output [[buffer(1)]],
                   constant uint2& shape [[buffer(2)]], uint tid [[thread_position_in_grid]],
                   ushort lane [[thread_index_in_simdgroup]]) {
    uint i = tid / 32, p = i / 3, c = i % 3, total = 0;
    bool supported = true;
    float values[8];
    uint weights[8];
    for (uint k = 0; k < 8; ++k) {
        uint frame = lane + 32 * k;
        float4 v = frame < shape.y ? input[frame * shape.x + p] : float4(0);
        float w = all(isfinite(v)) ? clamp(v.a, 0.f, 1.f) : 0;
        if (w <= 1.e-6f)
            w = 0;
        weights[k] = uint(w * 16777216.f);
        supported &= float(weights[k]) / 16777216.f == w;
        values[k] = v[c];
        total += weights[k];
    }
    total = simd_sum(total);
    float2 result = float2(NAN);
    if (simd_all(supported) && total) {
        result.x = weightedMiddle(values, weights, total);
        if (isfinite(result.x)) {
            for (uint k = 0; k < 8; ++k)
                values[k] = abs(values[k] - result.x);
            result.y = weightedMiddle(values, weights, total);
        }
    }
    if (lane == 0)
        output[i] = result;
}
)metal";

enum class Kernel { Warp, CFA, Winsor };
struct MetalDevice {
    std::mutex mutex;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipelines[3]{};
    id<MTLBuffer> input, output;
    MetalDevice() {
        queue = [device newCommandQueue];
        MTLCompileOptions* opts = [MTLCompileOptions new];
        if (@available(macOS 15, iOS 18, *))
            opts.mathMode = MTLMathModeSafe;
        else
            opts.fastMathEnabled = NO;
        NSError* error = nil;
        auto library = [device newLibraryWithSource:[NSString stringWithUTF8String:shader] options:opts error:&error];
        NSArray* names = @[ @"warp", @"demosaic", @"winsor" ];
        for (NSUInteger i = 0; i < names.count; ++i) {
            auto function = [library newFunctionWithName:names[i]];
            if (function)
                pipelines[i] = [device newComputePipelineStateWithFunction:function error:&error];
        }
    }
};
MetalDevice& sharedDevice() {
    static MetalDevice gpu;
    return gpu;
}
bool fallback(const char* reason) {
    recordBackend("cpu", reason);
    return false;
}

template <class Params>
bool execute(Kernel kernel, std::span<const float> source, const Params& params, std::size_t threads,
             std::size_t outputSamples, std::vector<float>& result, const char* backend) {
    @autoreleasepool {
        @try {
            static_assert(std::is_trivially_copyable_v<Params>);
            auto& gpu = sharedDevice();
            std::lock_guard lock(gpu.mutex);
            auto pipeline = gpu.pipelines[unsigned(kernel)];
            if (!gpu.queue || !pipeline)
                return fallback("Metal device or pipeline unavailable");
            if (kernel == Kernel::Winsor && pipeline.threadExecutionWidth != 32)
                return fallback("requires 32-lane Metal SIMD groups");
            int pressure = 0;
            std::size_t length = sizeof(pressure);
            if (sysctlbyname("kern.memorystatus_vm_pressure_level", &pressure, &length, nullptr, 0) == 0 &&
                pressure > 1) {
                gpu.input = nil;
                gpu.output = nil;
                return fallback("system memory pressure");
            }
            const NSUInteger inputBytes = source.size_bytes(), outputBytes = outputSamples * sizeof(float);
            const auto limit =
                std::min<std::uint64_t>(gpu.device.maxBufferLength, gpu.device.recommendedMaxWorkingSetSize / 8);
            if (inputBytes > limit || outputBytes > limit)
                return fallback("Metal working-set budget");
            const auto grow = [&](id<MTLBuffer> buffer, NSUInteger length) -> id<MTLBuffer> {
                if (buffer.length >= length)
                    return buffer;
                return [gpu.device newBufferWithLength:length options:MTLResourceStorageModeShared];
            };
            gpu.input = grow(gpu.input, inputBytes);
            gpu.output = grow(gpu.output, outputBytes);
            if (!gpu.input || !gpu.output)
                return fallback("Metal buffer allocation failed");
            auto command = [gpu.queue commandBuffer];
            auto encoder = [command computeCommandEncoder];
            if (!encoder)
                return fallback("Metal command encoder unavailable");
            std::memcpy(gpu.input.contents, source.data(), inputBytes);
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:gpu.input offset:0 atIndex:0];
            [encoder setBuffer:gpu.output offset:0 atIndex:1];
            [encoder setBytes:&params length:sizeof(params) atIndex:2];
            [encoder dispatchThreads:MTLSizeMake(threads, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted)
                return fallback("Metal command failed");
            const float* pixels = static_cast<const float*>(gpu.output.contents);
            if (kernel != Kernel::Winsor &&
                !std::all_of(pixels, pixels + outputSamples, [](float v) { return std::isfinite(v); }))
                return fallback("Metal result exceeded finite range");
            result.assign(pixels, pixels + outputSamples);
            recordBackend(backend, "", command.GPUEndTime - command.GPUStartTime);
            return true;
        } @catch (NSException*) {
            return fallback("Metal command failed");
        }
    }
}
} // namespace
bool metalWarp(const ImageBuffer& image, SimilarityTransform transform, RegistrationInterpolation interpolation,
               ImageBuffer& result) {
    struct {
        unsigned w, h, cubic;
        float dx, dy, scale, cosine, sine;
    } params{image.width,
             image.height,
             interpolation == RegistrationInterpolation::Bicubic,
             transform.dx,
             transform.dy,
             std::max(1.e-6F, transform.scale),
             std::cos(transform.rotationRadians),
             std::sin(transform.rotationRadians)};
    if (!execute(Kernel::Warp, image.pixels, params, image.pixelCount(), image.sampleCount(), result.pixels, "metal"))
        return false;
    result.width = image.width;
    result.height = image.height;
    result.channels = image.channels;
    result.format = image.format;
    result.colorEncoding = image.colorEncoding;
    result.sourceBitsPerChannel = image.sourceBitsPerChannel;
    return true;
}
bool metalDemosaic(const std::vector<float>& mosaic, const std::array<int, 4>& colors, ImageBuffer& result,
                   bool guarded) {
    struct {
        unsigned w, h;
        std::array<int, 4> colors;
    } params{result.width, result.height, colors};
    return execute(Kernel::CFA, mosaic, params, result.pixelCount(), result.sampleCount(), result.pixels,
                   guarded ? "metal+cpu-guard" : "metal");
}
bool metalWinsorCenters(const std::vector<float>& tile, std::size_t pixels, std::size_t frames,
                        std::vector<float>& centers) {
    std::array<unsigned, 2> params{static_cast<unsigned>(pixels), static_cast<unsigned>(frames)};
    return execute(Kernel::Winsor, tile, params, pixels * 3 * 32, pixels * 6, centers, "metal+cpu-reduction");
}
} // namespace photonstack::detail
