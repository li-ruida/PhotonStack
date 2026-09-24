#include "photonstack/FrequencyCoadd.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>
#include "vendor/pocketfft_hdronly.h"

namespace photonstack {
namespace {
bool finite(std::complex<double> value) {
    return std::isfinite(value.real()) && std::isfinite(value.imag());
}
double frequency(std::size_t y, std::size_t size) {
    return y < (size + 1) / 2 ? double(y) / size : (double(y) - double(size)) / size;
}
void checkFinite(std::span<const double> values) {
    if (!std::all_of(values.begin(), values.end(), [](double x) { return std::isfinite(x); }))
        throw std::invalid_argument("Frequency input samples must be finite");
}
} // namespace

FrequencyCoadd::FrequencyCoadd(std::size_t width, std::size_t height, std::size_t threads)
    : width_(width), height_(height), threads_(threads) {
    // Bound allocations and arithmetic before allocating any spectrum. This
    // includes the current 2304 x 4096 M31 grid and small non-power-of-two tests.
    constexpr std::size_t maxPixels = 16 * 1024 * 1024;
    if (!width || !height || width > 16384 || height > 16384 ||
        width > maxPixels / height || threads == 0 || threads > 64)
        throw std::invalid_argument("Frequency grid or thread count exceeds supported bounds");
    numerator_.resize(height * (width / 2 + 1));
    information_.resize(numerator_.size());
}

std::vector<std::complex<double>> FrequencyCoadd::forward(std::span<const double> image) const {
    std::vector<std::complex<double>> result(numerator_.size());
    const pocketfft::shape_t shape{height_, width_}, axes{0, 1};
    const pocketfft::stride_t realStride{std::ptrdiff_t(width_ * sizeof(double)), sizeof(double)};
    const pocketfft::stride_t complexStride{
        std::ptrdiff_t((width_ / 2 + 1) * sizeof(std::complex<double>)), sizeof(std::complex<double>)};
    pocketfft::r2c(shape, realStride, complexStride, axes, true, image.data(), result.data(), 1.0, threads_);
    return result;
}

std::vector<double> FrequencyCoadd::inverse(std::span<const std::complex<double>> spectrum) const {
    std::vector<double> result(width_ * height_);
    const pocketfft::shape_t shape{height_, width_}, axes{0, 1};
    const pocketfft::stride_t realStride{std::ptrdiff_t(width_ * sizeof(double)), sizeof(double)};
    const pocketfft::stride_t complexStride{
        std::ptrdiff_t((width_ / 2 + 1) * sizeof(std::complex<double>)), sizeof(std::complex<double>)};
    pocketfft::c2r(shape, complexStride, realStride, axes, false, spectrum.data(), result.data(),
                   1.0 / double(width_ * height_), threads_);
    if (!std::all_of(result.begin(), result.end(), [](double x) { return std::isfinite(x); }))
        throw std::overflow_error("Frequency reconstruction exceeds double precision range");
    return result;
}

void FrequencyCoadd::validateSpectrum(std::span<const std::complex<double>> spectrum) const {
    if (spectrum.size() != numerator_.size() ||
        !std::all_of(spectrum.begin(), spectrum.end(), finite))
        throw std::invalid_argument("Frequency spectrum has invalid dimensions or values");
    const auto stride = width_ / 2 + 1;
    for (std::size_t y = 0; y < height_; ++y) {
        for (auto x : {std::size_t(0), width_ / 2}) {
            if (x != 0 && width_ % 2) continue;
            const auto a = spectrum[y * stride + x];
            const auto b = std::conj(spectrum[((height_ - y) % height_) * stride + x]);
            if (std::abs(a - b) > 1e-10 * (1 + std::max(std::abs(a), std::abs(b))))
                throw std::invalid_argument("Frequency spectrum violates real-image boundary symmetry");
        }
    }
}

void FrequencyCoadd::validatePower(std::span<const double> power) const {
    if (power.size() != numerator_.size() ||
        !std::all_of(power.begin(), power.end(), [](double x) { return std::isfinite(x) && x > 0; }))
        throw std::invalid_argument("Frequency noise power must be positive, finite and grid-matched");
    const auto stride = width_ / 2 + 1;
    for (std::size_t y = 0; y < height_; ++y) {
        for (auto x : {std::size_t(0), width_ / 2}) {
            if (x != 0 && width_ % 2) continue;
            const auto a = power[y * stride + x], b = power[((height_ - y) % height_) * stride + x];
            if (std::abs(a - b) > 1e-10 * std::max(a, b))
                throw std::invalid_argument("Noise power violates real-image boundary symmetry");
        }
    }
}

void FrequencyCoadd::add(std::span<const double> image,
                        std::span<const std::complex<double>> transfer,
                        double flux, double variance, std::span<const double> noisePower) {
    if (image.size() != width_ * height_ || !std::isfinite(flux) || flux <= 0 ||
        !std::isfinite(variance) || variance <= 0 || count_ == std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("Frequency add requires a matching image and positive finite flux/variance");
    checkFinite(image);
    validateSpectrum(transfer);
    validatePower(noisePower);
    auto updatedNumerator = forward(image);
    std::vector<double> updatedInformation(information_.size());
    for (std::size_t i = 0; i < updatedNumerator.size(); ++i) {
        const double precision = 1 / (variance * noisePower[i]);
        const auto contribution = flux * std::conj(transfer[i]) * updatedNumerator[i] * precision;
        const double info = flux * flux * std::norm(transfer[i]) * precision;
        updatedNumerator[i] = numerator_[i] + contribution;
        updatedInformation[i] = information_[i] + info;
        if (!std::isfinite(precision) || precision <= 0 || !finite(updatedNumerator[i]) ||
            !std::isfinite(updatedInformation[i]) || updatedInformation[i] < 0)
            throw std::overflow_error("Frequency accumulation exceeds double precision range");
    }
    numerator_.swap(updatedNumerator);
    information_.swap(updatedInformation);
    ++count_;
}

void FrequencyCoadd::merge(const FrequencyCoadd& other) {
    if (width_ != other.width_ || height_ != other.height_ ||
        count_ > std::numeric_limits<std::size_t>::max() - other.count_)
        throw std::invalid_argument("Frequency accumulators must have matching grids and bounded counts");
    auto numerator = numerator_;
    auto information = information_;
    for (std::size_t i = 0; i < numerator.size(); ++i) {
        numerator[i] += other.numerator_[i];
        information[i] += other.information_[i];
        if (!finite(numerator[i]) || !std::isfinite(information[i]))
            throw std::overflow_error("Merged frequency state exceeds double precision range");
    }
    numerator_.swap(numerator);
    information_.swap(information);
    count_ += other.count_;
}

std::vector<double> FrequencyCoadd::render(std::span<const std::complex<double>> target) const {
    if (!count_) throw std::invalid_argument("Cannot render an empty frequency accumulator");
    validateSpectrum(target);
    if (std::abs(target[0] - std::complex<double>(1, 0)) > 1e-9)
        throw std::invalid_argument("Output response must preserve unit DC");
    std::vector<std::complex<double>> spectrum(numerator_.size());
    for (std::size_t i = 0; i < spectrum.size(); ++i) {
        if (information_[i] == 0) {
            if (target[i] != std::complex<double>{})
                throw std::domain_error("Output target requests an unsupported frequency");
        } else {
            spectrum[i] = numerator_[i] * target[i] / information_[i];
            if (!finite(spectrum[i])) throw std::overflow_error("Output frequency exceeds double precision range");
        }
    }
    return inverse(spectrum);
}

std::vector<std::complex<double>> FrequencyCoadd::noiseMatchedResponse(
    std::span<const double> outputNoisePower) const {
    if (!count_ || information_[0] <= 0) throw std::invalid_argument("No frequency DC information available");
    validatePower(outputNoisePower);
    const double normalization = std::sqrt(information_[0]) * std::sqrt(outputNoisePower[0]);
    if (!std::isfinite(normalization) || normalization <= 0)
        throw std::overflow_error("Output response normalization is outside double range");
    std::vector<std::complex<double>> result(information_.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        const double value = std::sqrt(information_[i]) * std::sqrt(outputNoisePower[i]) / normalization;
        if (!std::isfinite(value)) throw std::overflow_error("Output response exceeds double range");
        result[i] = value;
    }
    return result;
}

std::vector<std::complex<double>> FrequencyCoadd::psfTransfer(
    std::span<const double> kernel, std::size_t kw, std::size_t kh, double cx, double cy) const {
    if (!kw || !kh || kw > width_ || kh > height_ || !(kw % 2) || !(kh % 2) ||
        kernel.size() != kw * kh || !std::isfinite(cx) || !std::isfinite(cy) ||
        std::max(std::abs(cx), std::abs(cy)) > std::min(kw, kh) / 4.0)
        throw std::invalid_argument("PSF needs an odd matching kernel and a finite small centroid");
    checkFinite(kernel);
    double sum = 0;
    for (double value : kernel) sum += value;
    if (!std::isfinite(sum) || sum <= 0) throw std::invalid_argument("PSF must have positive finite total flux");
    std::vector<double> spatial(width_ * height_);
    for (std::size_t y = 0; y < kh; ++y)
        for (std::size_t x = 0; x < kw; ++x)
            spatial[((y + height_ - kh / 2) % height_) * width_ + (x + width_ - kw / 2) % width_] =
                kernel[y * kw + x] / sum;
    auto transfer = forward(spatial);
    if (cx != 0 || cy != 0) {
        const auto stride = width_ / 2 + 1;
        for (std::size_t y = 0; y < height_; ++y)
            for (std::size_t x = 0; x < stride; ++x) {
                const double phase = -2 * std::numbers::pi * (double(x) / width_ * cx + frequency(y, height_) * cy);
                transfer[y * stride + x] *= std::complex<double>(std::cos(phase), std::sin(phase));
            }
        // Match the real-image Nyquist projection, not a complex shifted image.
        transfer = forward(inverse(transfer));
    }
    validateSpectrum(transfer);
    return transfer;
}

FrequencyNoisePower FrequencyCoadd::noisePower(std::span<const NoiseCorrelation> correlations) const {
    if (correlations.size() > 256) throw std::invalid_argument("Too many stationary noise correlations");
    std::set<std::pair<int, int>> seen;
    for (const auto& c : correlations) {
        if (!std::isfinite(c.correlation) || std::abs(c.correlation) > 1 ||
            std::abs(double(c.dx)) > 64 || std::abs(double(c.dy)) > 64 || (c.dx == 0 && c.dy == 0))
            throw std::invalid_argument("Noise correlations require finite coefficients and nonzero bounded displacements");
        const auto lag = c.dx < 0 || (c.dx == 0 && c.dy < 0) ? std::pair{-c.dx, -c.dy} : std::pair{c.dx, c.dy};
        if (!seen.insert(lag).second) throw std::invalid_argument("Duplicate noise displacement or its negation");
    }
    FrequencyNoisePower result;
    result.values.assign(information_.size(), 1);
    const auto stride = width_ / 2 + 1;
    std::vector<double> cosx(stride), sinx(stride);
    for (const auto& c : correlations) {
        for (std::size_t x = 0; x < stride; ++x) {
            const double angle = 2 * std::numbers::pi * c.dx * double(x) / width_;
            cosx[x] = std::cos(angle); sinx[x] = std::sin(angle);
        }
        for (std::size_t y = 0; y < height_; ++y) {
            const double angle = 2 * std::numbers::pi * c.dy * frequency(y, height_);
            const double cosine = std::cos(angle), sine = std::sin(angle);
            for (std::size_t x = 0; x < stride; ++x)
                result.values[y * stride + x] += 2 * c.correlation * (cosine * cosx[x] - sine * sinx[x]);
        }
    }
    for (auto& value : result.values) {
        if (value < .05) { value = .05; ++result.flooredBins; }
    }
    validatePower(result.values);
    return result;
}

} // namespace photonstack
