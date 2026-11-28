#include "../../engine/src/WeightedQuantile.hpp"
#include "photonstack/ArtifactTrailRemover.hpp"
#include "photonstack/StarDetector.hpp"
#include "photonstack/Registration.hpp"
#include "photonstack/DeepSkyRecipe.hpp"
#include "photonstack/FileDigest.hpp"
#include "photonstack/AstrometricValidation.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <source_location>
#include <unistd.h>
using namespace photonstack;
void require(bool value, const std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Audit boundary regression at " + std::to_string(where.line()));
}
int main() {
    char filename[] = "/tmp/photonstack-hash-test-XXXXXX";
    const int fd = mkstemp(filename);
    require(fd >= 0);
    struct TemporaryFile {
        int fd;
        const char* name;
        ~TemporaryFile() { close(fd); unlink(name); }
    } temporary{fd, filename};
    const std::string contents = "abcdefghijklmnopqrstuvwxyz";
    require(write(fd, contents.data(), contents.size()) == static_cast<ssize_t>(contents.size()));
    DigestingFileReader reader(filename);
    std::array<unsigned char, 5> part;
    for (unsigned offset : {9, 3, 12, 16}) {
        reader.readAt(offset, part);
        require(std::equal(part.begin(), part.end(), contents.begin() + offset));
    }
    require(reader.finish() == "71c480df93d6ae2f1efad1447c66c9525e316218cf51fc8d9ed832f2daf18b73");
    DigestingFileReader changing(filename);
    changing.readAt(0, part);
    require(write(fd, "!", 1) == 1);
    bool changed = false;
    try { changing.finish(); } catch (const std::runtime_error&) { changed = true; }
    require(changed);
    struct Sample { float value, weight; };
    for (int population : {1, 2, 17, 129, 1024}) for (int kind : {0, 1, 2}) {
        std::vector<Sample> original;
        for (int i = 0; i < population; ++i) {
            const float v = float((i * 73) % (kind == 2 ? 17 : 1049)) - 127.F;
            original.push_back({v, kind == 0 ? 1.F : float(1 + (i * 53) % 47) / 64.F});
        }
        auto sorted = original;
        std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a.value < b.value; });
        double total = 0;
        for (auto s : sorted) total += s.weight;
        for (int percentile = 0; percentile <= 100; ++percentile) {
            const double q = percentile / 100.;
            double mass = 0, previous = 0;
            float expected = sorted.front().value;
            for (auto s : sorted) {
                mass += s.weight;
                const double pos = (mass - s.weight * .5) / total;
                if (pos >= q) { expected = float(double(expected) + (double(s.value) - expected) *
                                                      std::clamp((q - previous) / (pos - previous), 0., 1.)); break; }
                previous = pos; expected = s.value;
            }
            auto samples = original;
            require(detail::weightedSampleQuantile(samples, q) == expected);
        }
    }
    ImageBuffer image; image.width = image.height = 16; image.channels = 1; image.pixels.assign(256, .2F);
    image.pixels[0] = 100;
    StarDetectionOptions stars; stars.border = 0;
    require(StarDetector().detect(image, stars).errorCode == "ArgumentInvalid");
    require(StarDetector().detectLuminance(16, 16, image.pixels, stars).errorCode == "ArgumentInvalid");
    stars.border = 1; require(StarDetector().detect(image, stars).ok);
    ArtifactTrailOptions options; options.useDetectedTrails = true;
    const ArtifactTrailRemover remover;
    require(remover.remove({}, options).errorCode == "ImageBufferInvalid");
    auto broken = image; broken.pixels.resize(1);
    require(remover.remove(broken, options).errorCode == "ImageBufferInvalid");
    options.inpaintRadius = 65;
    require(remover.remove(image, options).errorCode == "ArgumentInvalid");
    options.inpaintRadius = 4;
    ArtifactTrail trail; trail.kind = ArtifactTrailKind::Satellite; trail.x1 = 2; trail.y1 = 2;
    trail.x2 = 12; trail.y2 = 12; trail.width = 1; trail.length = 14;
    options.detectedTrails = {trail}; require(remover.remove(image, options).ok);
    options.detectedTrails[0].x1 = std::numeric_limits<float>::quiet_NaN();
    require(remover.remove(image, options).errorCode == "ArgumentInvalid");
    options.detectedTrails = {trail}; options.detectedTrails[0].path = {{INFINITY, 0}};
    require(remover.remove(image, options).errorCode == "ArgumentInvalid");
    options.detectedTrails = {trail}; broken = image; broken.pixels[20] = INFINITY;
    require(remover.remove(broken, options).errorCode == "ImageBufferInvalid");
    require(remover.detect(broken).errorCode == "ImageBufferInvalid");
    // A singular homography must not silently become an identity mapping.
    image.channels = 4; image.pixels.assign(image.sampleCount(), 1);
    DistortionTransform transform; transform.useProjective = true; transform.projective.h22 = 0;
    const auto mapped = Registration().applyDistortion(image, transform);
    require(!mapped.empty());
    for (std::size_t i = 0; i < mapped.pixelCount(); ++i) require(mapped.pixels[i*4+3] == 0);
    // Non-finite alpha must not turn missing coverage into valid astrometric stars.
    ImageBuffer sky; sky.width = sky.height = 256; sky.channels = 4; sky.pixels.assign(sky.sampleCount(), 0);
    for (std::size_t i = 0; i < sky.pixelCount(); ++i) sky.pixels[i*4+3] = 1;
    for (int j = 0; j < 8; ++j) for (int i = 0; i < 8; ++i) {
        const double cx = 20 + 30*i + .2, cy = 20 + 30*j + .35;
        for (int y = int(cy)-6; y <= int(cy)+6; ++y) for (int x = int(cx)-6; x <= int(cx)+6; ++x) {
            const float signal = (100 + 11*(j*8+i))*std::exp(-((x-cx)*(x-cx)+(y-cy)*(y-cy))/(2*1.25*1.25));
            for (int c = 0; c < 3; ++c) sky.pixels[(y*256+x)*4+c] = signal;
        }
    }
    require(astrometricSources(sky).size() >= 40);
    for (std::size_t i = 0; i < sky.pixelCount(); ++i) sky.pixels[i*4+3] = NAN;
    bool missingCoverage = false;
    try { astrometricSources(sky); } catch (const std::runtime_error&) { missingCoverage = true; }
    require(missingCoverage);
    // Recipe and decoder share the same exposure range.
    auto recipe = DeepSkyRecipe::parse("PHOTONSTACK_DEEP_SKY_RECIPE 1\nraw.exposure \"5\"\n", "/tmp");
    require(recipe.options.stack.raw.exposureBias == 5);
    bool rejected = false;
    try { DeepSkyRecipe::parse("PHOTONSTACK_DEEP_SKY_RECIPE 1\nraw.exposure \"6\"\n", "/tmp"); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);
}
