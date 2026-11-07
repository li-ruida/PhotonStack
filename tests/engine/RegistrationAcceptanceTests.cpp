#include "photonstack/Registration.hpp"
#include <algorithm>
#include <cmath>
#include <future>
#include <limits>
#include <iostream>
#include <random>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

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
void requireSameRegistration(const RegistrationResult& expected, const RegistrationResult& actual) {
    const auto fields = [](const RegistrationResult& r) {
        return std::tie(r.ok, r.usedFallback, r.usedCentroidRefinement, r.usedCentroidAffine, r.matches,
                        r.detectedReferenceStars, r.detectedMovingStars, r.inlierRatio, r.centroidRefinementRms,
                        r.centroidAffineValidationRatio, r.centroidRefinementPairs, r.errorCode, r.message,
                        r.translation.dx, r.translation.dy, r.transform.scale, r.transform.rotationRadians,
                        r.transform.dx, r.transform.dy, r.affine.a, r.affine.b, r.affine.c, r.affine.d,
                        r.affine.dx, r.affine.dy);
    };
    require(fields(expected) == fields(actual), "prepared reference changed a registration result");
}

void testPreparedReference() {
    const Registration registration;
    const auto moving = field(98, false);
    const auto reference = field(98, false, {1.003F, .008F, .98F, -.52F});
    RegistrationOptions options;
    options.starDetection.maxStars = 40;
    options.minimumMatches = 12;
    options.refineSimilarityCentroids = true;
    const auto translation = registration.estimateTranslation(reference, moving, options);
    const auto similarity = registration.estimateSimilarity(reference, moving, options);
    const auto affine = registration.estimateAffine(reference, moving, options);
    DistortionTransform dense;
    const auto distortion = registration.estimateDistortion(reference, moving, dense, options);
    require(translation.ok && affine.ok && similarity.ok && distortion.ok, "fixture has sufficient registration stars");
    require(translation.detectedReferenceStars == 40 && distortion.detectedReferenceStars > 40,
            "fixture exercises distinct coarse and local catalogue limits");
    for (bool prepareDense : {false, true}) {
        auto source = reference;
        const auto* pixels = source.pixels.data();
        auto localOptions = options;
        const auto prepared = registration.prepareReference(std::move(source), localOptions, prepareDense);
        localOptions.starDetection.maxStars = 4;
        require(prepared.image().pixels.data() == pixels, "reference pixels were copied during preparation");
        auto inParallel = std::async(std::launch::async, [&] { return registration.estimateAffine(prepared, moving); });
        requireSameRegistration(translation, registration.estimateTranslation(prepared, moving));
        requireSameRegistration(similarity, registration.estimateSimilarity(prepared, moving));
        requireSameRegistration(affine, inParallel.get());
        DistortionTransform actual;
        requireSameRegistration(distortion, registration.estimateDistortion(prepared, moving, actual));
        const auto distortionFields = [](const DistortionTransform& d) {
            return std::tie(d.global.scale, d.global.rotationRadians, d.global.dx, d.global.dy, d.useProjective,
                            d.projective.h00, d.projective.h01, d.projective.h02, d.projective.h10, d.projective.h11,
                            d.projective.h12, d.projective.h20, d.projective.h21, d.usePolynomial, d.polynomialDx,
                            d.polynomialDy, d.influenceRadius);
        };
        require(distortionFields(actual) == distortionFields(dense) && actual.controlPoints.size() == dense.controlPoints.size(),
                "prepared reference changed the distortion baseline");
        for (std::size_t i = 0; i < dense.controlPoints.size(); ++i) {
            const auto& a = dense.controlPoints[i];
            const auto& b = actual.controlPoints[i];
            require(std::tie(a.x, a.y, a.dx, a.dy) == std::tie(b.x, b.y, b.dx, b.dy),
                    "prepared reference changed local distortion controls");
        }
    }
    auto broken = reference;
    broken.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    const auto invalidImage = registration.estimateTranslation(broken, moving, options);
    const auto invalid = registration.prepareReference(std::move(broken), options);
    require(!invalidImage.ok, "invalid reference accepted");
    requireSameRegistration(invalidImage, registration.estimateTranslation(invalid, moving));
    options.starDetection.maxStars = 0;
    auto badOptions = registration.prepareReference(ImageBuffer(reference), options, true);
    require(registration.estimateSimilarity(badOptions, moving).errorCode == "ArgumentInvalid",
            "dense preparation masked an invalid coarse star limit");
}

void testTiedOffsetModes() {
    ImageBuffer moving;
    moving.width = 220; moving.height = 140; moving.channels = 1;
    moving.pixels.assign(moving.sampleCount(), 0);
    auto reference = moving;
    std::mt19937 random(414);
    std::vector<std::pair<int, int>> positions;
    while (positions.size() < 49) {
        const int x = 10 + random() % 80, y = 9 + random() % 120;
        if (std::any_of(positions.begin(), positions.end(), [&](auto p) {
            return std::hypot(p.first - x, p.second - y) < 9;
        })) continue;
        positions.emplace_back(x, y);
        const float value = 100.F + positions.size();
        moving.pixels[y * moving.width + x] = value;
        for (int dx : {1, 122}) reference.pixels[y * moving.width + x + dx] = value;
    }
    // The two identical reference groups tie exactly. Reconstruct the legacy
    // first-maximum choice in this standard library's hash iteration order.
    RegistrationOptions options;
    const auto a = StarDetector().detect(reference, options.starDetection).stars;
    const auto b = StarDetector().detect(moving, options.starDetection).stars;
    require(a.size() == 2 * positions.size() && b.size() == positions.size(), "tie fixture lost stars");
    struct Bin { std::vector<Translation> offsets; };
    std::unordered_map<std::string, Bin> bins;
    bins.reserve(a.size() * b.size());
    for (const auto& m : b) for (const auto& r : a) {
        const float dx = r.x - m.x, dy = r.y - m.y;
        const auto key = std::to_string(std::lround(dx / options.matchTolerance)) + ":" +
                         std::to_string(std::lround(dy / options.matchTolerance));
        bins[key].offsets.push_back({dx, dy});
    }
    const auto winner = std::max_element(bins.begin(), bins.end(),
        [](const auto& x, const auto& y) { return x.second.offsets.size() < y.second.offsets.size(); });
    require(winner->second.offsets.size() == b.size() && std::count_if(bins.begin(), bins.end(),
        [&](const auto& bin) { return bin.second.offsets.size() == b.size(); }) == 2, "fixture needs exactly two maxima");
    const auto actual = Registration().estimateTranslation(reference, moving, options);
    require(actual.ok && actual.translation.dx == winner->second.offsets.front().dx &&
            actual.translation.dy == 0 && actual.matches == b.size(), "tied offset changed old translation result");
}

}
int main() {
    testPreparedReference();
    testTiedOffsetModes();
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
    // A long alt-az session rotates about the field center. Dense proximity
    // correspondences must not outweigh a smaller, geometrically correct set.
    options.refineSimilarityCentroids = true;
    for (const float angle : {.18F, .65F, 1.4F}) {
        const SimilarityTransform truth{.scale = 1.0F, .rotationRadians = angle,
            .dx = 320 - std::cos(angle)*320 + std::sin(angle)*240,
            .dy = 240 - std::sin(angle)*320 - std::cos(angle)*240};
        const auto moving = field(492, false);
        const auto reference = field(492, false, truth);
        const auto result = Registration().estimateSimilarity(reference, moving, options);
        std::cout << "wide rotation " << angle << " accepted=" << result.ok
                  << " matches=" << result.matches << std::endl;
        require(result.ok && !result.usedFallback, "valid wide rotation rejected");
        require(std::abs(result.transform.rotationRadians - angle) < .001,
                "wrong rotation accepted for long session");
        require(std::hypot(result.transform.dx-truth.dx, result.transform.dy-truth.dy) < .25,
                "wrong shift accepted for long session");
    }
}
