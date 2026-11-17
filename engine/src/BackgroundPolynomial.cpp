#include "MaskedImageSampling.hpp"
#include "photonstack/BackgroundExtractor.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace photonstack {
namespace {
using Basis = std::array<double, 15>;
struct Sample {
    Basis basis;
    std::array<double, 3> color{};
    std::size_t measurement = 0;
};
double median(std::vector<double> v) {
    auto m = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), m, v.end());
    return *m;
}
Basis basisAt(double x, double y, const ImageBuffer& im) {
    x = 2 * x / im.width - 1;
    y = 2 * y / im.height - 1;
    return {1,
            x,
            y,
            x * x,
            x * y,
            y * y,
            x * x * x,
            x * x * y,
            x * y * y,
            y * y * y,
            x * x * x * x,
            x * x * x * y,
            x * x * y * y,
            x * y * y * y,
            y * y * y * y};
}
double evaluate(const Basis& b, const Basis& c) {
    return std::inner_product(b.begin(), b.end(), c.begin(), 0.0);
}
bool fit(const std::vector<Sample>& samples, int channel, int terms, Basis& coefficients, std::vector<bool>& keep) {
    coefficients.fill(0);
    keep.assign(samples.size(), true);
    for (int iteration = 0; iteration < 4; ++iteration) {
        double system[15][16] = {};
        std::size_t count = 0;
        for (std::size_t n = 0; n < samples.size(); ++n)
            if (keep[n]) {
                ++count;
                const auto& s = samples[n];
                for (int i = 0; i < terms; ++i) {
                    for (int j = 0; j < terms; ++j)
                        system[i][j] += s.basis[i] * s.basis[j];
                    system[i][terms] += s.basis[i] * s.color[channel];
                }
            }
        if (count < std::size_t(std::max(12, 2 * terms)))
            return false;
        for (int i = 0; i < terms; ++i) {
            int pivot = i;
            for (int j = i + 1; j < terms; ++j)
                if (std::abs(system[j][i]) > std::abs(system[pivot][i]))
                    pivot = j;
            if (std::abs(system[pivot][i]) < 1e-10)
                return false;
            for (int j = 0; j <= terms; ++j)
                std::swap(system[i][j], system[pivot][j]);
            const double divisor = system[i][i];
            for (int j = i; j <= terms; ++j)
                system[i][j] /= divisor;
            for (int k = 0; k < terms; ++k)
                if (k != i) {
                    const double factor = system[k][i];
                    for (int j = i; j <= terms; ++j)
                        system[k][j] -= factor * system[i][j];
                }
        }
        for (int i = 0; i < terms; ++i)
            coefficients[i] = system[i][terms];
        if (iteration == 3)
            break;
        std::vector<double> residuals;
        for (const auto& s : samples)
            residuals.push_back(s.color[channel] - evaluate(s.basis, coefficients));
        const double center = median(residuals);
        std::vector<double> deviations;
        for (double v : residuals)
            deviations.push_back(std::abs(v - center));
        const double threshold = std::max(1e-10, 4.4478 * median(deviations));
        for (std::size_t n = 0; n < samples.size(); ++n)
            keep[n] = std::abs(residuals[n] - center) <= threshold;
    }
    return true;
}
} // namespace
BackgroundExtractionResult BackgroundExtractor::extractPolynomial(const ImageBuffer& im,
                                                                  const BackgroundPolynomialOptions& opt) const {
    const auto fail = [](const char* code, const char* message) {
        BackgroundExtractionResult r;
        r.errorCode = code;
        r.message = message;
        return r;
    };
    if (im.empty() || im.channels == 0 || im.channels > 4 || im.pixels.size() != im.sampleCount() || im.width < 16 ||
        im.height < 16)
        return fail("ImageBufferInvalid", "Polynomial background requires an image at least 16 by 16 pixels");
    if (detail::imageHasInvalidCoveredColor(im))
        return fail("ImageBufferInvalid", "Covered image samples must be finite");
    if (opt.extraction.mode != BackgroundMode::Subtract || !std::isfinite(opt.extraction.strength) ||
        opt.extraction.strength < 0 || opt.extraction.strength > 1 || opt.columns < 4 || opt.rows < 4 ||
        opt.columns > 512 || opt.rows > 512 || opt.degree < 1 || opt.degree > 4)
        return fail(
            "ArgumentInvalid",
            "Polynomial background requires subtraction, strength 0...1, degree 1...4, and grid dimensions 4...512");
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
    std::vector<Sample> samples;
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
        Sample sample;
        sample.basis = basisAt(x, y, im);
        sample.measurement = result.measurements.size();
        for (int c = 0; c < channels; ++c)
            sample.color[c] = measurement.measured[c] = median(values[c]);
        record("sampled");
        samples.push_back(sample);
    }
    std::array<Basis, 3> coefficients;
    const int terms = int((opt.degree + 1) * (opt.degree + 2) / 2);
    if (samples.size() < std::size_t(std::max(12, 2 * terms)))
        return fail("BackgroundSamplesInsufficient",
                    "Need at least twelve sky samples and twice as many samples as polynomial coefficients");
    std::array<std::vector<bool>, 3> kept;
    for (int c = 0; c < channels; ++c)
        if (!fit(samples, c, terms, coefficients[c], kept[c]))
            return fail("BackgroundSamplesInsufficient",
                        "Sky samples do not constrain the selected polynomial background surface");
    result.image = im;
    if (opt.diagnostics) {
        result.modelImage = im;
        result.differenceImage = im;
    }
    for (int c = 0; c < channels; ++c) {
        double squares = 0;
        std::size_t count = 0;
        for (std::size_t n = 0; n < samples.size(); ++n) {
            auto& m = result.measurements[samples[n].measurement];
            m.fitted[c] = evaluate(samples[n].basis, coefficients[c]);
            m.used[c] = kept[c][n];
            if (m.used[c]) {
                const auto d = m.measured[c] - m.fitted[c];
                squares += d * d;
                ++count;
            }
        }
        result.residualRMS[c] = std::sqrt(squares / std::max<std::size_t>(1, count));
    }
    for (const auto& s : samples) {
        auto& m = result.measurements[s.measurement];
        const int used = int(std::count(m.used.begin(), m.used.begin() + channels, true));
        m.status = used == channels ? "used" : used > 0 ? "partial" : "outlier";
    }
    result.sampledGridCells = static_cast<std::uint32_t>(samples.size());
    result.gridColumns = opt.columns;
    result.gridRows = opt.rows;
    std::array<double, 3> pedestal{};
    for (int c = 0; c < channels; ++c) {
        std::vector<double> v;
        for (const auto& s : samples)
            v.push_back(evaluate(s.basis, coefficients[c]));
        pedestal[c] = median(v);
    }
    result.background = static_cast<float>(pedestal[0]);
    for (std::uint32_t y = 0; y < im.height; ++y)
        for (std::uint32_t x = 0; x < im.width; ++x) {
            const auto p = static_cast<std::size_t>(y) * im.width + x;
            if (!detail::pixelHasValidColor(im, p)) {
                detail::clearMaskedPixel(result.image, p);
                if (opt.diagnostics) {
                    detail::clearMaskedPixel(result.modelImage, p);
                    detail::clearMaskedPixel(result.differenceImage, p);
                }
                continue;
            }
            const auto b = basisAt(x, y, im);
            for (int c = 0; c < channels; ++c) {
                const auto o = p * im.channels + c;
                const double value =
                    im.pixels[o] - opt.extraction.strength * (evaluate(b, coefficients[c]) -
                                                              (opt.extraction.preserveBrightness ? pedestal[c] : 0));
                if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                    return fail("ImageValueInvalid", "Background subtraction exceeds Float32 range");
                result.image.pixels[o] =
                    static_cast<float>(opt.extraction.clampOutput ? std::clamp(value, 0.0, 1.0) : value);
                if (opt.diagnostics) {
                    const double background = evaluate(b, coefficients[c]);
                    const double difference = double(im.pixels[o]) - result.image.pixels[o];
                    if (!std::isfinite(background) || std::abs(background) > std::numeric_limits<float>::max() ||
                        !std::isfinite(difference) || std::abs(difference) > std::numeric_limits<float>::max())
                        return fail("ImageValueInvalid", "Background model exceeds Float32 range");
                    result.modelImage.pixels[o] = float(background);
                    result.differenceImage.pixels[o] = float(difference);
                }
            }
        }
    if (!opt.diagnostics)
        result.measurements.clear();
    result.ok = true;
    return result;
}
} // namespace photonstack
