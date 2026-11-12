#pragma once
#include "photonstack/Calibrator.hpp"
#include "photonstack/Registration.hpp"
#include <span>
namespace photonstack::detail {
// Full-frame, finite, linear RGBA similarity registration.
bool metalWarp(const ImageBuffer& image, SimilarityTransform transform, RegistrationInterpolation interpolation,
               ImageBuffer& output);
bool metalAffine(const ImageBuffer&, AffineTransform, RegistrationInterpolation, ImageBuffer&);
bool metalMappedWarp(const ImageBuffer&, std::span<const float>, RegistrationInterpolation, ImageBuffer&);
bool metalScientificCFA(const std::vector<float>& mosaic, unsigned width, unsigned height,
                        const std::array<int,4>& colors, const std::array<float,3>& gains,
                        std::vector<float>& samples);
bool metalGuardLuminance(const ImageBuffer&, const std::array<float, 3>& gains, std::vector<float>& luminance);
bool metalGuardSky(std::span<const float> mosaic, unsigned width, const std::array<int, 4>& colors,
                   std::span<const float> apertures, std::vector<float>& sky);
bool metalPeakMask(std::span<const float> luminance, float threshold, std::vector<float>& mask);
bool metalCalibrate(const ImageBuffer&, const CalibrationOptions&, bool lightBias, bool flatBias,
                    const std::vector<double>& means, ImageBuffer&);
bool metalDemosaic(const std::vector<float>& mosaic, const std::array<int, 4>& colors, ImageBuffer& result,
                   bool guarded);
bool metalWinsorCenters(const std::vector<float>& tile, std::size_t pixels, std::size_t frames,
                        std::span<const float> weights, std::vector<float>& centers);
bool metalWinsorReduce(const std::vector<float>& tile, std::size_t pixels, std::size_t frames,
                       std::span<const float> weights, const std::vector<float>& centers, float low, float high,
                       bool counts, std::vector<float>& reduced);
} // namespace photonstack::detail
