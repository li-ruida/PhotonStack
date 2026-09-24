#include "photonstack/Registration.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace photonstack;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
ImageBuffer field(unsigned seed, bool mirror, SimilarityTransform transform = {}) {
    ImageBuffer image;
    image.width = 640; image.height = 480; image.channels = 4;
    image.format = PixelFormat::Float32RGBA; image.colorEncoding = ColorEncoding::Linear;
    image.pixels.assign(image.sampleCount(), 0);
    for (std::size_t p = 0; p < image.pixelCount(); ++p) image.pixels[p * 4 + 3] = 1;
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> xpos(35, 605), ypos(35, 445), amplitude(60, 140);
    for (int s = 0; s < 100; ++s) {
        double x = xpos(random), y = ypos(random), peak = amplitude(random);
        if (mirror) y = 479 - y;
        const double px = transform.scale * (std::cos(transform.rotationRadians) * x - std::sin(transform.rotationRadians) * y) + transform.dx;
        const double py = transform.scale * (std::sin(transform.rotationRadians) * x + std::cos(transform.rotationRadians) * y) + transform.dy;
        for (int iy = int(py) - 6; iy <= int(py) + 6; ++iy)
            for (int ix = int(px) - 6; ix <= int(px) + 6; ++ix) {
                if (ix < 0 || iy < 0 || ix >= 640 || iy >= 480) continue;
                const float v = float(peak * std::exp(-((ix - px) * (ix - px) + (iy - py) * (iy - py)) / 2.88));
                for (int c = 0; c < 3; ++c) image.pixels[(iy * 640 + ix) * 4 + c] += v;
            }
    }
    return image;
}
}
int main() {
    RegistrationOptions options;
    options.minimumMatches = 30; options.matchTolerance = 1;
    options.similarityFallbackToTranslation = false;
    options.starDetection = {.sigmaThreshold = 4, .minPeak = 0, .border = 12,
                             .maxStars = 200, .robustStatistics = true};
    for (unsigned seed : {192U, 492U, 921U}) {
        const auto moving = field(seed, false);
        const SimilarityTransform truth{.scale = 1.004F, .rotationRadians = .012F, .dx = 2.3F, .dy = -3.1F};
        const auto reference = field(seed, false, truth);
        const auto result = Registration().estimateSimilarity(reference, moving, options);
        require(result.ok && !result.usedFallback, "valid similarity rejected");
        require(std::abs(result.transform.scale - truth.scale) < .001, "incorrect scale accepted");
        require(std::abs(result.transform.rotationRadians - truth.rotationRadians) < .001, "incorrect rotation accepted");
        require(std::hypot(result.transform.dx - truth.dx, result.transform.dy - truth.dy) < .2, "incorrect shift accepted");
        for (bool reflected : {false, true}) {
            const auto wrong = field(reflected ? seed : seed + 73, reflected);
            const auto rejected = Registration().estimateSimilarity(reference, wrong, options);
            std::cout << seed << (reflected ? " mirrored " : " unrelated ") << rejected.ok
                      << " matches " << rejected.matches << " scale " << rejected.transform.scale << '\n';
#ifndef RECORD_BASELINE
            require(!rejected.ok, "unrelated or mirrored star field accepted as similarity");
            auto withFallback = options;
            withFallback.similarityFallbackToTranslation = true;
            const auto fallback = Registration().estimateSimilarity(reference, wrong, withFallback);
            require(!fallback.ok, "poor fit escaped acceptance through sparse translation fallback");
#endif
        }
    }
}
