#include "photonstack/SequenceQualityAnalyzer.hpp"
#include "photonstack/StarDetector.hpp"

#include <cmath>
#include <limits>
#include <random>
#include <source_location>
#include <stdexcept>

using namespace photonstack;
namespace {
void require(bool value, const std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Sequence quality check at line " + std::to_string(where.line()));
}
bool has(const std::vector<std::string>& values, const std::string& value) {
    for (const auto& v : values) if (v == value) return true;
    return false;
}
ImageBuffer scene(unsigned seed, double noise = 2, double offset = 0, double gradient = 0) {
    ImageBuffer image;
    image.width = image.height = 128;
    image.channels = 4;
    image.pixels.resize(image.sampleCount());
    std::mt19937 random(seed);
    std::normal_distribution<double> normal(0, noise);
    for (unsigned y = 0; y < image.height; ++y)
        for (unsigned x = 0; x < image.width; ++x) {
            // Stable extended object must not be mistaken for random noise or a transient.
            const double nebula = 40 * std::exp(-((x - 61.) * (x - 61.) + (y - 65.) * (y - 65.)) / 600);
            const float value = float(100 + offset + gradient * (x + 2 * y) + nebula + normal(random));
            const auto p = (y * image.width + x) * 4;
            image.pixels[p] = image.pixels[p + 1] = image.pixels[p + 2] = value;
            image.pixels[p + 3] = 1;
        }
    return image;
}
} // namespace

int main() {
    const SequenceQualityAnalyzer analyzer;
    // Rich fields must not all report the return-list cap. Preserve the same
    // brightest samples for registration and native shape fitting.
    {
        ImageBuffer image;
        image.width = image.height = 768; image.channels = 1;
        image.pixels.assign(image.sampleCount(), 100);
        std::size_t expected = 0;
        for (int cy = 24; cy < 744; cy += 16) for (int cx = 24; cx < 744; cx += 16) {
            ++expected;
            const double amplitude = 1000 + expected;
            for (int dy = -6; dy <= 6; ++dy) for (int dx = -6; dx <= 6; ++dx)
                image.pixels[std::size_t(cy + dy) * image.width + cx + dx] +=
                    float(amplitude * std::exp(-(dx * dx + dy * dy) / 2.88));
        }
        StarDetectionOptions options; options.robustStatistics = true;
        const auto capped = StarDetector().detect(image, options);
        options.countAllDetections = true;
        const auto complete = StarDetector().detect(image, options);
        require(capped.ok && complete.ok && expected > 1000);
        require(capped.stars.size() == 1000 && capped.detectedCount == 1000);
        require(complete.stars.size() == 1000 && complete.detectedCount == expected);
        for (std::size_t i = 0; i < capped.stars.size(); ++i) {
            require(capped.stars[i].x == complete.stars[i].x && capped.stars[i].y == complete.stars[i].y);
            require(capped.stars[i].flux == complete.stars[i].flux);
        }
        options.maxStars = 23;
        const auto limited = StarDetector().detect(image, options);
        require(limited.ok && limited.stars.size() == 23 && limited.detectedCount == expected);
        const auto measured = analyzer.sample(image);
        require(measured.ok && measured.starCount == expected && measured.starShapeCount >= 60);
        auto sparse = measured; sparse.starCount = 400;
        std::vector<SequenceFrameSample> frames(10, measured); frames[0] = sparse;
        SequenceQualityOptions quality; quality.applySelection = true;
        const auto assessed = analyzer.assess(frames, quality);
        require(assessed.ok && !assessed.frames[0].selected && has(assessed.frames[0].reasons, "few-stars"));

        // Compare the spatial index with the original unbounded greedy scan on
        // irregular close peaks, including neighboring grid cells and edges.
        std::mt19937 rng(713);
        std::uniform_real_distribution<float> values(0, 100);
        image.width = 91; image.height = 87;
        image.pixels.resize(image.sampleCount());
        for (auto& value : image.pixels) value = values(rng);
        options.maxStars = image.pixelCount(); options.sigmaThreshold = .2F;
        options.countAllDetections = false;
        const auto greedy = StarDetector().detect(image, options);
        options.countAllDetections = true;
        const auto indexed = StarDetector().detect(image, options);
        require(greedy.ok && indexed.ok && greedy.stars.size() > 100);
        require(indexed.detectedCount == greedy.stars.size() && indexed.stars.size() == greedy.stars.size());
        for (std::size_t i = 0; i < greedy.stars.size(); ++i)
            require(greedy.stars[i].x == indexed.stars[i].x && greedy.stars[i].y == indexed.stars[i].y);
    }
    // Detection resizing must not inflate native PSF widths. A sensor-sized
    // field crosses the 1 MP detection budget and includes subpixel phases.
    for (double sigma : {.9, 1.5, 2.4}) {
        ImageBuffer image;
        image.width = sigma == 1.5 ? 2160 : 1536;
        image.height = sigma == 1.5 ? 3840 : 1152;
        image.channels = 1;
        image.pixels.assign(image.sampleCount(), -200);
        for (int row = 0; row < 8; ++row) for (int col = 0; col < 10; ++col) {
            const double cx = 80 + col * ((image.width - 160) / 10) + .13 * (row % 5);
            const double cy = 70 + row * ((image.height - 140) / 8) + .17 * (col % 5);
            for (int y = int(cy) - 14; y <= int(cy) + 14; ++y)
                for (int x = int(cx) - 14; x <= int(cx) + 14; ++x)
                    image.pixels[std::size_t(y) * image.width + x] += float(3000 *
                        std::exp(-((x-cx)*(x-cx)+(y-cy)*(y-cy))/(2*sigma*sigma)));
        }
        const auto measured = analyzer.sample(image);
        require(measured.ok && measured.starsMeasured && measured.starShapeCount >= 60);
        require(std::abs(measured.medianFwhm - 2.354820045 * sigma) < .015);
        require(measured.medianEccentricity < .03);
        for (auto& v : image.pixels) v = v * .02F + 8;
        const auto scaled = analyzer.sample(image);
        require(scaled.starShapeCount == measured.starShapeCount);
        require(std::abs(scaled.medianFwhm - measured.medianFwhm) < .001);
    }
    SequenceQualityOptions options;
    options.sampleSide = 128;
    options.measureStars = false;
    const auto flat = analyzer.sample(scene(1), options);
    const auto gradient = analyzer.sample(scene(1, 2, 4000, 10), options);
    require(flat.ok && gradient.ok);
    require(std::abs(flat.noise - 2) < .12);
    require(std::abs(gradient.noise - flat.noise) < .01);
    auto extreme = scene(1);
    extreme.pixels[100 * 4] = extreme.pixels[100 * 4 + 1] = extreme.pixels[100 * 4 + 2] = 1.e7;
    require(std::abs(analyzer.sample(extreme, options).noise - flat.noise) < .01);
    auto scaled = scene(1);
    for (std::size_t p = 0; p < scaled.pixelCount(); ++p)
        for (int c = 0; c < 3; ++c) scaled.pixels[p * 4 + c] *= 5;
    require(std::abs(analyzer.sample(scaled, options).noise / flat.noise - 5) < .001);

    auto starField = scene(17, .5);
    for (int cy = 16; cy < 120; cy += 24)
        for (int cx = 16; cx < 120; cx += 24)
            for (int dy = -5; dy <= 5; ++dy)
                for (int dx = -5; dx <= 5; ++dx)
                    for (int c = 0; c < 3; ++c)
                        starField.pixels[((cy + dy) * 128 + cx + dx) * 4 + c] += float(150 * std::exp(-(dx * dx + dy * dy) / 3.));
    auto starOptions = options; starOptions.measureStars = true;
    const auto beforeTrail = analyzer.sample(starField, starOptions);
    for (int x = 0; x < 128; ++x)
        for (int c = 0; c < 3; ++c) starField.pixels[(3 * 128 + x) * 4 + c] += 100000;
    const auto afterTrail = analyzer.sample(starField, starOptions);
    require(beforeTrail.starCount >= 20 && afterTrail.starCount >= beforeTrail.starCount * .9);
    require(std::abs(beforeTrail.medianFwhm - afterTrail.medianFwhm) < .1);

    const auto gaussianField = [](double sigma) {
        auto image = scene(22, .05);
        for (int cy = 16; cy < 120; cy += 24)
            for (int cx = 16; cx < 120; cx += 24)
                for (int dy = -8; dy <= 8; ++dy)
                    for (int dx = -8; dx <= 8; ++dx)
                        for (int c = 0; c < 3; ++c)
                            image.pixels[((cy + dy) * 128 + cx + dx) * 4 + c] +=
                                float(1500 / (sigma * sigma) * std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma)));
        return image;
    };
    const auto sharpStars = analyzer.sample(gaussianField(.7), starOptions);
    const auto blurredStars = analyzer.sample(gaussianField(2.2), starOptions);
    require(sharpStars.starCount >= 20 && blurredStars.starCount >= 20);
    require(blurredStars.medianFwhm > sharpStars.medianFwhm * 1.1);
    auto blurOptions = starOptions; blurOptions.applySelection = true; blurOptions.maximumFwhmRatio = 1.1;
    std::vector<SequenceFrameSample> blurFrames(10, sharpStars); blurFrames[0] = blurredStars;
    const auto blurResult = analyzer.assess(blurFrames, blurOptions);
    require(blurResult.ok && !blurResult.frames[0].selected && has(blurResult.frames[0].reasons, "broad-stars"));
    auto missingShapes = sharpStars;
    missingShapes.starShapeCount = 0; missingShapes.medianFwhm = 0; missingShapes.medianEccentricity = 0;
    std::vector<SequenceFrameSample> partlyMeasured(10, missingShapes);
    partlyMeasured[0] = sharpStars; partlyMeasured[1] = blurredStars;
    const auto unknownResult = analyzer.assess(partlyMeasured, blurOptions);
    require(unknownResult.ok && unknownResult.medianFwhm > 0);
    require(has(unknownResult.frames[2].warnings, "stellar-shapes-unavailable"));

    std::vector<SequenceFrameSample> frames;
    for (unsigned i = 0; i < 12; ++i) frames.push_back(analyzer.sample(scene(i + 3, 2, i * 30), options));
    options.applySelection = true;
    auto baseline = analyzer.assess(frames, options);
    require(baseline.ok && baseline.selectedCount == frames.size());
    for (const auto& f : baseline.frames) {
        require(f.temporalAvailable);
        require(f.positiveFraction < .001 && f.negativeFraction < .001);
        require(!has(f.warnings, "stellar-measurements-unavailable"));
    }
    auto expectedMeasurements = options; expectedMeasurements.measureStars = true;
    const auto missingMeasurements = analyzer.assess(frames, expectedMeasurements);
    require(missingMeasurements.ok);
    for (const auto& f : missingMeasurements.frames) require(has(f.warnings, "stellar-measurements-unavailable"));

    // A long bright trail and a thin negative dropout are local pixel anomalies.
    // Neither should discard the otherwise useful exposure; both signs are reported.
    auto transient = scene(3);
    for (unsigned y = 5; y < 123; ++y) {
        for (int c = 0; c < 3; ++c) {
            transient.pixels[(y * 128 + 30) * 4 + c] += 200;
            transient.pixels[(y * 128 + 95) * 4 + c] -= 200;
        }
    }
    frames[0] = analyzer.sample(transient, options);
    auto trails = analyzer.assess(frames, options);
    require(trails.ok && trails.frames[0].selected);
    require(trails.frames[0].positiveFraction > .005 && trails.frames[0].negativeFraction > .005);
    require(has(trails.frames[0].warnings, "localized-transients-use-pixel-rejection"));
    require(trails.frames[0].trails.size() == 2);
    require(trails.frames[0].trails[0].sign == 1 && trails.frames[0].trails[1].sign == -1);
    require(trails.frames[0].trails[0].samples > 100);
    require(baseline.frames[0].trails.empty());

    // Broad nonuniform cloud/sky residual produces both signs after additive sky matching.
    auto cloud = scene(3);
    for (unsigned y = 0; y < 128; ++y)
        for (unsigned x = 0; x < 128; ++x)
            for (int c = 0; c < 3; ++c) cloud.pixels[(y * 128 + x) * 4 + c] += x < 64 ? 40 : -40;
    frames[0] = analyzer.sample(cloud, options);
    auto clouds = analyzer.assess(frames, options);
    require(clouds.ok && !clouds.frames[0].selected);
    require(has(clouds.frames[0].reasons, "broad-temporal-residuals"));
    require(clouds.frames[0].positiveFraction > .3 && clouds.frames[0].negativeFraction > .3);

    frames[0] = analyzer.sample(scene(3, 8), options);
    auto noisy = analyzer.assess(frames, options);
    require(noisy.ok && !noisy.frames[0].selected && has(noisy.frames[0].reasons, "high-noise"));
    for (auto& f : frames) {
        f.starsMeasured = true; f.starCount = 100; f.medianFwhm = 3; f.medianEccentricity = .3;
    }
    frames[1].medianFwhm = 7;
    frames[2].medianEccentricity = .9;
    frames[3].starCount = 10;
    auto stars = analyzer.assess(frames, options);
    require(stars.ok && !stars.frames[1].selected && !stars.frames[2].selected && !stars.frames[3].selected);
    require(has(stars.frames[1].reasons, "broad-stars") && has(stars.frames[2].reasons, "elongated-stars"));
    require(has(stars.frames[3].reasons, "few-stars"));
    options.overrides.assign(frames.size(), FrameSelectionOverride::Automatic);
    options.overrides[1] = FrameSelectionOverride::Keep;
    options.overrides[4] = FrameSelectionOverride::Reject;
    auto manual = analyzer.assess(frames, options);
    require(manual.ok && manual.frames[1].selected && !manual.frames[1].recommendedKeep && !manual.frames[4].selected);

    // Masked borders must never create negative residuals or bias the sky/noise.
    options.overrides.clear();
    auto edge = scene(13);
    for (unsigned y = 0; y < 128; ++y)
        for (unsigned x = 0; x < 20; ++x) {
            edge.pixels[(y * 128 + x) * 4 + 3] = 0;
            edge.pixels[(y * 128 + x) * 4] = std::numeric_limits<float>::quiet_NaN();
        }
    auto edgeSample = analyzer.sample(edge, options);
    require(edgeSample.ok && std::abs(edgeSample.validFraction - 108. / 128) < 1.e-6);
    frames.assign(10, flat);
    frames[0] = edgeSample;
    auto edges = analyzer.assess(frames, options);
    require(edges.ok && edges.frames[0].selected && edges.frames[0].negativeFraction < .001);
    frames[0].columns -= 1;
    require(!analyzer.assess(frames, options).frames[0].usable);

    // Too few peers disables temporal evidence rather than inventing confidence.
    frames.assign(3, flat);
    auto small = analyzer.assess(frames, options);
    require(small.ok && !small.frames[0].temporalAvailable);
    require(has(small.frames[0].warnings, "insufficient-temporal-peers"));
    options.overrides.assign(3, FrameSelectionOverride::Reject);
    auto tooFew = analyzer.assess(frames, options);
    require(!tooFew.ok && tooFew.errorCode == "SelectionNeedsReview");
    options.overrides.assign(3, FrameSelectionOverride::Keep);
    frames[0].ok = false;
    require(!analyzer.assess(frames, options).frames[0].selected);
    options.overrides.clear();
    options.residualSigma = std::numeric_limits<double>::quiet_NaN();
    require(!analyzer.assess(frames, options).ok);
    require(!analyzer.sample(scene(1), options).ok);
    options.residualSigma = 6;
    auto display = scene(1); display.colorEncoding = ColorEncoding::SRGB;
    require(!analyzer.sample(display, options).ok);
}
