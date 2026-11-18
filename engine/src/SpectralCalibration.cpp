#include "photonstack/SpectralCalibration.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
namespace photonstack {
namespace {
void validate(const SpectralCurve& c, bool response) {
    if (c.wavelength.size() < 2 || c.wavelength.size() > 100000 || c.value.size() != c.wavelength.size())
        throw std::invalid_argument("Invalid spectral curve size");
    for (std::size_t i = 0; i < c.value.size(); ++i)
        if (!std::isfinite(c.wavelength[i]) || c.wavelength[i] <= 0 || c.wavelength[i] > 1e6 ||
            !std::isfinite(c.value[i]) || (response && (c.value[i] < 0 || c.value[i] > 1)) ||
            (i && c.wavelength[i] <= c.wavelength[i - 1]))
            throw std::invalid_argument("Spectral curves require finite increasing nm wavelengths and 0..1 response");
}
double interpolate(const SpectralCurve& c, double x) {
    auto upper = std::upper_bound(c.wavelength.begin(), c.wavelength.end(), x);
    const auto i = std::clamp<std::size_t>(upper - c.wavelength.begin(), 1, c.value.size() - 1) - 1;
    const double t = (x - c.wavelength[i]) / (c.wavelength[i + 1] - c.wavelength[i]);
    return c.value[i] * (1 - t) + c.value[i + 1] * t;
}
double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    const double p = q * (v.size() - 1);
    const auto i = std::size_t(p);
    return v[i] + (p - i) * (v[std::min(i + 1, v.size() - 1)] - v[i]);
}
double mad(const std::vector<double>& v, double center) {
    std::vector<double> d;
    for (double x : v)
        d.push_back(std::abs(x - center));
    return 1.4826 * quantile(d, .5);
}
struct Sum {
    double value = 0, error = 0;
    void add(double x) {
        const double y = x - error, t = value + y;
        error = (t - value) - y;
        value = t;
    }
};
} // namespace
SyntheticFlux integratePhotonSpectrum(const SampledSpectrum& spectrum, const SpectralBand& band) {
    validate(spectrum.flux, false);
    if (band.empty() || band.size() > 8)
        throw std::invalid_argument("Specify 1..8 response components");
    SpectralCurve errors{spectrum.flux.wavelength, spectrum.error};
    if (!spectrum.error.empty()) {
        validate(errors, false);
        for (double e : spectrum.error)
            if (e < 0)
                throw std::invalid_argument("Negative spectral uncertainty");
    }
    double low = 0, high = 1e6;
    for (const auto& response : band) {
        validate(response, true);
        low = std::max(low, response.wavelength.front());
        high = std::min(high, response.wavelength.back());
    }
    if (!(low < high) || low < spectrum.flux.wavelength.front() || high > spectrum.flux.wavelength.back())
        throw std::invalid_argument(
            "Spectrum does not cover the complete response support; no spectral extrapolation is allowed");
    std::vector<double> knots{low, high};
    auto append = [&](const SpectralCurve& c) {
        for (double x : c.wavelength)
            if (x > low && x < high)
                knots.push_back(x);
    };
    append(spectrum.flux);
    for (const auto& c : band)
        append(c);
    std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
    // Eight-point Gauss-Legendre is exact for the product of up to eight linear
    // response components, linear F_lambda and the photon wavelength factor on
    // every union-knot interval. It also exactly integrates response moments.
    constexpr std::array<double, 4> x{.18343464249564980494, .52553240991632898582, .79666647741362673959,
                                      .96028985649753623168};
    constexpr std::array<double, 4> weight{.36268378337836198297, .31370664587788728734, .22238103445337447054,
                                           .10122853629037625915};
    Sum flux, uncertainty, area, first, second;
    for (std::size_t i = 1; i < knots.size(); ++i) {
        const double mid = (knots[i] + knots[i - 1]) / 2, half = (knots[i] - knots[i - 1]) / 2;
        for (unsigned j = 0; j < 4; ++j)
            for (double sign : {-1., 1.}) {
                const double lambda = mid + sign * half * x[j];
                double response = 1;
                for (const auto& c : band)
                    response *= interpolate(c, lambda);
                const double factor = half * weight[j] * response;
                area.add(factor);
                first.add(factor * lambda);
                second.add(factor * lambda * lambda);
                flux.add(factor * lambda * interpolate(spectrum.flux, lambda));
                if (!spectrum.error.empty())
                    uncertainty.add(factor * lambda * interpolate(errors, lambda));
            }
    }
    if (!(area.value > 0) || !std::isfinite(flux.value) || !std::isfinite(uncertainty.value))
        throw std::runtime_error("Empty or nonfinite synthetic band");
    SyntheticFlux result;
    result.value = flux.value;
    result.errorBound = uncertainty.value;
    result.hasErrors = !spectrum.error.empty();
    result.meanWavelength = first.value / area.value;
    result.rmsBandwidth =
        std::sqrt(std::max(0., second.value / area.value - result.meanWavelength * result.meanWavelength));
    return result;
}
SpectralColorFit fitSpectralColors(const std::vector<SpectralColorSample>& input, const std::array<double, 3>& white,
                                   unsigned width, unsigned height, double limit) {
    if (input.size() > 10000 || width < 16 || height < 16 || !std::isfinite(limit) || limit <= 0 || limit > 1)
        throw std::invalid_argument("Invalid color-fit geometry or threshold");
    for (double f : white)
        if (!std::isfinite(f) || f <= 0)
            throw std::invalid_argument("White reference must have positive finite synthetic flux in all channels");
    auto stars = input;
    std::set<std::string> ids;
    for (const auto& s : stars) {
        if (s.id.empty() || !ids.insert(s.id).second || !std::isfinite(s.x) || !std::isfinite(s.y) || s.x < 0 ||
            s.x >= width || s.y < 0 || s.y >= height)
            throw std::invalid_argument("Invalid or duplicated color reference star");
        for (unsigned c = 0; c < 3; ++c)
            if (!std::isfinite(s.observed[c]) || !std::isfinite(s.predicted[c]) || s.observed[c] <= 0 ||
                s.predicted[c] <= 0)
                throw std::invalid_argument("Synthetic and measured stellar fluxes must be positive");
    }
    std::sort(stars.begin(), stars.end(), [](const auto& a, const auto& b) {
        if (a.y != b.y)
            return a.y < b.y;
        if (a.x != b.x)
            return a.x < b.x;
        return a.id < b.id;
    });
    SpectralColorFit result;
    for (unsigned c = 0; c < 3; ++c) {
        result.whiteRatios[c] = white[c] / white[1];
        if (!std::isfinite(result.whiteRatios[c]) || result.whiteRatios[c] <= 0)
            throw std::invalid_argument("White reference color ratio is not representable");
    }
    std::array<std::vector<double>, 3> training;
    for (std::size_t i = 0; i < stars.size(); ++i) {
        const auto& s = stars[i];
        SpectralColorResidual record;
        record.id = s.id;
        record.x = s.x;
        record.y = s.y;
        record.heldOut = i % 5 == 4;
        record.status = record.heldOut ? "validation" : "fit";
        for (unsigned c = 0; c < 3; ++c) {
            record.logGain[c] =
                std::log(s.predicted[c]) - std::log(s.predicted[1]) - std::log(s.observed[c]) + std::log(s.observed[1]);
            if (!record.heldOut)
                training[c].push_back(record.logGain[c]);
        }
        result.stars.push_back(record);
    }
    if (training[0].size() < 12 || stars.size() - training[0].size() < 6) {
        result.reason = "At least 12 fitting and 6 held-out stars are required";
        return result;
    }
    std::array<double, 3> center{}, scatter{};
    for (unsigned c = 0; c < 3; ++c) {
        center[c] = quantile(training[c], .5);
        scatter[c] = mad(training[c], center[c]);
    }
    std::array<std::vector<double>, 3> accepted;
    for (auto& s : result.stars)
        if (!s.heldOut) {
            bool use = true;
            for (unsigned c : {0U, 2U})
                if (std::abs(s.logGain[c] - center[c]) > 4.685 * std::max(.005, scatter[c]))
                    use = false;
            if (use) {
                ++result.training;
                for (unsigned c = 0; c < 3; ++c)
                    accepted[c].push_back(s.logGain[c]);
            } else {
                ++result.rejected;
                s.status = "fit-outlier";
            }
        }
    if (result.training < 12) {
        result.reason = "Too few robust fitting stars";
        return result;
    }
    for (unsigned c = 0; c < 3; ++c) {
        center[c] = quantile(accepted[c], .5);
        result.trainingScatter[c] = mad(accepted[c], center[c]);
        if (std::abs(center[c]) > 700) {
            result.reason = "Instrument color ratio is not representable";
            return result;
        }
        result.instrumentGains[c] = std::exp(center[c]);
        result.gains[c] = result.instrumentGains[c] / result.whiteRatios[c];
    }
    std::set<unsigned> regions;
    std::array<std::vector<double>, 3> validation;
    for (auto& s : result.stars) {
        for (unsigned c = 0; c < 3; ++c) {
            s.residual[c] = center[c] - s.logGain[c];
            if (s.heldOut)
                validation[c].push_back(s.residual[c]);
        }
        if (s.heldOut) {
            ++result.validation;
            regions.insert(std::min(3u, unsigned(4 * s.y / height)) * 4 + std::min(3u, unsigned(4 * s.x / width)));
        }
    }
    result.validationRegions = unsigned(regions.size());
    for (unsigned c = 0; c < 3; ++c) {
        result.validationMedian[c] = quantile(validation[c], .5);
        for (auto& v : validation[c])
            v = std::abs(v);
        result.validationP90[c] = quantile(validation[c], .9);
    }
    for (double g : result.gains)
        if (!std::isfinite(g) || g < .05 || g > 20) {
            result.reason = "White-normalized channel gain is outside 0.05..20";
            return result;
        }
    if (result.validationRegions < 4) {
        result.reason = "Held-out stars must cover at least four image regions";
        return result;
    }
    if (std::max(result.validationP90[0], result.validationP90[2]) > limit) {
        result.reason =
            "Held-out colors do not support the selected spectral response and a single multiplicative calibration";
        return result;
    }
    result.valid = true;
    return result;
}
} // namespace photonstack
