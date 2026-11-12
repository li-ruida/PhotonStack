#include "MetalCompute.hpp"
#include "photonstack/StageProfile.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <span>
#include <sys/sysctl.h>
#include <unistd.h>

namespace photonstack::detail {
namespace {
constexpr const char* shader = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Params {
    uint w, h, cubic, mapping;
    float dx, dy, scale, cosine, sine, invA, invB, invC, invD;
};
float4 cubicWeights(float t) {
    return float4(-.5f * t + t * t - .5f * t * t * t, 1 - 2.5f * t * t + 1.5f * t * t * t,
                  .5f * t + 2 * t * t - 1.5f * t * t * t, -.5f * t * t + .5f * t * t * t);
}
kernel void warp(device const float4* input [[buffer(0)]], device float4* output [[buffer(1)]],
                 constant Params& p [[buffer(2)]], device const float2* coords [[buffer(3)]], uint i [[thread_position_in_grid]]) {
    if (i >= p.w * p.h)
        return;
    float x = float(i % p.w) - p.dx, y = float(i / p.w) - p.dy;
    float sx = fma(p.cosine, x, p.sine * y) / p.scale;
    float sy = fma(-p.sine, x, p.cosine * y) / p.scale;
    if (p.mapping == 1) {
        sx = fma(p.invA, x, p.invB * y);
        sy = fma(p.invC, x, p.invD * y);
    } else if (p.mapping == 2) {
        sx = coords[i].x;
        sy = coords[i].y;
    }
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
struct ScientificCFA { uint w,h; int colors[4]; float gains[3]; };
// A compensated numerator and a rounding interval preserve the CPU scientific
// stencil for non-integer mosaics. Ambiguous outputs use the existing scalar
// stencil, which also handles nonfinite neighbors and the Bayer border.
kernel void scientificCFA(device const float* src [[buffer(0)]], device float4* dst [[buffer(1)]],
                          constant ScientificCFA& p [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    int x=i%p.w,y=i/p.w,w=p.w;
    if (x<2 || y<2 || x+2>=w || y+2>=int(p.h)) { dst[i]=NAN; return; }
    int phase=2*(y&1)+(x&1), native=p.colors[phase];
    const int dx[13]={0,-1,1,0,0,-2,2,0,0,-1,1,-1,1};
    const int dy[13]={0,0,0,-1,1,0,0,-2,2,-1,-1,1,1};
    float4 rgb=src[i];
    for (int c=0;c<3;++c) {
        if (c==native) continue;
        float sum=0, tail=0, magnitude=0;
        bool horizontal=p.colors[phase^1]==c;
        for (int j=0;j<13;++j) {
            float factor;
            if (c==1) factor=j==0 ? 4 : j<=4 ? 2 : j<=8 ? -1 : 0;
            else if (native!=1) factor=j==0 ? 6 : j<=4 ? 0 : j<=8 ? -1.5f : 2;
            else if (j==0) factor=5;
            else if (j<=4) factor=((dy[j]==0)==horizontal) ? 4 : 0;
            else if (j<=8) factor=((dy[j]==0)==horizontal) ? -1 : .5f;
            else factor=-1;
            if (!factor) continue;
            int sx=x+dx[j],sy=y+dy[j];
            float sample=src[sy*w+sx], gain=p.gains[p.colors[(sy&1)*2+(sx&1)]];
            float scaled=sample*gain, term=scaled*factor;
            float termTail=fma(scaled,factor,-term)+fma(sample,gain,-scaled)*factor;
            float next=sum+term,z=next-sum;
            tail += ((sum-(next-z))+(term-z))+termTail;
            sum=next;
            magnitude+=abs(term);
        }
        float divisor=8*p.gains[c], q=sum/divisor;
        float correction=(fma(-q,divisor,sum)+tail)/divisor, v=q+correction;
        float residual=(q-v)+correction;
        float gap=abs(nextafter(v,residual<0 ? -INFINITY : INFINITY)-v);
        if (!isfinite(v) || abs(residual)+magnitude/divisor*2.e-13f >= .5f*gap) {
            dst[i]=NAN; return;
        }
        rgb[c]=v;
    }
    dst[i]=rgb;
}
struct LuminanceParams { float hi[3], lo[3]; };
kernel void guardLuminance(device const float4* src [[buffer(0)]], device float* dest [[buffer(1)]],
                           constant LuminanceParams& coeff [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    float3 rgb = src[i].rgb, hi(coeff.hi[0],coeff.hi[1],coeff.hi[2]), lo(coeff.lo[0],coeff.lo[1],coeff.lo[2]);
    float3 terms = rgb * hi, tails = fma(rgb,hi,-terms) + rgb * lo;
    float sum = terms.x + terms.y, z = sum - terms.x;
    float residue = (terms.x - (sum-z)) + (terms.y-z);
    float value = sum + terms.z;
    z = value-sum;
    residue += (sum-(value-z)) + (terms.z-z);
    residue += tails.x + tails.y + tails.z;
    // Resolve a rounding tie, cancellation or overflow using the native
    // double expression; ordinary samples are already correctly rounded.
    float gap = abs(nextafter(value,residue < 0 ? -INFINITY : INFINITY) - value);
    float tolerance = (abs(terms.x) + abs(terms.y) + abs(terms.z)) * 8.e-14f;
    dest[i] = isfinite(value) && abs(residue) + tolerance < gap*.5f ? value : NAN;
}

// One SIMD group sorts the native samples in a single CFA annulus/channel.
struct SkyParams { uint width; int colors[4]; };
kernel void guardSky(device const float* mosaic [[buffer(0)]], device float2* output [[buffer(1)]],
                     constant SkyParams& p [[buffer(2)]], device const float4* apertures [[buffer(3)]],
                     uint gid [[threadgroup_position_in_grid]], ushort lane [[thread_index_in_simdgroup]]) {
    const float4 a = apertures[gid/3];
    const int cx = int(floor(a.x+.5f)), cy = int(floor(a.y+.5f)), outer = int(a.w), side = 2*outer+1;
    const uint channel = gid%3;
    threadgroup float data[512];
    threadgroup atomic_uint length;
    threadgroup atomic_uint uncertain;
    if (!lane) { atomic_store_explicit(&length,0,memory_order_relaxed); atomic_store_explicit(&uncertain,0,memory_order_relaxed); }
    for (uint j = lane; j < 512; j+=32) data[j]=INFINITY;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (int j=lane; j<side*side; j+=32) {
        int x=cx+j%side-outer, y=cy+j/side-outer;
        if (p.colors[(y&1)*2+(x&1)] != int(channel)) continue;
        float dx=float(x)-a.x, dy=float(y)-a.y, d=dx*dx+dy*dy;
        float inner=(a.z+4.f)*(a.z+4.f), outside=float(outer*outer);
        // Exact integer boundary at the outer edge is unambiguous. Close
        // fractional radii are resolved by the CPU's original hypot test.
        if (abs(d-inner)<.001f || (abs(d-outside)<.001f && (dx!=rint(dx) || dy!=rint(dy))))
            atomic_store_explicit(&uncertain,1,memory_order_relaxed);
        if (d<=outside && d>inner) {
            uint pos=atomic_fetch_add_explicit(&length,1,memory_order_relaxed);
            if (pos<512) data[pos]=mosaic[y*p.width+x];
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint n=atomic_load_explicit(&length,memory_order_relaxed);
    if (n<12 || n>512 || atomic_load_explicit(&uncertain,memory_order_relaxed)) {
        if (!lane) output[gid]=float2(NAN);
        return;
    }
    float sky=0;
    for (uint pass=0;pass<2;++pass) {
        for (uint k=2;k<=512;k*=2)
            for (uint stride=k/2;stride;stride/=2) {
                for (uint j=lane;j<256;j+=32) {
                    uint l=(j/stride)*(2*stride)+j%stride, r=l+stride;
                    float x=data[l],y=data[r];
                    data[l]=(l&k) ? max(x,y) : min(x,y);
                    data[r]=(l&k) ? min(x,y) : max(x,y);
                }
                threadgroup_barrier(mem_flags::mem_threadgroup);
            }
        float value=data[n/2];
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (!pass) {
            sky=value;
            for (uint j=lane;j<n;j+=32) data[j]=abs(data[j]-sky);
            threadgroup_barrier(mem_flags::mem_threadgroup);
        } else if (!lane) output[gid]=float2(sky,value);
    }
}
// A mask keeps the CPU's double-precision centroid, sky and coverage decisions intact.
kernel void peaks(device const float* src [[buffer(0)]], device uint* mask [[buffer(1)]],
                  constant float& threshold [[buffer(2)]], uint word [[thread_position_in_grid]]) {
    uint bits = 0;
    for (uint i = 0; i < 32; ++i)
        if (src[word * 32 + i] >= threshold) bits |= 1u << i;
    mask[word] = bits;
}
struct CalibrationParams {
    uint dark, flat, lightBias, flatBias, clampNegative;
    float epsilon, means[3], meanTails[3];
};
kernel void calibration(device const float4* light [[buffer(0)]], device float4* output [[buffer(1)]],
                        constant CalibrationParams& p [[buffer(2)]], device const float4* dark [[buffer(3)]],
                        device const float4* bias [[buffer(4)]], device const float4* flat [[buffer(5)]],
                        uint i [[thread_position_in_grid]]) {
    float4 v = light[i];
    float3 error = 0;
    float a = isfinite(v.a) ? saturate(v.a) : 0;
    if (p.dark) {
        float4 d = dark[i];
        a = min(a, isfinite(d.a) ? saturate(d.a) : 0.f);
        float3 difference = v.rgb - d.rgb, z = difference - v.rgb;
        error = (v.rgb - (difference - z)) + (-d.rgb - z);
        v.rgb = difference;
    }
    float3 b = 0;
    if (p.lightBias || p.flatBias) {
        float4 sample = bias[i];
        a = min(a, isfinite(sample.a) ? saturate(sample.a) : 0.f);
        b = sample.rgb;
        if (p.lightBias) {
            float3 difference = v.rgb - b, z = difference - v.rgb;
            error += (v.rgb - (difference-z)) + (-b-z);
            v.rgb = difference;
        }
    }
    if (p.flat) {
        float4 f = flat[i];
        a = min(a, isfinite(f.a) ? saturate(f.a) : 0.f);
        float3 biasValue = p.flatBias ? b : float3(0);
        float3 flatValue = f.rgb - biasValue, z = flatValue - f.rgb;
        float3 flatTail = (f.rgb - (flatValue-z)) + (-biasValue-z);
        float3 mean(p.means[0],p.means[1],p.means[2]), meanTail(p.meanTails[0],p.meanTails[1],p.meanTails[2]);
        float3 norm = flatValue / mean;
        // Defer a borderline validity decision or an intermediate overflow to
        // the double-precision implementation before exposing this output.
        if (a > 1.e-6f && (any(abs(norm - p.epsilon) <= p.epsilon * 1.e-6f) || !all(isfinite(norm)))) {
            output[i] = float4(NAN);
            return;
        }
        if (any(norm <= p.epsilon)) a = 0;
        float3 numerator = v.rgb * mean;
        float3 tail = fma(v.rgb,mean,-numerator) + v.rgb * meanTail + error * mean;
        float3 quotient = numerator / flatValue;
        v.rgb = quotient + (fma(-quotient,flatValue,numerator) + (tail-quotient*flatTail)) / flatValue;
    } else v.rgb += error;
    if (p.clampNegative && all(isfinite(v.rgb))) v.rgb = max(v.rgb, 0.f);
    output[i] = a > 1.e-6f ? float4(v.rgb,a) : float4(0);
}
// One SIMD group per pixel/channel. Keep at most eight frame samples per
// lane in registers; frame-major reads keep the CPU tile layout unchanged.
// Q24 represents the near-unit binary32 coverage weights exactly.
uint warpSum(uint x) { return simd_sum(x); }
// Four exact 16-bit partial sums avoid overflow in SIMD collectives on
// hardware that only implements native 32-bit SIMD reductions.
ulong warpSum(ulong x) {
    return ulong(simd_sum(uint(x & 65535ul))) + (ulong(simd_sum(uint((x >> 16) & 65535ul))) << 16)
         + (ulong(simd_sum(uint((x >> 32) & 65535ul))) << 32) + (ulong(simd_sum(uint(x >> 48))) << 48);
}
uint warpMax(uint x) { return simd_max(x); }
ulong warpMax(ulong x) {
    const uint high = simd_max(uint(x >> 32));
    return (ulong(high) << 32) | simd_max(uint(x >> 32) == high ? uint(x) : 0u);
}
// Interpolate between adjacent sample centers. Integer residuals preserve
// low bits of large Q40 weights when converting them to binary32.
template<typename W>
float interpolateCenter(float lo, float hi, W numerator, W denominator) {
    float n = float(numerator), d = float(denominator);
    float nt = float(long(numerator) - long(n)), dt = float(long(denominator) - long(d));
    float t = n / d, tt = (fma(-t,d,n) + (nt-t*dt)) / d;
    float diff = hi-lo, dz = diff-hi, diffTail = (hi-(diff-dz)) + (-lo-dz);
    float term = diff*t, value = lo+term, z=value-lo;
    float tail = ((lo-(value-z)) + (term-z)) + fma(diff,t,-term) + diff*tt + diffTail*t;
    return value+tail;
}
template<typename W, uint N>
float weightedMiddle(thread float (&values)[N], thread W (&weights)[N], W total) {
    float lo = INFINITY, hi = -INFINITY;
    for (uint k = 0; k < N; ++k)
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
        W left = 0;
        float a = -INFINITY, b = INFINITY;
        W aw = 0, bw = 0;
        for (uint k = 0; k < N; ++k)
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
        left = warpSum(left);
        float lower = simd_max(a), upper = simd_min(b);
        W wa = warpMax(a == lower ? aw : W(0)), wb = warpMax(b == upper ? bw : W(0));
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
            for (uint k = 0; k < N; ++k) if (weights[k]) {
                if (values[k] == lower && weights[k] != wa) unambiguous = false;
                if (values[k] == upper && weights[k] != wb) unambiguous = false;
            }
            if (!simd_all(unambiguous)) return NAN;
            return interpolateCenter(lower,upper,W(delta+long(wa)),W(wa+wb));
        }
    }
    return NAN;
}
template<typename W, uint N>
void winsorCenters(device const float4* input, device float2* output, constant uint4& shape,
                   device const float* frameWeights, uint tid, ushort lane, float weightScale) {
    uint i = tid / 32, p = i / 3, c = i % 3;
    W total = 0;
    bool supported = true;
    float values[N];
    W weights[N];
    for (uint k = 0; k < N; ++k) {
        uint frame = lane + 32 * k;
        float4 v = frame < shape.y ? input[frame * shape.x + p] : float4(0);
        float w = all(isfinite(v)) ? clamp(v.a, 0.f, 1.f) : 0;
        if (w <= 1.e-6f)
            w = 0;
        if (shape.z && frame < shape.y) w *= frameWeights[frame];
        weights[k] = W(w * weightScale);
        supported &= float(weights[k]) / weightScale == w;
        values[k] = v[c];
        total += weights[k];
    }
    total = warpSum(total);
    float2 result = float2(NAN);
    if (simd_all(supported) && total) {
        result.x = weightedMiddle(values, weights, total);
        if (isfinite(result.x)) {
            for (uint k = 0; k < N; ++k)
                values[k] = abs(values[k] - result.x);
            result.y = weightedMiddle(values, weights, total);
        }
    }
    if (lane == 0)
        output[i] = result;
}
kernel void winsor(device const float4* input [[buffer(0)]], device float2* output [[buffer(1)]],
                   constant uint4& shape [[buffer(2)]], device const float* weights [[buffer(3)]],
                   uint tid [[thread_position_in_grid]], ushort lane [[thread_index_in_simdgroup]]) {
    winsorCenters<uint,8>(input, output, shape, weights, tid, lane, 16777216.f);
}
kernel void winsorWide(device const float4* input [[buffer(0)]], device float2* output [[buffer(1)]],
                       constant uint4& shape [[buffer(2)]], device const float* weights [[buffer(3)]],
                       uint tid [[thread_position_in_grid]], ushort lane [[thread_index_in_simdgroup]]) {
    const float scale = 1099511627776.f;
    if (shape.y <= 32) winsorCenters<ulong,1>(input,output,shape,weights,tid,lane,scale);
    else if (shape.y <= 128) winsorCenters<ulong,4>(input,output,shape,weights,tid,lane,scale);
    else if (shape.y <= 256) winsorCenters<ulong,8>(input,output,shape,weights,tid,lane,scale);
    else if (shape.y <= 512) winsorCenters<ulong,16>(input,output,shape,weights,tid,lane,scale);
    else winsorCenters<ulong,32>(input,output,shape,weights,tid,lane,scale);
}
// Large stacks scan the resident tile instead of retaining one register per
// frame. Quantiles are always taken over the entire sequence.
float streamMiddle(device const float4* tile, device const float* frameWeights, constant uint4& shape,
                   uint pixel, uint channel, ushort lane, float center, bool deviations) {
    const float scale = 1099511627776.f;
    ulong total = 0;
    float lo = INFINITY, hi = -INFINITY;
    bool supported = true;
    for (uint f=lane; f<shape.y; f+=32) {
        float4 v = tile[f*shape.x+pixel];
        if (!all(isfinite(v)) || v.a<=1.e-6f) continue;
        float w = saturate(v.a)*(shape.z ? frameWeights[f] : 1.f);
        ulong q = ulong(w*scale);
        supported &= float(q)/scale == w;
        if (!q) continue;
        float value = deviations ? abs(v[channel]-center) : v[channel];
        total += q;
        lo=min(lo,value);
        hi=max(hi,value);
    }
    lo=simd_min(lo); hi=simd_max(hi); total=warpSum(total);
    if (!simd_all(supported) || !total) return NAN;
    for (uint iteration=0;iteration<48;++iteration) {
        if (lo==hi) return lo;
        float pivot=lo*.5f+hi*.5f;
        if (!(pivot<hi)) pivot=lo;
        ulong left=0,wa=0,wb=0;
        float lower=-INFINITY,upper=INFINITY;
        for (uint f=lane; f<shape.y; f+=32) {
            float4 v=tile[f*shape.x+pixel];
            if (!all(isfinite(v)) || v.a<=1.e-6f) continue;
            ulong q=ulong(saturate(v.a)*(shape.z ? frameWeights[f] : 1.f)*scale);
            if (!q) continue;
            float value=deviations ? abs(v[channel]-center) : v[channel];
            if (value<=pivot) {
                left+=q;
                if (value>lower) { lower=value; wa=q; }
            } else if (value<upper) { upper=value; wb=q; }
        }
        float a=simd_max(lower), b=simd_min(upper);
        wa=warpMax(lower==a ? wa : 0ul); wb=warpMax(upper==b ? wb : 0ul);
        const long delta=long(total)-2L*long(warpSum(left));
        if (delta < -long(wa)) hi=a;
        else if (delta > long(wb)) lo=b;
        else {
            bool unambiguous=true;
            for (uint f=lane; f<shape.y; f+=32) {
                float4 v=tile[f*shape.x+pixel];
                if (!all(isfinite(v)) || v.a<=1.e-6f) continue;
                ulong q=ulong(saturate(v.a)*(shape.z ? frameWeights[f] : 1.f)*scale);
                if (!q) continue;
                float value=deviations ? abs(v[channel]-center) : v[channel];
                if ((value==a && q!=wa) || (value==b && q!=wb)) unambiguous=false;
            }
            if (!simd_all(unambiguous)) return NAN;
            return interpolateCenter(a,b,ulong(delta+long(wa)),wa+wb);
        }
    }
    return NAN;
}
kernel void winsorStream(device const float4* input [[buffer(0)]], device float2* output [[buffer(1)]],
                         constant uint4& shape [[buffer(2)]], device const float* weights [[buffer(3)]],
                         uint tid [[thread_position_in_grid]], ushort lane [[thread_index_in_simdgroup]]) {
    uint i=tid/32, p=i/3, c=i%3;
    float middle=streamMiddle(input,weights,shape,p,c,lane,0,false);
    float mad=isfinite(middle) ? streamMiddle(input,weights,shape,p,c,lane,middle,true) : NAN;
    if (!lane) output[i]=float2(middle,mad);
}
struct ReductionParams {
    uint pixels, frames, weighted, blockCount;
    float sigmaLow, sigmaHigh, totalWeight;
    float totalTail;
};
bool limits(device const float2* centers, uint p, constant ReductionParams& r,
            thread float3& low, thread float3& high) {
    const float2 a = centers[3*p], b = centers[3*p+1], c = centers[3*p+2];
    if (!all(isfinite(a)) || !all(isfinite(b)) || !all(isfinite(c))) return false;
    const float3 m(a.x,b.x,c.x), sigma = max(float3(1.e-6f),float3(a.y,b.y,c.y) * 1.4826f);
    low = fma(float3(-r.sigmaLow),sigma,m);
    high = fma(float3(r.sigmaHigh),sigma,m);
    return true;
}
kernel void reduceWinsor(device const float4* tile [[buffer(0)]], device float4* output [[buffer(1)]],
                         constant ReductionParams& r [[buffer(2)]], device const float2* centers [[buffer(3)]],
                         device const float* weights [[buffer(4)]], uint tid [[thread_position_in_grid]],
                         ushort lane [[thread_index_in_simdgroup]]) {
    const uint padded = (r.pixels + 31) & ~31u;
    float3 low, high;
    if (tid < padded) {
        if (tid >= r.pixels) return;
        if (!limits(centers,tid,r,low,high)) {
            output[3*tid] = float4(0,0,0,-1);
            output[3*tid+1] = output[3*tid+2] = 0;
            return;
        }
        float4 sum = 0, correction = 0;
        float tinyCoverage = 0;
        uint3 lowCount = 0, highCount = 0;
        uint count = 0;
        for (uint f = 0; f < r.frames; ++f) {
            float4 v = tile[f*r.pixels+tid];
            if (!all(isfinite(v))) continue;
            float a = saturate(v.a), w = a * (r.weighted ? weights[f] : 1.f);
            if (a <= 1.e-6f || w <= 0) { tinyCoverage += w; continue; }
            float3 rgb = clamp(v.rgb,low,high);
            float4 term = float4(rgb,1.f)*w;
            float4 next = sum + term, z = next - sum;
            correction += (sum - (next - z)) + (term - z);
            correction.rgb += fma(rgb,float3(w),-term.rgb);
            sum = next;
            lowCount += uint3(v.rgb < low); highCount += uint3(v.rgb > high);
            ++count;
        }
        const float ratio = sum.a / r.totalWeight;
        const float tail = fma(-ratio, r.totalWeight, sum.a) + (correction.a + tinyCoverage - ratio*r.totalTail);
        const float alpha = ratio + tail/r.totalWeight;
        const float3 quotient = sum.rgb / sum.a;
        const float3 remainder = fma(-quotient,float3(sum.a),sum.rgb) + (correction.rgb-quotient*correction.a);
        output[3*tid] = float4(quotient + remainder/sum.a, saturate(alpha));
        output[3*tid+1] = float4(float3(lowCount)/float(count),1);
        output[3*tid+2] = float4(float3(highCount)/float(count),1);
        return;
    }
    uint group = (tid - padded) / 32, block = group % r.blockCount, frame = group / r.blockCount;
    uint3 counts = 0;
    for (uint x = block * 256 + lane; x < min(r.pixels,(block+1)*256); x += 32) {
        float4 v = tile[frame*r.pixels+x];
        if (!all(isfinite(v)) || v.a <= 1.e-6f || !limits(centers,x,r,low,high) ||
            (r.weighted && saturate(v.a)*weights[frame] <= 0)) continue;
        uint3 lo = uint3(v.rgb < low), hi = uint3(v.rgb > high);
        counts += uint3(3,lo.x+lo.y+lo.z,hi.x+hi.y+hi.z);
    }
    counts = simd_sum(counts);
    if (!lane) output[3*r.pixels+group] = float4(float3(counts),0);
}
)metal";

enum class Kernel { Warp, CFA, Winsor, Peaks, Calibration, WinsorWide, Reduce, GuardLuminance, GuardSky, WinsorStream, ScientificCFA, Count };
constexpr std::array<const char*, unsigned(Kernel::Count)> kernelNames{
    "warp", "demosaic", "winsor", "peaks", "calibration", "winsorWide",
    "reduceWinsor", "guardLuminance", "guardSky", "winsorStream", "scientificCFA"};
struct MetalSlot {
    bool busy = false;
    id<MTLBuffer> input, output;
    id<MTLBuffer> extra[3]{};
};
struct MetalDevice {
    std::mutex mutex;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipelines[unsigned(Kernel::Count)]{};
    std::condition_variable available;
    MetalSlot slots[2];
    MetalDevice() {
        queue = [device newCommandQueue];
        MTLCompileOptions* opts = [MTLCompileOptions new];
        if (@available(macOS 15, iOS 18, *))
            opts.mathMode = MTLMathModeSafe;
        else
            opts.fastMathEnabled = NO;
        NSError* error = nil;
        auto library = [device newLibraryWithSource:[NSString stringWithUTF8String:shader] options:opts error:&error];
        for (std::size_t i = 0; i < kernelNames.size(); ++i) {
            auto function = [library newFunctionWithName:[NSString stringWithUTF8String:kernelNames[i]]];
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
             std::size_t outputSamples, std::vector<float>& result, const char* backend,
             std::array<std::span<const float>, 3> extra = {}) {
    @autoreleasepool {
        @try {
            static_assert(std::is_trivially_copyable_v<Params>);
            auto& gpu = sharedDevice();
            auto pipeline = gpu.pipelines[unsigned(kernel)];
            if (!gpu.queue || !pipeline)
                return fallback("Metal device or pipeline unavailable");
            if ((kernel == Kernel::Winsor || kernel == Kernel::WinsorWide || kernel == Kernel::Reduce || kernel == Kernel::GuardSky || kernel == Kernel::WinsorStream) && pipeline.threadExecutionWidth != 32)
                return fallback("requires 32-lane Metal SIMD groups");
            int pressure = 0;
            std::size_t length = sizeof(pressure);
            if (sysctlbyname("kern.memorystatus_vm_pressure_level", &pressure, &length, nullptr, 0) == 0 &&
                pressure > 1) {
                std::lock_guard lock(gpu.mutex);
                for (auto& slot : gpu.slots)
                    if (!slot.busy) slot = {};
                return fallback("system memory pressure");
            }
            const NSUInteger inputBytes = source.size_bytes(), outputBytes = outputSamples * sizeof(float);
            const auto limit =
                std::min<std::uint64_t>(gpu.device.maxBufferLength, gpu.device.recommendedMaxWorkingSetSize / 8);
            NSUInteger extraBytes = 0;
            for (auto span : extra) extraBytes += span.size_bytes();
            if (inputBytes > limit || outputBytes > limit || extraBytes > limit)
                return fallback("Metal working-set budget");
            const auto grow = [&](id<MTLBuffer> buffer, NSUInteger length) -> id<MTLBuffer> {
                if (buffer.length >= length)
                    return buffer;
                return [gpu.device newBufferWithLength:length options:MTLResourceStorageModeShared];
            };
            std::unique_lock lock(gpu.mutex);
            gpu.available.wait(lock, [&] { return !gpu.slots[0].busy || !gpu.slots[1].busy; });
            auto& slot = gpu.slots[gpu.slots[0].busy ? 1 : 0];
            slot.busy = true;
            lock.unlock();
            struct ReleaseSlot {
                MetalDevice& gpu;
                MetalSlot& slot;
                ~ReleaseSlot() {
                    std::lock_guard guard(gpu.mutex);
                    slot.busy = false;
                    gpu.available.notify_one();
                }
            } release{gpu, slot};
            // Completed before this call returns. Borrow only complete, aligned VM
            // pages so the driver never reads outside the vector's live elements.
            const auto page = static_cast<NSUInteger>(getpagesize());
            id<MTLBuffer> input = nil;
            if (gpu.device.hasUnifiedMemory && inputBytes % page == 0 &&
                reinterpret_cast<std::uintptr_t>(source.data()) % page == 0) {
                input = [gpu.device newBufferWithBytesNoCopy:const_cast<float*>(source.data())
                            length:inputBytes options:MTLResourceStorageModeShared deallocator:nil];
            }
            if (!input) {
                slot.input = grow(slot.input, inputBytes);
                if (!slot.input) return fallback("Metal buffer allocation failed");
                std::memcpy(slot.input.contents, source.data(), inputBytes);
                input = slot.input;
            }
            slot.output = grow(slot.output, outputBytes);
            if (!slot.output)
                return fallback("Metal buffer allocation failed");
            for (unsigned n = 0; n < extra.size(); ++n) {
                if (extra[n].empty()) continue;
                slot.extra[n] = grow(slot.extra[n], extra[n].size_bytes());
                if (!slot.extra[n]) return fallback("Metal auxiliary buffer allocation failed");
                std::memcpy(slot.extra[n].contents, extra[n].data(), extra[n].size_bytes());
            }
            auto command = [gpu.queue commandBuffer];
            auto encoder = [command computeCommandEncoder];
            if (!encoder)
                return fallback("Metal command encoder unavailable");
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:input offset:0 atIndex:0];
            [encoder setBuffer:slot.output offset:0 atIndex:1];
            for (unsigned n = 0; n < extra.size(); ++n)
                [encoder setBuffer:extra[n].empty() ? input : slot.extra[n] offset:0 atIndex:3+n];
            [encoder setBytes:&params length:sizeof(params) atIndex:2];
            [encoder dispatchThreads:MTLSizeMake(threads, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted)
                return fallback("Metal command failed");
            const float* pixels = static_cast<const float*>(slot.output.contents);
            if (kernel != Kernel::ScientificCFA && kernel != Kernel::GuardLuminance && kernel != Kernel::GuardSky && kernel != Kernel::Peaks && kernel != Kernel::Winsor && kernel != Kernel::WinsorWide && kernel != Kernel::WinsorStream &&
                !std::all_of(pixels, pixels + outputSamples, [](float v) { return std::isfinite(v); }))
                return fallback("Metal result exceeded finite range");
            result.assign(pixels, pixels + outputSamples);
            recordBackend(backend, "", command.GPUEndTime - command.GPUStartTime, kernelNames[unsigned(kernel)]);
            return true;
        } @catch (NSException*) {
            return fallback("Metal command failed");
        }
    }
}
} // namespace
struct WarpParameters {
    unsigned w, h, cubic, mapping;
    float dx, dy, scale, cosine, sine, invA, invB, invC, invD;
};

bool runWarp(const ImageBuffer& image, const WarpParameters& params, std::span<const float> coordinates, ImageBuffer& result) {
    if (!execute(Kernel::Warp, image.pixels, params, image.pixelCount(), image.sampleCount(), result.pixels, "metal",
                 {coordinates, {}, {}})) return false;
    result.width = image.width;
    result.height = image.height;
    result.channels = image.channels;
    result.format = image.format;
    result.colorEncoding = image.colorEncoding;
    result.sourceBitsPerChannel = image.sourceBitsPerChannel;
    return true;
}

bool metalAffine(const ImageBuffer& image, AffineTransform t, RegistrationInterpolation interpolation, ImageBuffer& result) {
    const float det = t.a * t.d - t.b * t.c;
    if (std::abs(det) < 1.e-8F) return false;
    return runWarp(image, {image.width, image.height, interpolation == RegistrationInterpolation::Bicubic, 1,
                           t.dx, t.dy, 1, 1, 0, t.d/det, -t.b/det, -t.c/det, t.a/det}, {}, result);
}
bool metalMappedWarp(const ImageBuffer& image, std::span<const float> coords, RegistrationInterpolation interpolation,
                     ImageBuffer& result) {
    return runWarp(image, {image.width, image.height, interpolation == RegistrationInterpolation::Bicubic, 2,
                           0, 0, 1, 1, 0, 0, 0, 0, 0}, coords, result);
}

bool metalWarp(const ImageBuffer& image, SimilarityTransform transform, RegistrationInterpolation interpolation,
               ImageBuffer& result) {
    return runWarp(image, {image.width, image.height, interpolation == RegistrationInterpolation::Bicubic, 0,
                           transform.dx, transform.dy, std::max(1.e-6F, transform.scale),
                           std::cos(transform.rotationRadians), std::sin(transform.rotationRadians), 0, 0, 0, 0},
                   {}, result);
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
bool metalScientificCFA(const std::vector<float>& mosaic, unsigned width, unsigned height,
                        const std::array<int,4>& colors, const std::array<float,3>& gains,
                        std::vector<float>& samples) {
    struct { unsigned w,h; std::array<int,4> colors; std::array<float,3> gains; } params{width,height,colors,gains};
    return execute(Kernel::ScientificCFA,mosaic,params,mosaic.size(),mosaic.size()*4,samples,"metal+cpu-cfa");
}
bool metalGuardLuminance(const ImageBuffer& image, const std::array<float,3>& gains, std::vector<float>& luminance) {
    struct { float hi[3], lo[3]; } params{};
    const double coefficients[] = {.2126*gains[0], .7152*gains[1], .0722*gains[2]};
    for (unsigned c=0;c<3;++c) {
        params.hi[c] = float(coefficients[c]);
        params.lo[c] = float(coefficients[c] - params.hi[c]);
    }
    return execute(Kernel::GuardLuminance, image.pixels, params, image.pixelCount(), image.pixelCount(),
                   luminance, "metal+cpu-guard");
}
bool metalGuardSky(std::span<const float> mosaic, unsigned width, const std::array<int,4>& colors,
                   std::span<const float> apertures, std::vector<float>& sky) {
    const auto channels = apertures.size()/4*3;
    struct { unsigned width; std::array<int,4> colors; } params{width,colors};
    return execute(Kernel::GuardSky, mosaic, params, channels*32, channels*2, sky, "metal+cpu-guard",
                   {apertures,{},{}});
}
bool metalPeakMask(std::span<const float> luminance, float threshold, std::vector<float>& mask) {
    const auto words = luminance.size() / 32;
    return execute(Kernel::Peaks, luminance, threshold, words, words, mask, "metal+cpu-photometry");
}
bool metalCalibrate(const ImageBuffer& light, const CalibrationOptions& options, bool lightBias, bool flatBias,
                    const std::vector<double>& means, ImageBuffer& result) {
    struct {
        unsigned dark, flat, lightBias, flatBias, clampNegative;
        float epsilon, means[3], meanTails[3];
    } params{options.dark != nullptr, options.flat != nullptr, lightBias, flatBias, options.clampNegativeValues,
             options.flatEpsilon, {float(means[0]), float(means[1]), float(means[2])}, {}};
    for (unsigned c = 0; c < 3; ++c) params.meanTails[c] = float(means[c] - params.means[c]);
    for (const auto m : params.means) if (!(m > 0 && std::isfinite(m))) return fallback("flat mean outside Metal range");
    const auto plane = [](const ImageBuffer* image) -> std::span<const float> {
        return image ? std::span<const float>(image->pixels) : std::span<const float>{};
    };
    return execute(Kernel::Calibration, light.pixels, params, light.pixelCount(), light.sampleCount(), result.pixels,
                   "metal", {plane(options.dark), plane(options.bias), plane(options.flat)});
}
bool metalWinsorCenters(const std::vector<float>& tile, std::size_t pixels, std::size_t frames,
                        std::span<const float> weights, std::vector<float>& centers) {
    std::array<unsigned, 4> params{unsigned(pixels), unsigned(frames), !weights.empty(), 0};
    // Two signed midpoint distances must fit in int64 at Q40 precision.
    if (frames >= (1u << 21) || pixels > UINT32_MAX / frames) return fallback("Metal tile indexing limit");
    const auto kernel = frames > 1024 ? Kernel::WinsorStream :
                        weights.empty() && frames <= 240 ? Kernel::Winsor : Kernel::WinsorWide;
    return execute(kernel, tile, params, pixels * 3 * 32, pixels * 6, centers, "metal",
                   {weights, {}, {}});
}
bool metalWinsorReduce(const std::vector<float>& tile, std::size_t pixels, std::size_t frames,
                       std::span<const float> weights, const std::vector<float>& centers, float low, float high,
                       bool counts, std::vector<float>& reduced) {
    const unsigned blocks = (unsigned(pixels) + 255) / 256;
    double total = weights.empty() ? double(frames) : std::accumulate(weights.begin(), weights.end(), 0.);
    struct {
        unsigned pixels, frames, weighted, blocks;
        float low, high, total;
        float totalTail;
    } params{unsigned(pixels),unsigned(frames),!weights.empty(),blocks,low,high,float(total),float(total - float(total))};
    const auto groups = counts ? frames * blocks : 0;
    return execute(Kernel::Reduce, tile, params, ((pixels + 31) / 32 + groups) * 32, (3 * pixels + groups) * 4,
                   reduced, "metal", {centers, weights, {}});
}
} // namespace photonstack::detail
