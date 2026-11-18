#include "photonstack/SpectralCalibrator.hpp"
#include "MaskedImageSampling.hpp"
#include "photonstack/StarDetector.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
namespace photonstack {
namespace {
void checkImage(const ImageBuffer& image) {
    if (image.empty() || (image.channels != 3 && image.channels != 4) || image.pixels.size() != image.sampleCount() ||
        image.colorEncoding != ColorEncoding::Linear)
        throw std::invalid_argument("Spectral calibration requires linear RGB");
}
// Bounded local neighbor lookup; an incomplete detection list is never used as
// proof of isolation. Catalog neighbors also cover close unresolved pairs.
class Neighbors {
    using Cell = std::pair<int, int>;
    std::map<Cell, std::vector<SkyPoint>> cells;
    double size;
    Cell cell(double x, double y) const { return {int(std::floor(x / size)), int(std::floor(y / size))}; }

  public:
    explicit Neighbors(double size) : size(size) {}
    void add(double x, double y) { cells[cell(x, y)].push_back({x, y}); }
    unsigned count(double x, double y, double radius) const {
        const auto [cx, cy] = cell(x, y);
        const int reach = int(std::ceil(radius / size));
        unsigned count = 0;
        for (int dy = -reach; dy <= reach; ++dy)
            for (int dx = -reach; dx <= reach; ++dx) {
                const auto found = cells.find({cx + dx, cy + dy});
                if (found == cells.end())
                    continue;
                for (const auto& p : found->second)
                    count += std::hypot(p.x - x, p.y - y) <= radius;
            }
        return count;
    }
};
} // namespace
SpectralCalibrationReview reviewSpectralCalibration(const ImageBuffer& image, const SolvedPanel& sky,
                                                    const std::vector<SpectralReferenceStar>& stars,
                                                    const std::array<SpectralBand, 3>& response,
                                                    const SampledSpectrum& white,
                                                    const SpectralCalibrationOptions& options) {
    checkImage(image);
    const auto& grid = sky.grid;
    for (double v : {grid.x, grid.y, grid.ra, grid.dec, grid.cd[0], grid.cd[1], grid.cd[2], grid.cd[3]})
        if (!std::isfinite(v))
            throw std::invalid_argument("Nonfinite spectral calibration WCS");
    const double determinant = grid.cd[0] * grid.cd[3] - grid.cd[1] * grid.cd[2];
    if (grid.ra < 0 || grid.ra >= 360 || grid.dec < -90 || grid.dec > 90 || !std::isfinite(determinant) ||
        determinant == 0)
        throw std::invalid_argument("Invalid spectral calibration WCS");
    if (stars.empty() || stars.size() > 10000 || !std::isfinite(options.maxRelativeSpectralError) ||
        options.maxRelativeSpectralError <= 0 || options.maxRelativeSpectralError > .5 ||
        !std::isfinite(options.maxApertureColorChange) || options.maxApertureColorChange <= 0 ||
        options.maxApertureColorChange > .2 || options.aperture.radius > 18 || options.aperture.skyOuter > 38 ||
        !std::isfinite(options.maxValidationLogError) || options.maxValidationLogError <= 0 ||
        options.maxValidationLogError > 1)
        throw std::invalid_argument("Invalid spectral calibration selection or limits");
    // Reuse aperture option validation even when every candidate is off-frame.
    measureStellarAperture(image, 0, 0, options.aperture);
    std::set<std::string> ids;
    std::size_t totalSamples = 0;
    for (const auto& s : stars)
        if (s.id.empty() || s.id.size() > 128 || !ids.insert(s.id).second || !std::isfinite(s.ra) ||
            !std::isfinite(s.dec) || s.ra < 0 || s.ra >= 360 || s.dec < -90 || s.dec > 90)
            throw std::invalid_argument("Invalid spectral reference identity or ICRS position");
    for (const auto& s : stars) {
        if (s.spectrum.flux.wavelength.size() > 100000)
            throw std::invalid_argument("Spectral reference has too many samples");
        totalSamples += s.spectrum.flux.wavelength.size();
    }
    if (totalSamples > 4000000)
        throw std::invalid_argument("Spectral references exceed four million samples");
    for (const auto& id : options.excluded)
        if (!ids.contains(id))
            throw std::invalid_argument("Excluded spectral reference does not exist");
    SpectralCalibrationReview result;
    result.width = image.width;
    result.height = image.height;
    for (unsigned c = 0; c < 3; ++c) {
        result.whiteFlux[c] = integratePhotonSpectrum(white, response[c]).value;
        if (result.whiteFlux[c] <= 0)
            throw std::invalid_argument("White reference has nonpositive synthetic signal");
    }
    std::vector<float> luminance(image.pixelCount()), coverage(image.pixelCount());
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        if (!detail::pixelHasValidColor(image, p) || detail::pixelCoverage(image, p) < .999999F)
            continue;
        const auto* color = &image.pixels[p * image.channels];
        luminance[p] = float(.2126 * color[0] + .7152 * color[1] + .0722 * color[2]);
        coverage[p] = 1;
    }
    StarDetectionOptions detect;
    detect.robustStatistics = true;
    detect.sigmaThreshold = 3;
    detect.minPeak = std::numeric_limits<float>::lowest();
    detect.maxStars = 100000;
    detect.countAllDetections = true;
    const auto detections =
        StarDetector().detectLuminanceWithCoverage(image.width, image.height, luminance, coverage, detect);
    if (!detections.ok)
        throw std::runtime_error(detections.message);
    result.detected = detections.detectedCount;
    if (detections.detectedCount != detections.stars.size())
        throw std::runtime_error("Too many detections to verify spectral reference isolation");
    const double separation = options.aperture.radius + 4;
    Neighbors detected(separation), catalog(separation);
    for (const auto& s : detections.stars)
        detected.add(s.x, s.y);
    result.stars.resize(stars.size());
    for (std::size_t i = 0; i < stars.size(); ++i) {
        auto& r = result.stars[i];
        const auto& s = stars[i];
        r.id = s.id;
        const auto point = SkyProjection(sky, {1, 1, s.ra, s.dec, {1, 0, 0, 1}}).toPanel(0, 0);
        r.hasPosition = std::isfinite(point.x) && std::isfinite(point.y);
        if (r.hasPosition) {
            r.predictedX = point.x;
            r.predictedY = point.y;
        }
        if (!r.hasPosition || point.x < 0 || point.y < 0 || point.x >= image.width || point.y >= image.height) {
            r.status = "outside";
            continue;
        }
        catalog.add(point.x, point.y);
    }
    std::vector<SpectralColorSample> eligible;
    auto expanded = options.aperture;
    expanded.radius += 2;
    expanded.skyInner += 2;
    expanded.skyOuter += 2;
    for (std::size_t i = 0; i < stars.size(); ++i) {
        auto& r = result.stars[i];
        if (options.excluded.contains(r.id)) {
            r.status = "excluded";
            continue;
        }
        if (!r.status.empty())
            continue;
        if (catalog.count(r.predictedX, r.predictedY, separation) != 1) {
            r.status = "catalog-neighbor";
            continue;
        }
        const auto& spectrum = stars[i].spectrum;
        if (spectrum.error.empty()) {
            r.status = "spectral-error-missing";
            continue;
        }
        for (unsigned c = 0; c < 3; ++c) {
            const auto signal = integratePhotonSpectrum(spectrum, response[c]);
            r.predicted[c] = signal.value;
            r.spectralErrorBound[c] = signal.errorBound;
            if (signal.value <= 0 || signal.errorBound > options.maxRelativeSpectralError * signal.value)
                r.status = "spectral-error";
        }
        if (!r.status.empty())
            continue;
        r.photometry = measureStellarAperture(image, r.predictedX, r.predictedY, options.aperture);
        if (!r.photometry.valid) {
            r.status = r.photometry.status;
            continue;
        }
        const double x = r.photometry.x, y = r.photometry.y;
        if (detected.count(x, y, 2) != 1) {
            r.status = "detection-mismatch";
            continue;
        }
        if (detected.count(x, y, separation) != 1) {
            r.status = "detected-neighbor";
            continue;
        }
        r.expandedPhotometry = measureStellarAperture(image, x, y, expanded);
        if (!r.expandedPhotometry.valid) {
            r.status = "expanded-" + r.expandedPhotometry.status;
            continue;
        }
        for (unsigned c = 0; c < 3; ++c) {
            const double a = r.photometry.flux[c] / r.photometry.flux[1];
            const double b = r.expandedPhotometry.flux[c] / r.expandedPhotometry.flux[1];
            r.apertureColorChange[c] = std::log(b) - std::log(a);
            if (!std::isfinite(r.apertureColorChange[c]) ||
                std::abs(r.apertureColorChange[c]) > options.maxApertureColorChange)
                r.status = "aperture-color-unstable";
        }
        if (!r.status.empty())
            continue;
        r.status = "eligible";
        eligible.push_back({r.id, x, y, r.photometry.flux, r.predicted});
    }
    result.fit =
        fitSpectralColors(eligible, result.whiteFlux, image.width, image.height, options.maxValidationLogError);
    std::map<std::string, std::size_t> reviewIndex;
    for (std::size_t i = 0; i < result.stars.size(); ++i)
        reviewIndex.emplace(result.stars[i].id, i);
    for (const auto& fitted : result.fit.stars)
        result.stars[reviewIndex.at(fitted.id)].status = fitted.status;
    return result;
}
ImageBuffer applySpectralCalibration(const ImageBuffer& source, const SpectralColorFit& fit) {
    checkImage(source);
    if (!fit.valid)
        throw std::invalid_argument("Spectral calibration did not pass held-out validation");
    for (double gain : fit.gains)
        if (!std::isfinite(gain) || gain < .05 || gain > 20)
            throw std::invalid_argument("Invalid spectral channel gain");
    auto image = source;
    for (std::size_t p = 0; p < image.pixelCount(); ++p) {
        if (!detail::pixelHasValidColor(image, p) || detail::pixelCoverage(image, p) <= 0)
            continue;
        for (unsigned c = 0; c < 3; ++c) {
            const double value = double(source.pixels[p * source.channels + c]) * fit.gains[c];
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                throw std::runtime_error("Spectral calibration exceeds Float32 science range");
            image.pixels[p * image.channels + c] = float(value);
        }
    }
    return image;
}
} // namespace photonstack
