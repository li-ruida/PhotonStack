#include "ChromaticBeaconDetector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

namespace photonstack::detail {
namespace {
struct Point {
    double x, y, strength;
};
struct Field {
    int w, h;
    double scale;
    std::vector<std::array<float, 3>> rgb;
    float chroma(int x, int y, int c) const {
        const auto& v = rgb[std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)];
        return v[c] - .5F * (v[(c + 1) % 3] + v[(c + 2) % 3]);
    }
};
double median(std::vector<double> v) {
    if (v.empty())
        return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    const double upper = v[v.size() / 2];
    return v.size() % 2 ? upper : .5 * (upper + *std::max_element(v.begin(), v.begin() + v.size() / 2));
}
bool smoothBackground(const Field& f, int x, int y, double tolerance, bool coreCandidate = false,
                      bool robustGradient = false) {
    std::vector<double> ring;
    double mean = 0, gx = 0, gy = 0;
    for (int k = 0; k < 16; ++k) {
        const double angle = k * 6.283185307 / 16;
        const int qx = std::clamp(x + int(12 * std::cos(angle)), 0, f.w - 1);
        const int qy = std::clamp(y + int(12 * std::sin(angle)), 0, f.h - 1);
        const auto& v = f.rgb[qy * f.w + qx];
        const double value = (v[0] + v[1] + v[2]) / 3;
        ring.push_back(value);
        mean += value / 16;
        gx += value * std::cos(angle) / 8;
        gy += value * std::sin(angle) / 8;
    }
    const double baseline = median(ring);
    if (robustGradient) {
        // A single bright stellar sample can poison a least-squares plane
        // and make the entire ring look textured. Opposite-pair medians
        // estimate the gradient without spreading that outlier around it.
        std::vector<double> xs, ys;
        for (int k = 0; k < 8; ++k) {
            const double angle = k * 6.283185307 / 16;
            const double dx = std::cos(angle), dy = std::sin(angle);
            if (std::abs(dx) > .7)
                xs.push_back((ring[k] - ring[k + 8]) / (2 * dx));
            if (std::abs(dy) > .7)
                ys.push_back((ring[k] - ring[k + 8]) / (2 * dy));
        }
        gx = median(xs);
        gy = median(ys);
        std::vector<double> intercepts;
        for (int k = 0; k < 16; ++k) {
            const double angle = k * 6.283185307 / 16;
            intercepts.push_back(ring[k] - gx * std::cos(angle) - gy * std::sin(angle));
        }
        mean = median(intercepts);
    }
    auto deviations = ring;
    for (auto& value : deviations)
        value = std::abs(value - baseline);
    if (coreCandidate && median(deviations) >= tolerance)
        return false;
    // A city-lit sky can have a smooth, steep gradient. Remove the local
    // affine component before measuring texture, rather than excluding
    // genuine faint lights merely because their background is not flat.
    for (int k = 0; k < 16; ++k) {
        const double angle = k * 6.283185307 / 16;
        ring[k] -= mean + gx * std::cos(angle) + gy * std::sin(angle);
    }
    // MAD can be exactly zero on two-level facades when one level has a
    // narrow majority. The interquartile spread still sees the structure,
    // while ignoring the few ring samples touched by a crossing star.
    std::sort(ring.begin(), ring.end());
    return ring[12] - ring[3] < (coreCandidate ? 6 : 2) * tolerance;
}
double backgroundLevel(const Field& f, double x, double y) {
    std::vector<double> ring;
    for (int k = 0; k < 16; ++k) {
        const double angle = k * 6.283185307 / 16;
        const int qx = std::clamp(int(std::lround(x + 12 * std::cos(angle))), 0, f.w - 1);
        const int qy = std::clamp(int(std::lround(y + 12 * std::sin(angle))), 0, f.h - 1);
        const auto& v = f.rgb[qy * f.w + qx];
        ring.push_back((v[0] + v[1] + v[2]) / 3);
    }
    // Detect a silhouette before the centre of the repair corridor enters
    // it. A median waits until half the ring is already inside the building.
    std::sort(ring.begin(), ring.end());
    return ring[3];
}
Field reduce(const ImageBuffer& im) {
    const int step = std::max(1, int(std::ceil(std::max(im.width, im.height) / 2048.0)));
    Field f{int((im.width + step - 1) / step), int((im.height + step - 1) / step), double(step), {}};
    f.rgb.resize(std::size_t(f.w) * f.h);
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
            std::array<double, 3> sum{};
            int n = 0;
            for (int dy = 0; dy < step; ++dy)
                for (int dx = 0; dx < step; ++dx) {
                    const auto ix = x * step + dx, iy = y * step + dy;
                    if (ix >= int(im.width) || iy >= int(im.height))
                        continue;
                    const auto p = (std::size_t(iy) * im.width + ix) * im.channels;
                    if (!std::isfinite(im.pixels[p]) || !std::isfinite(im.pixels[p + 1]) ||
                        !std::isfinite(im.pixels[p + 2]) || (im.channels == 4 && im.pixels[p + 3] <= 0))
                        continue;
                    for (int c = 0; c < 3; ++c)
                        sum[c] += im.pixels[p + c];
                    ++n;
                }
            if (n)
                for (int c = 0; c < 3; ++c)
                    f.rgb[y * f.w + x][c] = float(sum[c] / n);
        }
    return f;
}
std::vector<Point> points(const Field& f, int channel) {
    const int r = 5;
    std::vector<float> contrast(f.rgb.size());
    std::vector<double> noise;
    for (int y = r; y < f.h - r; ++y)
        for (int x = r; x < f.w - r; ++x) {
            std::array<float, 4> side{f.chroma(x - r, y, channel), f.chroma(x + r, y, channel),
                                      f.chroma(x, y - r, channel), f.chroma(x, y + r, channel)};
            std::sort(side.begin(), side.end());
            contrast[y * f.w + x] = f.chroma(x, y, channel) - .5F * (side[1] + side[2]);
            if (x % 13 == 0 && y % 13 == 0)
                noise.push_back(std::abs(contrast[y * f.w + x]));
            // A slightly warm white star is not a navigation lamp. Compare local
            // chromatic excess to the local brightness excess, not absolute RGB.
            const auto brightness = [&](int px, int py) {
                const auto& v = f.rgb[py * f.w + px];
                return std::max({v[0], v[1], v[2]});
            };
            std::array<float, 4> light{brightness(x - r, y), brightness(x + r, y), brightness(x, y - r),
                                       brightness(x, y + r)};
            std::sort(light.begin(), light.end());
            const auto excess = brightness(x, y) - .5F * (light[1] + light[2]);
            if (contrast[y * f.w + x] < excess * .25F)
                contrast[y * f.w + x] = 0;
        }
    const double threshold = std::max(.0025, median(noise) * 7);
    std::vector<std::uint8_t> seen(f.rgb.size());
    std::vector<Point> result;
    for (int y = r; y < f.h - r; ++y)
        for (int x = r; x < f.w - r; ++x) {
            const auto root = y * f.w + x;
            if (seen[root] || contrast[root] < threshold)
                continue;
            std::vector<int> queue{root};
            seen[root] = 1;
            double mass = 0, sx = 0, sy = 0;
            int minx = x, maxx = x, miny = y, maxy = y;
            for (std::size_t i = 0; i < queue.size(); ++i) {
                const int px = queue[i] % f.w, py = queue[i] / f.w;
                const double weight = contrast[queue[i]];
                mass += weight;
                sx += px * weight;
                sy += py * weight;
                minx = std::min(minx, px);
                maxx = std::max(maxx, px);
                miny = std::min(miny, py);
                maxy = std::max(maxy, py);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx, ny = py + dy;
                        if (nx < r || ny < r || nx >= f.w - r || ny >= f.h - r)
                            continue;
                        const auto q = ny * f.w + nx;
                        if (!seen[q] && contrast[q] >= threshold) {
                            seen[q] = 1;
                            queue.push_back(q);
                        }
                    }
            }
            // A beacon is compact; long red stellar streaks are not evidence.
            if (queue.size() <= 70 && maxx - minx <= 15 && maxy - miny <= 15 && mass >= threshold * 1.3) {
                // Colored windows are compact too, but sit in spatially structured
                // foreground. Require a smooth local background, robust to a star
                // crossing one of the ring samples. No fixed horizon is assumed.
                const int cx = int(sx / mass), cy = int(sy / mass);
                if (smoothBackground(f, cx, cy, std::max(.004, threshold * 2), true))
                    result.push_back({sx / mass, sy / mass, mass});
            }
        }
    // Bound the voting cost in city scenes, deterministically.
    if (result.size() > 2500) {
        std::stable_sort(result.begin(), result.end(), [](auto a, auto b) { return a.strength > b.strength; });
        result.resize(2500);
    }
    return result;
}
struct Curve {
    double ox = 0, oy = 0, ux = 1, uy = 0;
    std::array<double, 3> c{};
    double t(const Point& p) const { return (p.x - ox) * ux + (p.y - oy) * uy; }
    double n(const Point& p) const { return -(p.x - ox) * uy + (p.y - oy) * ux; }
    double value(double t) const {
        t /= 2048;
        return c[0] + c[1] * t + c[2] * t * t;
    }
    double residual(const Point& p) const { return n(p) - value(t(p)); }
    Point at(double t) const {
        const double n = value(t);
        return {ox + ux * t - uy * n, oy + uy * t + ux * n, 0};
    }
};
bool fit(Curve& curve, const std::vector<Point>& pts, const std::vector<int>& ids) {
    double a[3][4]{};
    for (auto i : ids) {
        const double t = curve.t(pts[i]) / 2048, n = curve.n(pts[i]);
        const double p[] = {1, t, t * t, t * t * t, t * t * t * t};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c)
                a[r][c] += p[r + c];
            a[r][3] += p[r] * n;
        }
    }
    for (int k = 0; k < 3; ++k) {
        int pivot = k;
        for (int j = k + 1; j < 3; ++j)
            if (std::abs(a[j][k]) > std::abs(a[pivot][k]))
                pivot = j;
        if (std::abs(a[pivot][k]) < 1e-10)
            return false;
        for (int c = k; c < 4; ++c)
            std::swap(a[k][c], a[pivot][c]);
        const double d = a[k][k];
        for (int c = k; c < 4; ++c)
            a[k][c] /= d;
        for (int j = 0; j < 3; ++j)
            if (j != k) {
                const double m = a[j][k];
                for (int c = k; c < 4; ++c)
                    a[j][c] -= m * a[k][c];
            }
    }
    for (int k = 0; k < 3; ++k)
        curve.c[k] = a[k][3];
    return true;
}
} // namespace

std::vector<ArtifactTrail> detectChromaticBeaconChains(const ImageBuffer& image) {
    if (image.empty() || image.channels < 3 || image.pixels.size() != image.sampleCount())
        return {};
    const auto f = reduce(image);
    const auto red = points(f, 0), green = points(f, 1);
    const bool profile = std::getenv("PHOTONSTACK_PROFILE_ARTIFACTS") != nullptr;
    if (profile)
        std::clog << "beacons red=" << red.size() << " green=" << green.size() << '\n';
    if (red.size() < 8 || green.size() < 3)
        return {};
    std::vector<bool> used(red.size());
    std::vector<ArtifactTrail> result;
    std::mt19937 rng(0x50484f54);
    for (int chain = 0; chain < 16; ++chain) {
        Curve best;
        std::vector<int> bestIds;
        // A bounded, repeatable consensus search is independent of image axes.
        for (int iteration = 0; iteration < 6000; ++iteration) {
            const auto i = rng() % red.size(), j = rng() % red.size();
            if (i == j || used[i] || used[j])
                continue;
            const double dx = red[j].x - red[i].x, dy = red[j].y - red[i].y, len = std::hypot(dx, dy);
            if (len < 120)
                continue;
            Curve trial;
            trial.ox = red[i].x;
            trial.oy = red[i].y;
            trial.ux = dx / len;
            trial.uy = dy / len;
            std::vector<int> ids;
            for (std::size_t k = 0; k < red.size(); ++k)
                if (!used[k] && std::abs(trial.n(red[k])) < 6)
                    ids.push_back(int(k));
            if (ids.size() > bestIds.size()) {
                best = trial;
                bestIds = std::move(ids);
            }
        }
        if (bestIds.size() < 8)
            break;
        for (int iteration = 0; iteration < 4; ++iteration) {
            if (!fit(best, red, bestIds))
                break;
            std::vector<int> ids;
            for (std::size_t k = 0; k < red.size(); ++k)
                if (!used[k] && std::abs(best.residual(red[k])) < 2.5)
                    ids.push_back(int(k));
            bestIds = std::move(ids);
            if (bestIds.size() < 8)
                break;
        }
        if (bestIds.empty())
            break;
        for (auto i : bestIds)
            used[i] = true;
        if (bestIds.size() < 8)
            continue;
        std::vector<double> positions;
        for (auto i : bestIds)
            positions.push_back(best.t(red[i]));
        std::sort(positions.begin(), positions.end());
        // Merge components belonging to the same flash before counting gaps.
        std::vector<double> distinct;
        for (auto t : positions)
            if (distinct.empty() || t - distinct.back() > 4)
                distinct.push_back(t);
        if (distinct.size() < 8)
            continue;
        const double lo = distinct.front(), hi = distinct.back(), span = hi - lo;
        if (span < 160 || std::abs(2 * best.c[2] * span / (2048 * 2048)) > .12)
            continue;
        // A line through a dense city/random star field can collect many
        // points by chance. Compare it with equally wide neighboring bands.
        int onBand = 0, sideBands = 0;
        for (const auto& p : red) {
            const auto t = best.t(p);
            if (t < lo || t > hi)
                continue;
            const double d = std::abs(best.residual(p));
            if (d < 2.5)
                ++onBand;
            if (std::abs(d - 12) < 2.5 || std::abs(d - 24) < 2.5)
                ++sideBands;
        }
        if (onBand < std::max(8., 3.5 * sideBands / 4.))
            continue;
        std::vector<double> gaps;
        for (std::size_t k = 1; k < distinct.size(); ++k)
            gaps.push_back(distinct[k] - distinct[k - 1]);
        const double spacing = median(gaps);
        int regular = 0;
        for (auto gap : gaps) {
            const double multiple = std::round(gap / spacing);
            if (multiple >= 1 && multiple <= 8 && std::abs(gap / spacing - multiple) < .24)
                ++regular;
        }
        if (profile)
            std::clog << "beacon candidate points=" << distinct.size() << " span=" << span << " spacing=" << spacing
                      << " regular=" << regular << '/' << gaps.size() << " endpoints=" << best.at(lo).x << ','
                      << best.at(lo).y << ":" << best.at(hi).x << ',' << best.at(hi).y << '\n';
        std::vector<double> deviations;
        for (auto i : bestIds)
            deviations.push_back(std::abs(best.residual(red[i])));
        // Two sets of navigation lamps and changing apparent aircraft speed
        // need not share a single period. Long, tightly fitted multicolor
        // chains may establish identity without strict global periodicity.
        // Do not impose a second, weaker global-period test here: two exposure
        // cadences can each be regular while neither matches their combined
        // median gap. Dense chains instead require tight geometry plus the
        // stronger independent green-light test below (8 rather than 3).
        const bool denseCoherent = distinct.size() >= 24 && median(deviations) < 1.5;
        std::vector<double> ratios;
        {
            for (auto index : bestIds) {
                const int x = int(red[index].x), y = int(red[index].y);
                std::array<double, 3> delta{};
                for (int c = 0; c < 3; ++c) {
                    const auto read = [&](int dx, int dy) {
                        return f.rgb[std::clamp(y + dy, 0, f.h - 1) * f.w + std::clamp(x + dx, 0, f.w - 1)][c];
                    };
                    delta[c] = read(0, 0) - median({read(-5, 0), read(5, 0), read(0, -5), read(0, 5)});
                }
                const double chroma = delta[0] - .5 * (delta[1] + delta[2]);
                if (chroma > .0005)
                    ratios.push_back((delta[0] + delta[1] + delta[2]) / (3 * chroma));
            }
        }
        if (profile)
            std::clog << "beacon luminance/chroma median=" << median(ratios) << " samples=" << ratios.size() << '\n';
        // Added red photons have mean-RGB/chroma >= 1/3. Allow background
        // estimation noise, but reject chains dominated by redistribution
        // between channels (JPEG/demosaic chroma noise) rather than light.
        // This is especially important when cadence changes prevent a
        // global-period test from providing independent evidence.
        if (ratios.size() < 8 || median(ratios) < .25)
            continue;
        if (spacing < 7 || (!denseCoherent && regular < int(gaps.size() * .65)))
            continue;
        int greenCount = 0;
        double greenLo = hi, greenHi = lo;
        for (const auto& p : green) {
            const double t = best.t(p);
            if (t >= lo - spacing && t <= hi + spacing && std::abs(best.residual(p)) < 8) {
                ++greenCount;
                greenLo = std::min(greenLo, t);
                greenHi = std::max(greenHi, t);
            }
        }
        if (profile)
            std::clog << "beacon green=" << greenCount << " span=" << greenHi - greenLo << '\n';
        if (greenCount < (denseCoherent ? 8 : 3) || greenHi - greenLo < span * .35)
            continue;
        int illuminated = 0, sampled = 0;
        for (double t = lo; t <= hi; t += 2) {
            const auto p = best.at(t);
            const auto read = [&](double n) {
                const int x = std::clamp(int(std::lround(p.x - best.uy * n)), 0, f.w - 1);
                const int y = std::clamp(int(std::lround(p.y + best.ux * n)), 0, f.h - 1);
                return f.rgb[y * f.w + x];
            };
            const auto left = read(-8), right = read(8);
            double excess = 0;
            for (int n = -2; n <= 2; ++n) {
                const auto v = read(n);
                for (int c = 0; c < 3; ++c)
                    excess = std::max(excess, double(v[c] - .5F * (left[c] + right[c])));
            }
            if (excess > .006)
                ++illuminated;
            ++sampled;
        }
        if (profile)
            std::clog << "beacon illuminated fraction=" << double(illuminated) / sampled << '\n';
        // Chromatic demosaic/JPEG fringes can split a continuous stellar ridge
        // into compact red and green components. It is still a continuous
        // ridge, not sparse navigation flashes.
        if (illuminated > sampled * .50)
            continue;
        double start = lo - spacing * .5, stop = hi + spacing * .5;
        ArtifactTrail trail;
        trail.kind = ArtifactTrailKind::Airplane;
        trail.confidence = .95F;
        trail.weight = .95F;
        trail.width = float(4 * f.scale);
        trail.meanBrightness = .08F;
        // Wide-angle distortion and changes of aircraft direction can bend a
        // faint endpoint away from the global quadratic. Follow local red
        // excess from the verified core outward instead of widening the mask.
        std::vector<std::pair<double, double>> tailOffsets{{lo, 0}, {hi, 0}};
        for (int side : {-1, 1}) {
            double offset = 0;
            const double anchor = side < 0 ? lo : hi;
            double last = anchor;
            const auto anchorPoint = best.at(anchor);
            double previousBackground = backgroundLevel(f, anchorPoint.x, anchorPoint.y);
            for (double step = 16; step <= std::hypot(f.w, f.h); step += 16) {
                const double at = anchor + side * step;
                if (std::abs(at - last) > std::max(80., spacing * 3))
                    break;
                const auto center = best.at(at);
                const double cx = center.x - best.uy * offset, cy = center.y + best.ux * offset;
                if (cx < 0 || cy < 0 || cx >= f.w || cy >= f.h)
                    break;
                const double currentBackground = backgroundLevel(f, cx, cy);
                // A smooth dark roof can pass a texture-only gate. Do not
                // extrapolate a sky flight through an abrupt silhouette edge.
                if (previousBackground - currentBackground > std::max(.012, previousBackground * .2))
                    break;
                previousBackground = currentBackground;
                std::vector<double> offsets;
                const double window = std::clamp(spacing, 12., 32.);
                for (double u = at - window; u <= at + window; u += 1) {
                    const auto p = best.at(u);
                    for (int n = -8; n <= 8; ++n) {
                        const double normal = offset + n;
                        const int x = int(std::lround(p.x - best.uy * normal));
                        const int y = int(std::lround(p.y + best.ux * normal));
                        if (x < 5 || y < 5 || x + 5 >= f.w || y + 5 >= f.h)
                            continue;
                        const double background = median({f.chroma(x - 4, y, 0), f.chroma(x + 4, y, 0),
                                                          f.chroma(x, y - 4, 0), f.chroma(x, y + 4, 0)});
                        if (f.chroma(x, y, 0) - background > .001 && smoothBackground(f, x, y, .004))
                            offsets.push_back(normal);
                    }
                }
                if (offsets.size() >= 2) {
                    offset += std::clamp(median(offsets) - offset, -4., 4.);
                    last = at;
                }
                if (profile)
                    std::clog << "beacon tail side=" << side << " step=" << step << " samples=" << offsets.size()
                              << " offset=" << offset << '\n';
                tailOffsets.emplace_back(at, offset);
            }
            if (side < 0)
                start = last - spacing * .5;
            else
                stop = last + spacing * .5;
        }
        std::sort(tailOffsets.begin(), tailOffsets.end());
        for (double t = start; t <= stop + 16; t += 16) {
            const double at = std::min(t, stop);
            auto p = best.at(at);
            const auto right = std::upper_bound(tailOffsets.begin(), tailOffsets.end(), std::pair{at, 1e20});
            double offset = 0;
            if (right == tailOffsets.begin())
                offset = right->second;
            else if (right == tailOffsets.end())
                offset = tailOffsets.back().second;
            else {
                const auto left = right - 1;
                const double fraction = (at - left->first) / std::max(1e-12, right->first - left->first);
                offset = left->second + fraction * (right->second - left->second);
            }
            p.x -= best.uy * offset;
            p.y += best.ux * offset;
            trail.path.push_back({float((p.x + .5) * f.scale - .5), float((p.y + .5) * f.scale - .5)});
        }
        trail.x1 = trail.path.front().x;
        trail.y1 = trail.path.front().y;
        trail.x2 = trail.path.back().x;
        trail.y2 = trail.path.back().y;
        trail.length = float((stop - start) * f.scale);
        trail.angleRadians = std::atan2(trail.y2 - trail.y1, trail.x2 - trail.x1);
        result.push_back(std::move(trail));
    }
    return result;
}

bool sameBeaconChain(const ArtifactTrail& a, const ArtifactTrail& b) {
    if (a.kind != ArtifactTrailKind::Airplane || b.kind != ArtifactTrailKind::Airplane || a.path.size() < 8 ||
        b.path.size() < 8)
        return false;
    const auto distance = [](const auto& p, const auto& path) {
        double best = 1e20;
        for (std::size_t i = 1; i < path.size(); ++i) {
            const double dx = path[i].x - path[i - 1].x, dy = path[i].y - path[i - 1].y;
            const double t = std::clamp(
                ((p.x - path[i - 1].x) * dx + (p.y - path[i - 1].y) * dy) / std::max(1e-12, dx * dx + dy * dy), 0., 1.);
            best = std::min(best, std::hypot(p.x - path[i - 1].x - t * dx, p.y - path[i - 1].y - t * dy));
        }
        return best;
    };
    // Endpoints alone cannot authenticate a caller-supplied repair corridor:
    // a different curve could share both ends but cross unrelated stars.
    return std::all_of(a.path.begin(), a.path.end(), [&](const auto& p) { return distance(p, b.path) < b.width; }) &&
           std::all_of(b.path.begin(), b.path.end(), [&](const auto& p) { return distance(p, a.path) < b.width; });
}

void repairChromaticBeaconChains(const ImageBuffer& source, ImageBuffer& output,
                                 const std::vector<ArtifactTrail>& trails,
                                 const std::function<void(double)>& progress) {
    if (trails.empty() || source.channels < 3 || source.pixels.size() != source.sampleCount() ||
        output.pixels.size() != source.pixels.size())
        return;
    struct Mask {
        float distance = 1e9F, radius = 0, ux = 1, uy = 0;
    };
    // Most of a large frame is not near an aircraft. Allocate metadata only
    // for touched 32x32 tiles, while keeping O(1) membership checks for repair
    // samples. A full 6048x4032 metadata image alone would consume ~390 MB.
    constexpr std::size_t tileSide = 32;
    using Tile = std::array<Mask, tileSide * tileSide>;
    const std::size_t tileColumns = (source.width + tileSide - 1) / tileSide;
    const std::size_t tileRows = (source.height + tileSide - 1) / tileSide;
    std::vector<std::unique_ptr<Tile>> tiles(tileColumns * tileRows);
    std::vector<std::size_t> activePixels;
    const auto readMask = [&](std::size_t pixel) -> const Mask& {
        static const Mask empty;
        const auto x = pixel % source.width, y = pixel / source.width;
        const auto& tile = tiles[(y / tileSide) * tileColumns + x / tileSide];
        return tile ? (*tile)[(y % tileSide) * tileSide + x % tileSide] : empty;
    };
    const auto writeMask = [&](std::size_t pixel) -> Mask& {
        const auto x = pixel % source.width, y = pixel / source.width;
        auto& tile = tiles[(y / tileSide) * tileColumns + x / tileSide];
        if (!tile)
            tile = std::make_unique<Tile>();
        return (*tile)[(y % tileSide) * tileSide + x % tileSide];
    };
    if (progress)
        progress(0);
    for (const auto& trail : trails)
        for (std::size_t i = 1; i < trail.path.size(); ++i) {
            const auto a = trail.path[i - 1], b = trail.path[i];
            const float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
            if (len < .01F)
                continue;
            const float radius = std::max(5.F, trail.width * 1.4F);
            const int minx = std::max(0, int(std::floor(std::min(a.x, b.x) - radius))),
                      maxx = std::min(int(source.width) - 1, int(std::ceil(std::max(a.x, b.x) + radius)));
            const int miny = std::max(0, int(std::floor(std::min(a.y, b.y) - radius))),
                      maxy = std::min(int(source.height) - 1, int(std::ceil(std::max(a.y, b.y) + radius)));
            for (int y = miny; y <= maxy; ++y)
                for (int x = minx; x <= maxx; ++x) {
                    const float t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / (len * len), 0.F, 1.F);
                    const float d = std::hypot(x - a.x - t * dx, y - a.y - t * dy);
                    if (d > radius)
                        continue;
                    const auto pixel = std::size_t(y) * source.width + x;
                    auto& m = writeMask(pixel);
                    if (m.radius == 0)
                        activePixels.push_back(pixel);
                    if (d < m.distance)
                        m = {d, radius, dx / len, dy / len};
                }
        }
    // Opposite-wing navigation lights can lie outside the red-centred
    // corridor, especially near the camera. Add only compact green lamps
    // supported by a local companion chain, never a wider full-frame band.
    const auto field = reduce(source);
    std::vector<std::int8_t> smoothSky(field.rgb.size(), -1);
    const auto safeBackground = [&](double x, double y) {
        const int fx = std::clamp(int(x / field.scale), 0, field.w - 1);
        const int fy = std::clamp(int(y / field.scale), 0, field.h - 1);
        auto& cached = smoothSky[fy * field.w + fx];
        if (cached < 0)
            cached = smoothBackground(field, fx, fy, .012, false, true) ? 1 : 0;
        return cached != 0;
    };
    const auto companions = points(field, 1);
    for (const auto& trail : trails) {
        struct Companion {
            double x, y, along, normal, ux, uy;
        };
        std::vector<Companion> candidates;
        for (const auto& point : companions) {
            const double x = (point.x + .5) * field.scale - .5;
            const double y = (point.y + .5) * field.scale - .5;
            if (!smoothBackground(field, int(point.x), int(point.y), .004))
                continue;
            double nearest = 1e20, traversed = 0;
            Companion candidate{};
            for (std::size_t i = 1; i < trail.path.size(); ++i) {
                const auto a = trail.path[i - 1], b = trail.path[i];
                const double dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
                if (length < .01)
                    continue;
                const double t = ((x - a.x) * dx + (y - a.y) * dy) / (length * length);
                const double distance =
                    std::hypot(x - a.x - std::clamp(t, 0., 1.) * dx, y - a.y - std::clamp(t, 0., 1.) * dy);
                if (t >= 0 && t <= 1 && distance < nearest) {
                    nearest = distance;
                    candidate = {
                        x,           y,          traversed + t * length, ((y - a.y) * dx - (x - a.x) * dy) / length,
                        dx / length, dy / length};
                }
                traversed += length;
            }
            if (nearest <= trail.width * 3)
                candidates.push_back(candidate);
        }
        for (const auto& point : candidates) {
            if (std::abs(point.normal) < trail.width * .8)
                continue;
            int support = 0;
            for (const auto& other : candidates)
                if (std::abs(other.along - point.along) > field.scale * 4 &&
                    std::abs(other.along - point.along) < field.scale * 80 &&
                    std::abs(other.normal - point.normal) < field.scale * 4)
                    ++support;
            if (support < 2)
                continue;
            const float radius = float(field.scale * 3);
            for (int y = std::max(0, int(point.y - radius));
                 y <= std::min(int(source.height) - 1, int(point.y + radius)); ++y)
                for (int x = std::max(0, int(point.x - radius));
                     x <= std::min(int(source.width) - 1, int(point.x + radius)); ++x) {
                    const float distance = float(std::hypot(x - point.x, y - point.y));
                    if (distance > radius)
                        continue;
                    const auto pixel = std::size_t(y) * source.width + x;
                    auto& mask = writeMask(pixel);
                    if (mask.radius == 0)
                        activePixels.push_back(pixel);
                    // A smaller companion circle must not replace a fully
                    // opaque primary mask with its own feathered edge.
                    if (mask.radius == 0 || distance / radius < mask.distance / mask.radius)
                        mask = {distance, radius, float(point.ux), float(point.uy)};
                }
        }
    }
    if (progress)
        progress(.1);
    const auto sample = [&](double x, double y, std::array<double, 3>& value, bool excludeMasked = true) {
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        if (ix < 0 || iy < 0 || ix + 1 >= int(source.width) || iy + 1 >= int(source.height))
            return false;
        if (excludeMasked && !safeBackground(x, y))
            return false;
        value = {};
        const double fx = x - ix, fy = y - iy;
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const auto pixel = std::size_t(iy + dy) * source.width + ix + dx;
                if (excludeMasked && readMask(pixel).radius > 0)
                    return false;
                const auto p = pixel * source.channels;
                if (source.channels == 4 && source.pixels[p + 3] <= 0)
                    return false;
                for (int c = 0; c < 3; ++c) {
                    if (!std::isfinite(source.pixels[p + c]))
                        return false;
                    value[c] += source.pixels[p + c] * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                }
            }
        return true;
    };
    const auto ridgeContrast = [&](double x, double y, double ux, double uy) {
        std::array<double, 3> center{}, left{}, right{};
        if (!sample(x, y, center, false) || !sample(x - uy * 3, y + ux * 3, left, false) ||
            !sample(x + uy * 3, y - ux * 3, right, false))
            return 0.;
        double contrast = 0;
        for (int c = 0; c < 3; ++c)
            contrast += std::max(0., center[c] - .5 * (left[c] + right[c])) / 3;
        return contrast;
    };
    long traceX = -1, traceY = -1;
    if (const char* probe = std::getenv("PHOTONSTACK_PROFILE_REPAIR_PIXEL")) {
        char* end = nullptr;
        traceX = std::strtol(probe, &end, 10);
        if (end && *end == ',')
            traceY = std::strtol(end + 1, nullptr, 10);
    }
    // Source sampling is immutable, so traversal order cannot affect pixels.
    for (std::size_t index = 0; index < activePixels.size(); ++index) {
        if (progress && index % 2048 == 0)
            progress(.1 + .9 * double(index) / activePixels.size());
        const auto pixel = activePixels[index];
        const auto x = unsigned(pixel % source.width), y = unsigned(pixel / source.width);
        const auto m = readMask(pixel);
        // A verified flight may cross behind a foreground object between
        // its measured lamps. Do not repair textured foreground or sample it
        // as sky: doing either can carve rooftops into adjacent sky pixels.
        if (!safeBackground(x, y))
            continue;
        const auto offset = pixel * source.channels;
        const bool tracePixel = long(x) == traceX && long(y) == traceY;
        if (source.channels == 4 && source.pixels[offset + 3] <= 0)
            continue;
        std::array<std::vector<double>, 3> background;
        const double nx = -m.uy, ny = m.ux, r = m.radius * 2 + 3;
        for (int side : {-1, 1})
            for (int shift = -2; shift <= 2; ++shift) {
                std::array<double, 3> s{};
                if (sample(x + nx * r * side + m.ux * shift * 3, y + ny * r * side + m.uy * shift * 3, s))
                    for (int c = 0; c < 3; ++c)
                        background[c].push_back(s[c]);
            }
        if (background[0].size() < 4)
            continue;
        std::array<double, 3> bg{};
        for (int c = 0; c < 3; ++c)
            bg[c] = median(background[c]);
        // Look for bilateral stellar structure through the mask. Only samples
        // outside *all* aircraft corridors may reconstruct a crossing star.
        std::array<double, 3> replacement = bg;
        double bestSupport = .002;
        for (int k = 0; k < 48; ++k) {
            const double angle = k * 3.141592653589793 / 48, ux = std::cos(angle), uy = std::sin(angle);
            const double normal = std::abs(ux * nx + uy * ny);
            if (normal < .45)
                continue;
            const double distance = (m.radius + m.distance + 3) / normal;
            std::array<double, 3> a{}, b{};
            if (!sample(x + ux * distance, y + uy * distance, a) || !sample(x - ux * distance, y - uy * distance, b))
                continue;
            double support = 0;
            for (int c = 0; c < 3; ++c)
                support += std::max(0., std::min(a[c], b[c]) - bg[c]) / 3;
            if (support > bestSupport) {
                // Two unrelated bright samples on opposite sides do not
                // establish a crossing star. Its ridge must also be present
                // between the samples and the pixel being reconstructed.
                bool continuous = true;
                for (int side : {-1, 1}) {
                    int connected = 0, checked = 0;
                    for (double d = 3; d < distance; d += 3) {
                        connected +=
                            ridgeContrast(x + ux * d * side, y + uy * d * side, ux, uy) > std::max(.001, support * .25);
                        ++checked;
                    }
                    continuous &= checked > 0 && connected >= checked * .8;
                }
                if (continuous) {
                    if (tracePixel)
                        std::clog << "repair bilateral angle=" << angle << " support=" << support << '\n';
                    bestSupport = support;
                    for (int c = 0; c < 3; ++c)
                        replacement[c] = std::max(bg[c], std::min(a[c], b[c]));
                }
            }
            // A short stellar exposure can terminate inside the corridor.
            // Accept one intact arm only if several unmasked samples form
            // the same ridge and its color agrees with the current pixel.
            // This excludes isolated red/green aircraft flashes even when
            // an unrelated star happens to lie farther along a ray.
            for (int side : {-1, 1}) {
                const auto& near = side > 0 ? a : b;
                std::array<double, 3> far{}, farther{}, candidate{};
                if (!sample(x + ux * (distance + 3) * side, y + uy * (distance + 3) * side, far) ||
                    !sample(x + ux * (distance + 6) * side, y + uy * (distance + 6) * side, farther))
                    continue;
                double armSupport = 0, minRatio = 1e9, maxRatio = 0;
                for (int c = 0; c < 3; ++c) {
                    const auto signal = std::max(0., std::min({near[c], far[c], farther[c]}) - bg[c]);
                    candidate[c] = bg[c] + signal;
                    armSupport += signal / 3;
                    const auto ratio = std::max(0., source.pixels[offset + c] - bg[c]) / std::max(.001, signal);
                    minRatio = std::min(minRatio, ratio);
                    maxRatio = std::max(maxRatio, ratio);
                }
                if (armSupport > std::max(.004, bestSupport) && minRatio > .5 * maxRatio) {
                    // An aircraft strobe can have the same color as a nearby
                    // star. Require the ridge to reach this pixel, not merely
                    // three collinear samples beyond a gap of empty sky.
                    int connected = 0, checked = 0;
                    for (double d = 3; d < distance; d += 3) {
                        // Compare across the ridge locally: a smooth sky
                        // gradient brighter than the remote background is
                        // not a continuous stellar arm.
                        connected += ridgeContrast(x + ux * d * side, y + uy * d * side, ux, uy) >
                                     std::max(.001, armSupport * .25);
                        ++checked;
                    }
                    if (checked > 0 && connected >= checked * .8) {
                        if (tracePixel)
                            std::clog << "repair arm angle=" << angle << " side=" << side << " support=" << armSupport
                                      << " connected=" << connected << '/' << checked << '\n';
                        bestSupport = armSupport;
                        replacement = candidate;
                    }
                }
            }
        }
        // Do not repaint the quiet sky. Restrict changes to positive residuals;
        // this also keeps noise and dark structure outside the lights intact.
        double excess = 0;
        for (int c = 0; c < 3; ++c)
            excess = std::max(excess, source.pixels[offset + c] - replacement[c]);
        if (excess < .0015)
            continue;
        const double alpha = std::clamp((m.radius - m.distance) / std::max(1.F, m.radius * .18F), 0.F, 1.F);
        if (tracePixel) {
            std::clog << "repair-pixel distance=" << m.distance << " radius=" << m.radius << " alpha=" << alpha
                      << " support=" << bestSupport << '\n';
            for (int c = 0; c < 3; ++c)
                std::clog << "repair-channel " << c << " source=" << source.pixels[offset + c]
                          << " background=" << bg[c] << " replacement=" << replacement[c] << '\n';
        }
        for (int c = 0; c < 3; ++c) {
            const double original = source.pixels[offset + c];
            if (!std::isfinite(original))
                continue;
            output.pixels[offset + c] = float(original - alpha * std::max(0., original - replacement[c]));
        }
    }
    if (progress)
        progress(1);
}
} // namespace photonstack::detail
