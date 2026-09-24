#include "photonstack/SequenceQualityAnalyzer.hpp"

#include "photonstack/ImageResizer.hpp"
#include "photonstack/StarCentroid.hpp"
#include "photonstack/StarDetector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace photonstack {
namespace {
constexpr double madScale = 1.482602218505602;

double median(std::vector<double> values) {
    if (values.empty()) return 0;
    const auto mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    const double upper = values[mid];
    return values.size() % 2 ? upper
        : (upper + *std::max_element(values.begin(), values.begin() + mid)) * 0.5;
}

double mad(const std::vector<double>& values, double center) {
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (double v : values) deviations.push_back(std::abs(v - center));
    return median(std::move(deviations)) * madScale;
}

bool validOptions(const SequenceQualityOptions& o) {
    const auto between = [](double v, double low, double high) {
        return std::isfinite(v) && v >= low && v <= high;
    };
    return o.sampleSide >= 8 && o.sampleSide <= 512 &&
        between(o.residualSigma, 2, 20) && between(o.signalTolerance, 0, 1) &&
        between(o.minimumCoverage, 0, 1) && between(o.maximumNoiseRatio, 1, 100) &&
        between(o.maximumFwhmRatio, 1, 100) && between(o.maximumEccentricity, 0, 1) &&
        between(o.minimumStarRatio, 0, 1) && between(o.maximumResidualFraction, 0, 1) &&
        o.minimumTemporalPeers >= 4 && o.minimumTemporalPeers <= 10000 &&
        o.minimumKeptFrames >= 1 && between(o.minimumKeptFraction, 0, 1);
}

double coverageAt(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    for (std::size_t c = 0; c < (image.channels == 4 ? 3 : image.channels); ++c)
        if (!std::isfinite(image.pixels[offset + c])) return 0;
    if (image.channels != 4) return 1;
    const float alpha = image.pixels[offset + 3];
    return std::isfinite(alpha) ? std::clamp(double(alpha), 0.0, 1.0) : 0;
}

double luminanceAt(const ImageBuffer& image, std::size_t pixel) {
    const auto offset = pixel * image.channels;
    return image.channels == 1 ? image.pixels[offset]
        : .2126 * image.pixels[offset] + .7152 * image.pixels[offset + 1] + .0722 * image.pixels[offset + 2];
}

bool validSample(const SequenceFrameSample& f) {
    const bool shapeOK = f.ok && f.width > 0 && f.height > 0 && f.columns > 0 && f.rows > 0 &&
        f.columns <= 512 && f.rows <= 512 &&
        f.luminance.size() == std::size_t(f.columns) * f.rows &&
        f.coverage.size() == f.luminance.size() && std::isfinite(f.sky) &&
        std::isfinite(f.noise) && f.noise >= 0 && std::isfinite(f.validFraction) &&
        f.validFraction > 0 && f.validFraction <= 1 &&
        std::isfinite(f.medianFwhm) && f.medianFwhm >= 0 &&
        std::isfinite(f.medianEccentricity) && f.medianEccentricity >= 0 && f.medianEccentricity <= 1 &&
        std::all_of(f.coverage.begin(), f.coverage.end(), [](float v) {
            return std::isfinite(v) && v >= 0 && v <= 1;
        });
    if (!shapeOK) return false;
    for (std::size_t p = 0; p < f.luminance.size(); ++p)
        if (f.coverage[p] > 0 && !std::isfinite(f.luminance[p])) return false;
    return true;
}

std::vector<SequenceTransientTrail> detectResidualLines(const std::vector<std::size_t>& points,
                                                       const SequenceFrameSample& grid, int sign) {
    std::vector<SequenceTransientTrail> result;
    // Broad clouds and gradients belong to frame assessment, not a trail label.
    if (points.size() < 8 || points.size() > grid.luminance.size() / 10) return result;
    constexpr int angles = 180;
    const int radius = int(std::ceil(std::hypot(grid.columns, grid.rows))) + 2, bins = radius * 2 + 1;
    std::vector<unsigned> votes(std::size_t(angles) * bins);
    double cosine[angles], sine[angles];
    for (int a = 0; a < angles; ++a) { cosine[a] = std::cos(a * 3.141592653589793 / angles); sine[a] = std::sin(a * 3.141592653589793 / angles); }
    for (auto p : points) {
        const double x = p % grid.columns, y = p / grid.columns;
        for (int a = 0; a < angles; ++a) ++votes[std::size_t(a) * bins + int(std::lround(x * cosine[a] + y * sine[a])) + radius];
    }
    auto peak = std::max_element(votes.begin(), votes.end());
    if (*peak < 8) return result;
    const auto index = std::size_t(peak - votes.begin());
    const auto angle = index / bins;
    const double rho = int(index % bins) - radius;
    std::vector<std::pair<double, std::size_t>> inliers;
    for (auto p : points) {
        const double x = p % grid.columns, y = p / grid.columns;
        if (std::abs(x * cosine[angle] + y * sine[angle] - rho) <= .6)
            inliers.emplace_back(-x * sine[angle] + y * cosine[angle], p);
    }
    std::sort(inliers.begin(), inliers.end());
    if (inliers.size() < 8) return result;
    const double span = inliers.back().first - inliers.front().first;
    // Require a long, substantially continuous narrow line. This is a candidate,
    // never a reason by itself to discard an entire exposure.
    if (span < 12 || inliers.size() / (span + 1) < .55) return result;
    const auto first = inliers.front().second, last = inliers.back().second;
    result.push_back({sign, inliers.size(), (first % grid.columns + .5) * grid.width / grid.columns,
        (first / grid.columns + .5) * grid.height / grid.rows,
        (last % grid.columns + .5) * grid.width / grid.columns,
        (last / grid.columns + .5) * grid.height / grid.rows});
    return result;
}
} // namespace

SequenceFrameSample SequenceQualityAnalyzer::sample(const ImageBuffer& image,
                                                    const SequenceQualityOptions& o) const {
    SequenceFrameSample result;
    if (!validOptions(o)) {
        result.errorCode = "ArgumentInvalid";
        result.message = "Invalid sequence quality options";
        return result;
    }
    if (image.empty() || image.pixels.size() != image.sampleCount() ||
        (image.channels != 1 && image.channels != 3 && image.channels != 4) ||
        image.colorEncoding != ColorEncoding::Linear) {
        result.errorCode = "ScientificImageRequired";
        result.message = "Sequence assessment requires a valid linear image";
        return result;
    }
    result.width = image.width;
    result.height = image.height;
    result.columns = std::min(o.sampleSide, image.width);
    result.rows = std::min(o.sampleSide, image.height);
    std::vector<double> skySamples, differences;
    double valid = 0;
    for (std::uint32_t row = 0; row < result.rows; ++row) {
        const auto y = std::min(image.height - 1,
            std::uint32_t((std::uint64_t(row) * 2 + 1) * image.height / (2 * result.rows)));
        for (std::uint32_t column = 0; column < result.columns; ++column) {
            const auto x = std::min(image.width - 1,
                std::uint32_t((std::uint64_t(column) * 2 + 1) * image.width / (2 * result.columns)));
            const auto p = std::size_t(y) * image.width + x;
            const double c = coverageAt(image, p);
            const double value = c > 0 ? luminanceAt(image, p) : 0;
            const bool representable = std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
            result.luminance.push_back(representable ? float(value) : 0);
            result.coverage.push_back(representable ? float(c) : 0);
            valid += representable ? c : 0;
            if (!representable || c < .99) continue;
            skySamples.push_back(value);
            // 2x2 diagonal Haar detail: a planar sky gradient cancels; independent
            // equal-variance samples retain their noise sigma (norm of coefficients = 1).
            if (x + 1 < image.width && y + 1 < image.height &&
                coverageAt(image, p + 1) >= .99 && coverageAt(image, p + image.width) >= .99 &&
                coverageAt(image, p + image.width + 1) >= .99) {
                const double detail = (value - luminanceAt(image, p + 1) -
                    luminanceAt(image, p + image.width) + luminanceAt(image, p + image.width + 1)) * .5;
                if (std::isfinite(detail)) differences.push_back(detail);
            }
        }
    }
    result.validFraction = valid / result.luminance.size();
    if (skySamples.size() < 16 || differences.size() < 8) {
        result.errorCode = "InsufficientValidSamples";
        result.message = "Too little complete pixel coverage for scientific noise estimation";
        return result;
    }
    result.sky = median(skySamples);
    result.noise = mad(differences, median(differences));
    if (o.measureStars) {
        // Detect on a bounded image, but measure shapes on native pixels. A
        // resized PSF multiplied by the resize factor includes sampling blur
        // and cannot serve as a native-width measurement.
        ImageBuffer smaller;
        const ImageBuffer* stellarImage = &image;
        if (image.pixelCount() > 1024 * 1024) {
            const auto targetWidth = std::max(16U, std::uint32_t(image.width *
                std::sqrt(double(1024 * 1024) / image.pixelCount())));
            auto resized = ImageResizer().resizeToWidth(image, targetWidth);
            if (resized.ok) {
                smaller = std::move(resized.image);
                stellarImage = &smaller;
            }
        }
        StarDetectionOptions starOptions;
        starOptions.minPeak = -std::numeric_limits<float>::max();
        starOptions.robustStatistics = true;
        starOptions.countAllDetections = true;
        const auto stars = StarDetector().detect(*stellarImage, starOptions);
        if (stars.ok) {
            result.starsMeasured = true;
            result.starCount = stars.detectedCount;
            const double scaleX = double(image.width) / stellarImage->width;
            const double scaleY = double(image.height) / stellarImage->height;
            const int search = std::min(8, int(std::ceil(std::max(scaleX, scaleY))));
            std::vector<double> widths, eccentricities;
            std::vector<std::pair<double, double>> centers;
            std::size_t attempts = 0;
            for (const auto& star : stars.stars) {
                if (++attempts > 512 || widths.size() >= 128) break;
                const int cx = int(std::round((star.x + .5) * scaleX - .5));
                const int cy = int(std::round((star.y + .5) * scaleY - .5));
                double peak = -std::numeric_limits<double>::infinity();
                int px = cx, py = cy;
                for (int y = std::max(0, cy - search); y <= std::min(int(image.height) - 1, cy + search); ++y)
                    for (int x = std::max(0, cx - search); x <= std::min(int(image.width) - 1, cx + search); ++x) {
                        const auto p = std::size_t(y) * image.width + x;
                        if (coverageAt(image, p) < .999999) continue;
                        const double value = luminanceAt(image, p);
                        if (std::isfinite(value) && value > peak) { peak = value; px = x; py = y; }
                    }
                const auto fit = fitStarCentroid(image, float(px), float(py));
                if (!fit.ok) continue;
                // Neighboring detections may converge to the same native star.
                if (std::any_of(centers.begin(), centers.end(), [&](const auto& p) {
                    return std::hypot(p.first - fit.x, p.second - fit.y) < 3;
                })) continue;
                centers.emplace_back(fit.x, fit.y);
                const double middle = .5 * (fit.covarianceXX + fit.covarianceYY);
                const double spread = std::hypot(.5 * (fit.covarianceXX - fit.covarianceYY), fit.covarianceXY);
                widths.push_back(2.354820045 * std::sqrt(middle));
                eccentricities.push_back(std::sqrt(std::clamp(2 * spread / (middle + spread), 0., 1.)));
            }
            result.starShapeCount = widths.size();
            if (widths.size() >= 5) {
                result.medianFwhm = median(std::move(widths));
                result.medianEccentricity = median(std::move(eccentricities));
            }
        }
    }
    result.ok = true;
    return result;
}

SequenceQualityResult SequenceQualityAnalyzer::assess(const std::vector<SequenceFrameSample>& frames,
                                                     const SequenceQualityOptions& o) const {
    SequenceQualityResult result;
    if (!validOptions(o) || frames.empty() ||
        (!o.overrides.empty() && o.overrides.size() != frames.size())) {
        result.errorCode = "ArgumentInvalid";
        result.message = "Invalid sequence quality options, empty sequence, or override count mismatch";
        return result;
    }
    result.frames.resize(frames.size());
    std::vector<double> noises, widths, eccentricities, starCounts;
    const SequenceFrameSample* grid = nullptr;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        auto& a = result.frames[i];
        a.usable = validSample(f);
        if (!a.usable) {
            a.reasons.push_back(f.errorCode.empty() ? "invalid-frame" : f.errorCode);
            continue;
        }
        if (!grid) grid = &f;
        if (f.width != grid->width || f.height != grid->height ||
            f.columns != grid->columns || f.rows != grid->rows) {
            result.errorCode = "AnalysisGridMismatch";
            result.message = "Temporal assessment requires the same registered grid for every frame";
            return result;
        }
        noises.push_back(f.noise);
        if (f.starsMeasured && f.starCount > 0) {
            if (f.medianFwhm > 0) {
                widths.push_back(f.medianFwhm);
                eccentricities.push_back(f.medianEccentricity);
            }
            starCounts.push_back(double(f.starCount));
        }
    }
    if (!grid) {
        result.errorCode = "NoUsableFrames";
        result.message = "No frame has usable scientific samples";
        return result;
    }
    result.medianNoise = median(noises);
    result.medianFwhm = median(widths);
    const double medianEccentricity = median(eccentricities), medianStars = median(starCounts);
    std::vector<std::size_t> positive(frames.size()), negative(frames.size());
    std::vector<double> squares(frames.size());
    std::vector<std::vector<std::size_t>> positivePoints(frames.size()), negativePoints(frames.size());
    std::vector<std::pair<double, std::size_t>> values;
    std::vector<double> pixelValues;
    values.reserve(frames.size()); pixelValues.reserve(frames.size());
    for (std::size_t p = 0; p < grid->luminance.size(); ++p) {
        values.clear(); pixelValues.clear();
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto& f = frames[i];
            if (!result.frames[i].usable || f.coverage[p] < .99 || !std::isfinite(f.luminance[p])) continue;
            const double value = double(f.luminance[p]) - f.sky;
            values.emplace_back(value, i); pixelValues.push_back(value);
        }
        if (values.size() <= o.minimumTemporalPeers) continue;
        std::sort(values.begin(), values.end());
        const double center = median(pixelValues);
        const double scatter = mad(pixelValues, center);
        const std::size_t peerCount = values.size() - 1;
        for (std::size_t rank = 0; rank < values.size(); ++rank) {
            const auto [value, i] = values[rank];
            const auto peerAt = [&](std::size_t k) { return values[k >= rank ? k + 1 : k].first; };
            const double peerMedian = peerCount % 2 ? peerAt(peerCount / 2)
                : (peerAt(peerCount / 2 - 1) + peerAt(peerCount / 2)) * .5;
            const double noiseFloor = std::hypot(frames[i].noise, result.medianNoise / std::sqrt(double(peerCount)));
            const double threshold = o.residualSigma * std::max({scatter, noiseFloor, 1.e-6}) +
                o.signalTolerance * std::abs(peerMedian);
            const double residual = value - peerMedian;
            auto& a = result.frames[i];
            ++a.comparedSamples;
            squares[i] += residual * residual;
            positive[i] += residual > threshold;
            negative[i] += residual < -threshold;
            if (o.measureTrails) {
                if (residual > threshold) positivePoints[i].push_back(p);
                if (residual < -threshold) negativePoints[i].push_back(p);
            }
        }
    }
    std::vector<double> residualFractions;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        auto& a = result.frames[i];
        a.temporalAvailable = a.comparedSamples >= 16;
        if (a.temporalAvailable) {
            a.positiveFraction = double(positive[i]) / a.comparedSamples;
            a.negativeFraction = double(negative[i]) / a.comparedSamples;
            a.residualRms = std::sqrt(squares[i] / a.comparedSamples);
            residualFractions.push_back(a.positiveFraction + a.negativeFraction);
        }
    }
    const double medianResidual = median(residualFractions);
    const double residualLimit = std::max(o.maximumResidualFraction,
        medianResidual + 4 * mad(residualFractions, medianResidual));
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        auto& a = result.frames[i];
        if (!a.usable) continue;
        if (o.measureTrails && a.temporalAvailable) {
            a.trails = detectResidualLines(positivePoints[i], f, 1);
            const auto low = detectResidualLines(negativePoints[i], f, -1);
            a.trails.insert(a.trails.end(), low.begin(), low.end());
            if (!a.trails.empty()) a.warnings.push_back("signed-linear-transient-candidate");
        }
        a.noiseRatio = f.noise / std::max(result.medianNoise, 1.e-6);
        a.fwhmRatio = result.medianFwhm > 0 ? f.medianFwhm / result.medianFwhm : 0;
        if (f.validFraction < o.minimumCoverage) a.reasons.push_back("low-coverage");
        if (a.noiseRatio > o.maximumNoiseRatio) a.reasons.push_back("high-noise");
        if (a.fwhmRatio > o.maximumFwhmRatio) a.reasons.push_back("broad-stars");
        if (f.medianEccentricity > o.maximumEccentricity && f.medianEccentricity > medianEccentricity + .15)
            a.reasons.push_back("elongated-stars");
        if (f.starsMeasured && medianStars >= 20 && f.starCount < medianStars * o.minimumStarRatio)
            a.reasons.push_back("few-stars");
        if (!a.temporalAvailable) a.warnings.push_back("insufficient-temporal-peers");
        else if (a.positiveFraction + a.negativeFraction > residualLimit)
            a.reasons.push_back("broad-temporal-residuals");
        else if (positive[i] + negative[i] > 0)
            a.warnings.push_back("localized-transients-use-pixel-rejection");
        if (o.measureStars && !f.starsMeasured) a.warnings.push_back("stellar-measurements-unavailable");
        else if (f.starCount > 0 && f.medianFwhm <= 0) a.warnings.push_back("stellar-shapes-unavailable");
        a.recommendedKeep = a.reasons.empty();
        a.selected = !o.applySelection || a.recommendedKeep;
        const auto override = o.overrides.empty() ? FrameSelectionOverride::Automatic : o.overrides[i];
        if (override == FrameSelectionOverride::Keep) a.selected = true;
        if (override == FrameSelectionOverride::Reject) {
            a.selected = false;
            a.reasons.push_back("user-excluded");
        }
        result.selectedCount += a.selected;
    }
    if (o.applySelection && (result.selectedCount < o.minimumKeptFrames ||
        double(result.selectedCount) / frames.size() < o.minimumKeptFraction)) {
        result.errorCode = "SelectionNeedsReview";
        result.message = "Selection would retain too few frames; review the report and thresholds before stacking";
        return result;
    }
    result.ok = true;
    return result;
}
} // namespace photonstack
