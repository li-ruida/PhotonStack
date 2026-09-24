#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace photonstack {

struct NoiseCorrelation {
    int dx = 0;
    int dy = 0;
    double correlation = 0;
};

struct FrequencyNoisePower {
    std::vector<double> values;
    std::size_t flooredBins = 0;
};

// Experimental stationary-noise, known-PSF linear coaddition. All image planes
// must already be registered, fully covered, background-subtracted and padded
// on the same grid. Values retain their scientific units, including negatives.
// Half spectra use row-major height x (width/2+1), as in an ordinary real FFT.
// Model construction, registration, rejection and saturation guards are separate.
// Invalid input throws std::invalid_argument; numerical failure throws
// std::overflow_error/domain_error. A failed add/merge never changes the state.
class FrequencyCoadd {
public:
    FrequencyCoadd(std::size_t width, std::size_t height, std::size_t threads = 1);
    [[nodiscard]] std::size_t width() const { return width_; }
    [[nodiscard]] std::size_t height() const { return height_; }
    [[nodiscard]] std::size_t frameCount() const { return count_; }
    [[nodiscard]] const std::vector<std::complex<double>>& numerator() const { return numerator_; }
    [[nodiscard]] const std::vector<double>& information() const { return information_; }

    void add(std::span<const double> image,
             std::span<const std::complex<double>> transfer,
             double flux, double variance, std::span<const double> noisePower);
    void merge(const FrequencyCoadd& other);

    // Unit-DC output PSF. Nonzero target frequencies require positive information.
    [[nodiscard]] std::vector<double> render(std::span<const std::complex<double>> target) const;
    [[nodiscard]] std::vector<std::complex<double>> noiseMatchedResponse(
        std::span<const double> outputNoisePower) const;

    // Odd empirical kernel, centered at floor(size/2), normalized to unit sum.
    // Fractional shifts explicitly project Nyquist bins onto a real spatial PSF.
    [[nodiscard]] std::vector<std::complex<double>> psfTransfer(
        std::span<const double> kernel, std::size_t kernelWidth,
        std::size_t kernelHeight, double centroidX = 0, double centroidY = 0) const;
    // Each displacement also represents its negative; duplicates are rejected.
    [[nodiscard]] FrequencyNoisePower noisePower(std::span<const NoiseCorrelation> correlations) const;

private:
    std::size_t width_, height_, threads_, count_ = 0;
    std::vector<std::complex<double>> numerator_;
    std::vector<double> information_;
    [[nodiscard]] std::vector<std::complex<double>> forward(std::span<const double> image) const;
    [[nodiscard]] std::vector<double> inverse(std::span<const std::complex<double>> spectrum) const;
    void validateSpectrum(std::span<const std::complex<double>> spectrum) const;
    void validatePower(std::span<const double> power) const;
};

} // namespace photonstack
