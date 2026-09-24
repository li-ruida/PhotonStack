#include "photonstack/CfaDrizzle.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace photonstack {
namespace {
struct Point { double x, y; };
struct Polygon { std::array<Point, 12> p{}; int size = 0; };
Polygon clip(const Polygon& in, int axis, double bound, bool greater) {
    Polygon out;
    if (!in.size) return out;
    auto coord = [axis](Point p) { return axis ? p.y : p.x; };
    Point a = in.p[in.size - 1];
    bool ain = greater ? coord(a) >= bound : coord(a) <= bound;
    for (int i = 0; i < in.size; ++i) {
        Point b = in.p[i];
        bool bin = greater ? coord(b) >= bound : coord(b) <= bound;
        if (ain != bin) {
            double t = (bound - coord(a)) / (coord(b) - coord(a));
            out.p[out.size++] = {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
        }
        if (bin) out.p[out.size++] = b;
        a = b; ain = bin;
    }
    return out;
}
double overlap(const Polygon& drop, int x, int y) {
    // Local coordinates avoid cancellation of large absolute positions in the area sum.
    Polygon p = drop;
    for (int i = 0; i < p.size; ++i) { p.p[i].x -= x; p.p[i].y -= y; }
    p = clip(p, 0, -.5, true); p = clip(p, 0, .5, false);
    p = clip(p, 1, -.5, true); p = clip(p, 1, .5, false);
    double twice = 0;
    for (int i = 0; i < p.size; ++i) {
        const auto a = p.p[i], b = p.p[(i + 1) % p.size];
        twice += a.x * b.y - a.y * b.x;
    }
    return std::abs(twice) * .5;
}
bool finite(double v) { return std::isfinite(v); }
void require(bool test, const char* message) { if (!test) throw std::invalid_argument(message); }
}

unsigned CfaDrizzleAccumulator::colorAt(BayerPattern pattern, int x, int y, int xo, int yo) {
    static constexpr unsigned colors[4][4] = {{0,1,1,2},{1,0,2,1},{1,2,0,1},{2,1,1,0}};
    const auto p = static_cast<unsigned>(pattern);
    require(p < 4, "Unsupported Bayer pattern");
    const auto px = ((x % 2 + xo % 2) + 4) % 2;
    const auto py = ((y % 2 + yo % 2) + 4) % 2;
    return colors[p][py * 2 + px];
}

CfaDrizzleAccumulator::CfaDrizzleAccumulator(CfaDrizzleOptions o) : options_(o) {
    require(o.width > 0 && o.height > 0 && o.width <= 1000000 && o.height <= 1000000,
            "Invalid CFA drizzle canvas dimensions");
    require(finite(o.originX) && finite(o.originY) && finite(o.scale) && o.scale >= .01 && o.scale <= 16 &&
            finite(o.pixfrac) && o.pixfrac >= .001 && o.pixfrac <= 1, "Invalid CFA drizzle canvas geometry");
    const auto count = static_cast<std::size_t>(o.width) * o.height;
    require(count <= sums_.max_size() / 3, "CFA drizzle canvas too large");
    sums_.resize(count * 3); weights_.resize(count * 3);
}

void CfaDrizzleAccumulator::add(const CfaDrizzleFrame& f) {
    require(f.width > 0 && f.height > 0 && f.width <= 1000000 && f.height <= 1000000,
            "Invalid CFA source dimensions");
    const auto count = static_cast<std::size_t>(f.width) * f.height;
    require(f.samples.size() == count && (f.validity.empty() || f.validity.size() == count),
            "CFA source storage mismatch");
    (void)colorAt(f.pattern, 0, 0);
    require(finite(f.weight) && f.weight > 0 && f.weight <= 1e12, "Invalid CFA frame weight");
    for (unsigned c = 0; c < 3; ++c)
        require(finite(f.background[c]) && finite(f.gain[c]) && f.gain[c] > 0, "Invalid CFA calibration");
    for (double v : f.transform) require(finite(v), "Nonfinite CFA transform");
    const auto [a,b,c,d,tx,ty] = f.transform;
    const double det = a*d-b*c;
    require(finite(det) && std::abs(det) >= 1e-8 &&
            std::max({std::abs(a),std::abs(b),std::abs(c),std::abs(d)}) <= 16,
            "Singular or excessive CFA transform");
    const auto& o = options_;
    auto map = [&](double x, double y) -> Point {
        return {o.scale * (a*x+b*y+tx-o.originX+.5)-.5,
                o.scale * (c*x+d*y+ty-o.originY+.5)-.5};
    };
    for (double x : {-.5, double(f.width)-.5}) for (double y : {-.5, double(f.height)-.5}) {
        auto p = map(x, y);
        require(finite(p.x) && finite(p.y) && std::abs(p.x) < 1e12 && std::abs(p.y) < 1e12,
                "CFA transform outside supported coordinate range");
    }
    // Restrict work to sensor centers whose transformed droplets can intersect the canvas.
    double xmin = double(f.width), xmax = -1, ymin = double(f.height), ymax = -1;
    for (double x : {0., double(o.width)}) for (double y : {0., double(o.height)}) {
        double qx = x/o.scale + o.originX-.5-tx, qy = y/o.scale + o.originY-.5-ty;
        double sx = (d*qx-b*qy)/det, sy = (-c*qx+a*qy)/det;
        xmin=std::min(xmin,sx); xmax=std::max(xmax,sx); ymin=std::min(ymin,sy); ymax=std::max(ymax,sy);
    }
    const double half = o.pixfrac * .5;
    auto bound = [](double v, unsigned size) { return int(std::clamp(v, 0., double(size))); };
    const int x0=bound(std::floor(xmin-half),f.width), x1=bound(std::ceil(xmax+half)+1,f.width);
    const int y0=bound(std::floor(ymin-half),f.height), y1=bound(std::ceil(ymax+half)+1,f.height);
    const double area = std::abs(det)*o.scale*o.scale*o.pixfrac*o.pixfrac;
    // Reject calibration overflow before changing any sums. Scientific output is Float32.
    for (int y=y0; y<y1; ++y) for (int x=x0; x<x1; ++x) {
        const auto src=std::size_t(y)*f.width+x;
        const double valid=f.validity.empty()?1:f.validity[src];
        if (!finite(f.samples[src]) || !finite(valid) || valid<=0 || valid>1) continue;
        const auto ch=colorAt(f.pattern,x,y,f.xOffset,f.yOffset);
        const double value=(f.samples[src]-f.background[ch])*f.gain[ch];
        require(finite(value) && std::abs(value)<=std::numeric_limits<float>::max(),
                "Calibrated CFA sample exceeds Float32 scientific range");
    }
    for (int y=y0; y<y1; ++y) for (int x=x0; x<x1; ++x) {
        const auto src = std::size_t(y)*f.width+x;
        const double valid = f.validity.empty() ? 1 : f.validity[src];
        if (!finite(f.samples[src]) || !finite(valid) || valid <= 0 || valid > 1) continue;
        const auto ch = colorAt(f.pattern, x, y, f.xOffset, f.yOffset);
        const double value = (f.samples[src]-f.background[ch])*f.gain[ch];
        if (!finite(value)) continue;
        Polygon drop;
        drop.size=4; drop.p[0]=map(x-half,y-half); drop.p[1]=map(x+half,y-half);
        drop.p[2]=map(x+half,y+half); drop.p[3]=map(x-half,y+half);
        double lx=drop.p[0].x, hx=lx, ly=drop.p[0].y, hy=ly;
        for (int k=1;k<4;++k) { lx=std::min(lx,drop.p[k].x); hx=std::max(hx,drop.p[k].x);
            ly=std::min(ly,drop.p[k].y); hy=std::max(hy,drop.p[k].y); }
        const int ox0=bound(std::floor(lx+.5),o.width), ox1=bound(std::ceil(hx+.5),o.width);
        const int oy0=bound(std::floor(ly+.5),o.height), oy1=bound(std::ceil(hy+.5),o.height);
        for (int oy=oy0;oy<oy1;++oy) for (int ox=ox0;ox<ox1;++ox) {
            const double w = overlap(drop,ox,oy)/area * f.weight * valid;
            const auto dest = (std::size_t(oy)*o.width+ox)*3+ch;
            sums_[dest] += w*value; weights_[dest] += w;
        }
    }
}

ImageBuffer CfaDrizzleAccumulator::image() const {
    ImageBuffer out; out.width=options_.width; out.height=options_.height;
    out.pixels.resize(out.sampleCount());
    for (std::size_t i=0;i<out.pixelCount();++i) {
        bool complete=true;
        for (unsigned c=0;c<3;++c) {
            const auto k=i*3+c;
            if (weights_[k]>0) out.pixels[i*4+c]=static_cast<float>(sums_[k]/weights_[k]);
            else { out.pixels[i*4+c]=std::numeric_limits<float>::quiet_NaN(); complete=false; }
        }
        out.pixels[i*4+3]=complete?1.F:0.F;
    }
    return out;
}
} // namespace photonstack
