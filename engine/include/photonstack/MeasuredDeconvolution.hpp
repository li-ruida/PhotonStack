#pragma once
#include <cstddef>
#include <vector>
#include "photonstack/ImageBuffer.hpp"

namespace photonstack {
struct SpatialPsfNode {
    double x = 0, y = 0; // normalized field coordinates in [0,1]
    ImageBuffer kernel;
};
enum class PsfNoiseModel { Constant, Poisson, PoissonGaussian };
struct MeasuredDeconvolutionOptions {
    // Four quadrant-center nodes, row-major. Empty retains the global kernel.
    std::vector<SpatialPsfNode> spatialNodes;
    PsfNoiseModel noiseModel = PsfNoiseModel::Constant;
    double electronsPerUnit = 1, readNoiseElectrons = 0;
    unsigned iterations = 8;
    unsigned threads = 4;
    double amount = 1;
    double pedestal = 0;       // Explicit counts added for RL, removed on output.
    double noiseSigma = 0;     // Residual damping scale in source pixel units.
    double protectAbove = 0;   // 0 disables bright-core protection.
};
struct DeconvolutionChannelMetrics {
    double beforeRms = 0, afterRms = 0, beforeFlux = 0, afterFlux = 0;
};
struct MeasuredDeconvolutionResult {
    ImageBuffer image, difference, residual, protection;
    std::vector<DeconvolutionChannelMetrics> channels;
    std::size_t coveredPixels = 0, protectedPixels = 0;
    unsigned fftWidth = 0, fftHeight = 0;
};
// Linear finite-domain, coverage-normalized convolution with its exact adjoint.
// Nonnegative RL operates on source + explicit pedestal; no display clipping.
// A single kernel is applied independently to each input color channel.
MeasuredDeconvolutionResult restoreMeasuredPsf(const ImageBuffer& source,
    const ImageBuffer& kernel, const MeasuredDeconvolutionOptions& options = {});
}
