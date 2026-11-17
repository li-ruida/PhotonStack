#include "BackgroundSurface.hpp"
#include <set>
namespace photonstack::detail {
namespace {
double median(std::vector<double> values) {
    return backgroundMedian(std::move(values));
}
} // namespace
BackgroundExtractionResult measureBackgroundSamples(const ImageBuffer& im, const BackgroundPolynomialOptions& opt) {
    const auto fail = [](const char* code, const char* message) {
        BackgroundExtractionResult r;
        r.errorCode = code;
        r.message = message;
        return r;
    };
    if (im.empty() || im.channels == 0 || im.channels > 4 || im.pixels.size() != im.sampleCount() || im.width < 16 ||
        im.height < 16)
        return fail("ImageBufferInvalid", "Background sampling requires an image at least 16 by 16 pixels");
    if (detail::imageHasInvalidCoveredColor(im))
        return fail("ImageBufferInvalid", "Covered image samples must be finite");
    if (opt.extraction.mode != BackgroundMode::Subtract || !std::isfinite(opt.extraction.strength) ||
        opt.extraction.strength < 0 || opt.extraction.strength > 1 || opt.columns < 4 || opt.rows < 4 ||
        opt.columns > 512 || opt.rows > 512 || opt.degree < 1 || opt.degree > 4)
        return fail(
            "ArgumentInvalid",
            "Background sampling requires subtraction, strength 0...1, degree 1...4, and grid dimensions 4...512");
    for (const auto& e : opt.exclusions)
        if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.major) || !std::isfinite(e.minor) ||
            !std::isfinite(e.angleDegrees) || e.major <= 0 || e.minor <= 0)
            return fail("ArgumentInvalid", "Background exclusion ellipses must be finite with positive semiaxes");
    const auto excluded = [&](double x, double y) {
        for (const auto& e : opt.exclusions) {
            const double angle = e.angleDegrees * 3.141592653589793 / 180;
            const double dx = x - e.x, dy = y - e.y;
            const double u = (dx * std::cos(angle) + dy * std::sin(angle)) / e.major;
            const double v = (-dx * std::sin(angle) + dy * std::cos(angle)) / e.minor;
            if (u * u + v * v <= 1)
                return true;
        }
        return false;
    };
    const int channels = std::min<int>(3, im.channels);
    BackgroundExtractionResult result;
    std::vector<BackgroundSamplePoint> points = opt.samples;
    if (opt.manualSamples && points.size() > 4096)
        return fail("ArgumentInvalid", "At most 4096 manual sky samples are supported");
    if (!opt.manualSamples) {
        points.clear();
        for (std::uint32_t gy = 0; gy < opt.rows; ++gy)
            for (std::uint32_t gx = 0; gx < opt.columns; ++gx)
                points.push_back({std::floor((gx + .5) * im.width / opt.columns),
                                  std::floor((gy + .5) * im.height / opt.rows), 4, true});
    }

    std::set<std::pair<int, int>> centers;
    for (const auto& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0 ||
            point.x > im.width - 1 || point.y > im.height - 1 || point.radius < 1 || point.radius > 64)
            return fail("ArgumentInvalid", "Sample centers must be inside the image and radius must be 1...64 pixels");
        BackgroundSampleMeasurement measurement;
        measurement.point = point;
        const auto record = [&](const char* reason) {
            measurement.status = reason;
            result.measurements.push_back(measurement);
        };
        if (!point.enabled) {
            record("disabled");
            continue;
        }
        const int x = int(std::lround(point.x)), y = int(std::lround(point.y)), radius = int(point.radius);
        if (opt.manualSamples && !centers.emplace(x, y).second)
            return fail("BackgroundSamplesDuplicate", "Enabled background samples must have distinct pixel centers");
        if (x < radius || y < radius || x + radius >= static_cast<int>(im.width) ||
            y + radius >= static_cast<int>(im.height)) {
            record("edge");
            continue;
        }
        std::array<std::vector<double>, 3> values;
        bool subject = false;
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx) {
                if (excluded(x + dx, y + dy)) {
                    subject = true;
                    continue;
                }
                const auto p = static_cast<std::size_t>(y + dy) * im.width + x + dx;
                if (!detail::pixelHasValidColor(im, p) || (im.channels == 4 && im.pixels[p * 4 + 3] < .99F))
                    continue;
                for (int c = 0; c < channels; ++c)
                    values[c].push_back(im.pixels[p * im.channels + c]);
            }
        if (subject) {
            record("excluded");
            continue;
        }
        if (values[0].size() <
            std::size_t(std::ceil((opt.manualSamples ? 1.0 : .74) * (2 * radius + 1) * (2 * radius + 1)))) {
            record("coverage");
            continue;
        }
        for (int c = 0; c < channels; ++c)
            measurement.measured[c] = median(values[c]);
        record("sampled");
    }
    result.ok = true;
    return result;
}
} // namespace photonstack::detail
