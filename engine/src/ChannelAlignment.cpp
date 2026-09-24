#include "photonstack/ChannelAlignment.hpp"
#include "photonstack/StarCentroid.hpp"
#include "photonstack/StarDetector.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace photonstack {
namespace {
double median(std::vector<double> a) {
    auto mid = a.begin() + a.size() / 2;
    std::nth_element(a.begin(), mid, a.end());
    return *mid;
}
int reflect(int x, int n) {
    const int period = 2 * (n - 1);
    x %= period;
    if (x < 0)
        x += period;
    return x < n ? x : period - x;
}
std::array<double, 6> kernel(double fraction) {
    std::array<double, 6> w{};
    double sum = 0;
    for (int k = 0; k < 6; ++k) {
        const double x = k - 2 - fraction;
        const double a = std::numbers::pi * x;
        w[k] = std::abs(x) < 1e-10 ? 1 : std::sin(a) * std::sin(a / 3) / (a * a / 3);
        sum += w[k];
    }
    for (auto& v : w)
        v /= sum;
    return w;
}
bool resampleChannel(const ImageBuffer& input, ImageBuffer& output, unsigned channel, double dx, double dy) {
    const int ix = int(std::floor(dx)), iy = int(std::floor(dy));
    const auto wx = kernel(dx - ix), wy = kernel(dy - iy);
    std::vector<float> temp(input.pixelCount());
    for (unsigned y = 0; y < input.height; ++y)
        for (unsigned x = 0; x < input.width; ++x) {
            double sum = 0;
            for (int k = 0; k < 6; ++k)
                sum += wx[k] * input.pixels[(std::size_t(y) * input.width + reflect(int(x) + ix + k - 2, input.width)) * input.channels + channel];
            temp[std::size_t(y) * input.width + x] = float(sum);
        }
    for (unsigned y = 0; y < input.height; ++y)
        for (unsigned x = 0; x < input.width; ++x) {
            double sum = 0;
            for (int k = 0; k < 6; ++k)
                sum += wy[k] * temp[std::size_t(reflect(int(y) + iy + k - 2, input.height)) * input.width + x];
            if (!std::isfinite(sum) || !std::isfinite(float(sum))) return false;
            output.pixels[(std::size_t(y) * input.width + x) * input.channels + channel] = float(sum);
        }
    return true;
}
} // namespace
ChannelAlignmentResult ChannelAlignment::apply(const ImageBuffer& im) const {
    ChannelAlignmentResult r;
    if (im.empty() || (im.channels != 3 && im.channels != 4) ||
        im.colorEncoding != ColorEncoding::Linear || im.pixels.size() != im.sampleCount()) {
        r.errorCode = "ImageBufferInvalid";
        r.message = "Channel alignment requires linear RGB";
        return r;
    }
    for (float v : im.pixels)
        if (!std::isfinite(v)) {
            r.errorCode = "ImageBufferInvalid";
            r.message = "Channel alignment requires finite pixels";
            return r;
        }
    r.image = im;
    r.ok = true;
    r.sourceWidth = im.width; r.sourceHeight = im.height;
    if (im.width < 25 || im.height < 25) {
        r.message = "Skipped channel alignment: image too small for stellar evidence";
        return r;
    }
    if (im.channels == 4)
        for (std::size_t p = 0; p < im.pixelCount(); ++p)
            if (im.pixels[p * 4 + 3] != 1) {
                r.message = "Skipped channel alignment: incomplete coverage";
                return r;
            }
    ImageBuffer green;
    green.width = im.width;
    green.height = im.height;
    green.channels = 1;
    green.format = PixelFormat::Float32Gray;
    green.pixels.resize(im.pixelCount());
    for (std::size_t p = 0; p < im.pixelCount(); ++p)
        green.pixels[p] = im.pixels[p * im.channels + 1];
    StarDetectionOptions o;
    o.minPeak = 0;
    o.sigmaThreshold = 8;
    o.maxStars = 512;
    o.border = 12;
    o.robustStatistics = true;
    const auto detected = StarDetector().detect(green, o);
    if (!detected.ok) {
        r.message = "Skipped channel alignment: no reliable stars";
        return r;
    }
    std::vector<StarCentroidFit> reference;
    for (const auto& s : detected.stars) {
        if (s.fwhm > 7 || s.eccentricity > .7)
            continue;
        if (std::any_of(reference.begin(), reference.end(),
                        [&](const auto& t) { return std::hypot(t.x - s.x, t.y - s.y) < 20; }))
            continue;
        const auto fit = fitStarCentroid(green, s.x, s.y);
        if (fit.ok)
            reference.push_back(fit);
        if (reference.size() == 128)
            break;
    }
    green = {};
    for (unsigned c : {0U, 2U}) {
        ImageBuffer plane;
        plane.width = im.width;
        plane.height = im.height;
        plane.channels = 1;
        plane.format = PixelFormat::Float32Gray;
        plane.pixels.resize(im.pixelCount());
        for (std::size_t p = 0; p < im.pixelCount(); ++p)
            plane.pixels[p] = im.pixels[p * im.channels + c];
        struct Sample {
            double dx, dy, x, y;
        };
        std::vector<Sample> samples;
        std::vector<double> xs, ys;
        for (const auto& ref : reference) {
            const auto fit = fitStarCentroid(plane, ref.x, ref.y);
            if (!fit.ok || std::hypot(fit.x - ref.x, fit.y - ref.y) > 2)
                continue;
            samples.push_back({fit.x - ref.x, fit.y - ref.y, ref.x, ref.y});
            xs.push_back(fit.x - ref.x);
            ys.push_back(fit.y - ref.y);
        }
        if (samples.size() < 12)
            continue;
        const double mx = median(xs), my = median(ys);
        std::vector<double> residuals;
        for (const auto& s : samples)
            residuals.push_back(std::hypot(s.dx - mx, s.dy - my));
        const double scatter = median(residuals), limit = std::max(.08, 3 * scatter);
        xs.clear();
        ys.clear();
        std::array<std::vector<double>, 4> qx, qy;
        for (const auto& s : samples)
            if (std::hypot(s.dx - mx, s.dy - my) <= limit) {
                xs.push_back(s.dx);
                ys.push_back(s.dy);
                const auto q = (s.x >= im.width * .5) + 2 * (s.y >= im.height * .5);
                qx[q].push_back(s.dx);
                qy[q].push_back(s.dy);
            }
        r.stars[c] = xs.size();
        r.scatter[c] = scatter;
        if (xs.size() < 12 || xs.size() < samples.size() * .75 || scatter > .10)
            continue;
        const double dx = median(xs), dy = median(ys), shift = std::hypot(dx, dy);
        r.dx[c] = dx;
        r.dy[c] = dy;
        unsigned quadrants = 0;
        bool coherent = true;
        for (unsigned q = 0; q < 4; ++q)
            if (qx[q].size() >= 3) {
                ++quadrants;
                if (std::hypot(median(qx[q]) - dx, median(qy[q]) - dy) > .10)
                    coherent = false;
            }
        if (!coherent || quadrants < 3 || shift < .05 || shift > 1.5 || shift < 5 * scatter / std::sqrt(xs.size()))
            continue;
        // Normalized separable Lanczos-3: no per-pixel range clamp, which can
        // bias stellar flux. Reflection is limited to the three-pixel boundary.
        if (!resampleChannel(im, r.image, c, dx, dy)) {
            r.ok = false; r.errorCode = "ImageValueInvalid";
            r.message = "Channel interpolation overflow"; return r;
        }
        r.applied[c] = true;
    }
    return r;
}
ChannelAlignmentResult ChannelAlignment::applyMeasured(const ImageBuffer& im, const ChannelAlignmentResult& measured) const {
    ChannelAlignmentResult r;
    const auto fail = [&](const char* message) {
        r.ok=false; r.errorCode="ChannelTransformInvalid"; r.message=message; return r;
    };
    if (!measured.ok || im.empty() || im.width!=measured.sourceWidth || im.height!=measured.sourceHeight ||
        (im.channels!=3 && im.channels!=4) || im.colorEncoding!=ColorEncoding::Linear ||
        im.pixels.size()!=im.sampleCount() || measured.applied[1])
        return fail("Measured channel transform requires matching linear RGB geometry");
    for(float value:im.pixels) if(!std::isfinite(value)) return fail("Nonfinite input sample");
    for(std::size_t p=0;p<im.pixelCount();++p)
        if(im.channels==4 && im.pixels[p*4+3]!=1) return fail("Full coverage is required for transform replay");
    for(unsigned c=0;c<3;++c)
        if(!std::isfinite(measured.dx[c]) || !std::isfinite(measured.dy[c]) ||
           (measured.applied[c] && (im.width<2 || im.height<2 || std::hypot(measured.dx[c],measured.dy[c])>1.5)))
            return fail("Invalid measured displacement");
    r.image=im; r.sourceWidth=im.width; r.sourceHeight=im.height;
    r.dx=measured.dx; r.dy=measured.dy; r.applied=measured.applied;
    r.scatter=measured.scatter; r.stars=measured.stars;
    for(unsigned c:{0U,2U}) if(measured.applied[c])
        if(!resampleChannel(im,r.image,c,measured.dx[c],measured.dy[c])) return fail("Channel interpolation overflow");
    r.ok=true; return r;
}
} // namespace photonstack
