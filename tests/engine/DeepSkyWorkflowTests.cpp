#include "photonstack/DeepSkyWorkflow.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/FileDigest.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <source_location>
#include <stdexcept>

using namespace photonstack;
namespace fs = std::filesystem;
void require(bool ok, const std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error("Deep-sky workflow check at line " + std::to_string(where.line()));
}
int main() {
    const auto root = fs::temp_directory_path() / ("photonstack-workflow-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{root};
    std::vector<fs::path> inputs;
    std::vector<std::uintmax_t> sizes;
    std::vector<fs::file_time_type> modified;
    std::vector<std::string> hashes;
    for (unsigned frame = 0; frame < 9; ++frame) {
        ImageBuffer image;
        image.width = 96; image.height = 128; image.channels = 4;
        image.pixels.resize(image.sampleCount());
        std::mt19937 random(frame + 1); std::normal_distribution<float> noise(0, 1);
        for (unsigned y = 0; y < image.height; ++y)
            for (unsigned x = 0; x < image.width; ++x) {
                const double nebula = 40 * std::exp(-((x - 48.) * (x - 48.) + (y - 64.) * (y - 64.)) / 140);
                const auto p = (y * image.width + x) * 4;
                for (int c = 0; c < 3; ++c)
                    image.pixels[p + c] = float(100 + frame * 10 + .2 * x + .1 * y + nebula + noise(random));
                if (frame == 0 && x == 25)
                    for (int c = 0; c < 3; ++c) image.pixels[p + c] += 400;
                image.pixels[p + 3] = x < 5 ? 0 : 1;
            }
        const auto path = root / ("light-" + std::to_string(frame) + ".fits");
        require(FitsCodec().write(image, path).ok); inputs.push_back(path);
        sizes.push_back(fs::file_size(path)); modified.push_back(fs::last_write_time(path));
        hashes.push_back(fileSHA256(path));
    }
    DeepSkyWorkflowOptions options;
    options.stack.alignSimilarity = false; // Synthetic frames already share coordinates.
    options.quality.measureStars = false;
    options.quality.sampleSide = 96;
    options.develop.stellarBalance = false;
    options.develop.toneScale = 10; options.develop.whitePoint = 200;
    options.polynomial.columns = 12; options.polynomial.rows = 16;
    double previous = 0;
    bool sawConsumedWork = false;
    bool sawCombineStart = false, sawCombineEnd = false;
    auto result = DeepSkyWorkflow().run(inputs, root / "first", options,
        [&](const std::string& stage, double fraction, std::size_t step, std::size_t total) {
            require(fraction >= previous && fraction >= 0 && fraction <= 1); previous = fraction;
            if (stage == "stack-combine") {
                require(total == 128 && step <= total);
                sawCombineStart |= step == 0;
                sawCombineEnd |= step == total;
            }
            if (stage == "stack" || stage == "stack-combine") {
                std::size_t copies = 0;
                for (const auto& item : fs::recursive_directory_iterator(root / "first/.aligned-work"))
                    if (item.is_regular_file()) ++copies;
                // At most one additional frame exists during conversion, never
                // two whole sequences of aligned FITS and internal float data.
                require(copies <= inputs.size()+1);
            }
            if ((stage == "stack" || stage == "stack-combine") && fraction > .58 + .27*.82) {
                std::size_t cachedFrames = 0;
                for (const auto& item : fs::recursive_directory_iterator(root / "first/.aligned-work")) {
                    require(item.path().extension() != ".fits");
                    if (item.path().extension() == ".f32") ++cachedFrames;
                }
                require(cachedFrames == inputs.size());
                for (const auto& input : inputs) require(fs::exists(input));
                sawConsumedWork = true;
            }
        });
    if (!result.ok) throw std::runtime_error(result.errorCode + ": " + result.message);
    require(result.selectedCount == inputs.size() && previous == 1 && sawConsumedWork);
    require(sawCombineStart && sawCombineEnd);
    require(result.cropX == 5 && result.cropY == 0 && result.cropWidth == 91 && result.cropHeight == 128);
    require(result.frames[0].rejectedHighSamples > 200);
    require(fs::exists(result.master) && fs::exists(result.backgroundMaster) && fs::exists(result.developed));
    require(fs::exists(result.rejectionLow) && fs::exists(result.rejectionHigh));
    require(!fs::exists(root / "first/.aligned-work"));
    require(!fs::exists(root / "first/stack-multiscale.fits"));
    bool cancelledAfterConsumption = false;
    const auto cancelled = DeepSkyWorkflow().run(inputs, root / "cancelled", options,
        [&](const std::string& stage, double fraction, std::size_t, std::size_t) {
            if (stage == "stack" && fraction > .7) {
                cancelledAfterConsumption = true;
                throw std::runtime_error("test cancellation during cache conversion");
            }
        });
    require(cancelledAfterConsumption && !cancelled.ok);
    require(!fs::exists(root / "cancelled/.aligned-work") && !fs::exists(root / "cancelled/stack-linear.fits"));
    const auto developed = ImageCodec().read(result.developed);
    require(developed.ok && developed.image.width == 91 && developed.image.height == 128);
    require(developed.image.sourceBitsPerChannel == 16);
    auto again = DeepSkyWorkflow().run(inputs, root / "second", options);
    require(again.ok && again.selectedCount == result.selectedCount && again.referenceIndex == result.referenceIndex);
    require(ImageCodec().read(again.developed).image.pixels == developed.image.pixels);
    auto finish = DeepSkyWorkflow().finishMaster(result.master, root / "finish", options);
    require(finish.ok && finish.masterReused && finish.frames.empty());
    require(ImageCodec().read(finish.developed).image.pixels == developed.image.pixels);
    require(!DeepSkyWorkflow().finishMaster(result.master, root / "finish", options).ok);
    auto unsupportedNoise = options;
    unsupportedNoise.stack.collectNoiseDiagnostic = true;
    unsupportedNoise.stack.frameNoiseVariances.assign(inputs.size(), 1);
    const auto noiseRun = DeepSkyWorkflow().run(inputs, root / "unsupported-noise", unsupportedNoise);
    const auto noiseFinish = DeepSkyWorkflow().finishMaster(result.master, root / "unsupported-finish", unsupportedNoise);
    require(!noiseRun.ok && noiseRun.errorCode == "ArgumentInvalid");
    require(!noiseFinish.ok && noiseFinish.errorCode == "ArgumentInvalid");
    require(!fs::exists(root / "unsupported-noise") && !fs::exists(root / "unsupported-finish"));
    auto backgroundOptions = options;
    // A diagnostic belongs to the scientific stack and must survive finish-only
    // replay with exactly the same crop/colour transform. Disable clipping here
    // so this nine-frame fixture has >=3 accepted samples in both halves.
    auto independentOptions = options;
    independentOptions.noiseModel = DeepSkyNoiseModel::IndependentLuminance;
    independentOptions.stack.sigma.sigmaLow = 1000;
    independentOptions.stack.sigma.sigmaHigh = 1000;
    independentOptions.quality.overrides.assign(inputs.size(), FrameSelectionOverride::Keep);
    auto independent = DeepSkyWorkflow().run(inputs, root / "independent", independentOptions);
    if (!independent.ok) throw std::runtime_error(independent.errorCode + ": " + independent.message);
    require(independent.independentLuminanceNoise && independent.cropX == 5);
    require(fs::exists(independent.noiseReference) && fs::exists(independent.noiseReferenceManifest) && fs::exists(independent.noiseReferenceInputs));
    auto replay = DeepSkyWorkflow().finishMaster(independent.master, root / "independent-replay", independentOptions);
    if (!replay.ok) throw std::runtime_error(replay.errorCode + ": " + replay.message);
    require(replay.independentLuminanceNoise && replay.noiseReference == independent.noiseReference);
    require(fileSHA256(replay.developed) == fileSHA256(independent.developed));
    auto missing = DeepSkyWorkflow().finishMaster(result.master, root / "missing-noise", independentOptions);
    require(!missing.ok && missing.errorCode == "NoiseReferenceInvalid" && !fs::exists(root / "missing-noise"));
    auto incompatible = independentOptions; incompatible.denoiseH = 5;
    require(!DeepSkyWorkflow().run(inputs, root / "nlm-noise", incompatible).ok);
    require(!DeepSkyWorkflow().finishMaster(independent.master, root / "nlm-replay", incompatible).ok);
    require(!fs::exists(root / "nlm-noise") && !fs::exists(root / "nlm-replay"));
    for (const auto& path : {independent.master, independent.noiseReference, independent.noiseReferenceInputs, independent.noiseReferenceManifest}) {
        std::ifstream original(path, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(original), {}};
        original.close();
        { std::ofstream changed(path, std::ios::binary | std::ios::app); changed << "tampered"; }
        const auto stale = DeepSkyWorkflow().finishMaster(independent.master, root / "stale-noise", independentOptions);
        require(!stale.ok && stale.errorCode == "NoiseReferenceInvalid" && !fs::exists(root / "stale-noise"));
        { std::ofstream restore(path, std::ios::binary); restore << bytes; }
    }
    backgroundOptions.backgroundDenoiseStrength = .8;
    backgroundOptions.denoiseH = 2;
    backgroundOptions.saveDenoiseStages = true;
    backgroundOptions.polynomial.exclusions = {{48,64,15,12,25}};
    auto backgroundFinish = DeepSkyWorkflow().finishMaster(result.master, root / "finish-background", backgroundOptions);
    require(backgroundFinish.ok && backgroundFinish.cropX == 5);
    require(fs::exists(root / "finish-background/stack-denoised.fits"));
    auto compactOptions = backgroundOptions;
    compactOptions.saveDenoiseStages = false;
    const auto compact = DeepSkyWorkflow().finishMaster(result.master, root / "finish-compact", compactOptions);
    require(compact.ok && compact.master == backgroundFinish.master);
    require(fileSHA256(compact.developed) == fileSHA256(backgroundFinish.developed));
    require(fileSHA256(compact.backgroundMaster) == fileSHA256(backgroundFinish.backgroundMaster));
    for (const auto* name : {"stack-denoised.fits", "stack-multiscale.fits", "stack-background-denoised.fits"})
        require(!fs::exists(root / "finish-compact" / name));
    const auto unfiltered = FitsCodec().read(root / "finish-background/stack-multiscale.fits");
    const auto filtered = FitsCodec().read(root / "finish-background/stack-background-denoised.fits");
    require(unfiltered.ok && filtered.ok);
    // Native source (48,64) becomes (43,64) after the nonzero coverage crop.
    for(unsigned y=60;y<=68;++y)for(unsigned x=39;x<=47;++x)for(unsigned c=0;c<4;++c) {
        const auto p=(std::size_t(y)*filtered.image.width+x)*4+c;
        require(unfiltered.image.pixels[p] == filtered.image.pixels[p]);
    }
    auto collision = DeepSkyWorkflow().run(inputs, root / "first", options);
    require(!collision.ok && collision.errorCode == "OutputExists");
    require(ImageCodec().read(result.developed).image.pixels == developed.image.pixels);
    options.analysisOnly = true;
    auto analysis = DeepSkyWorkflow().run(inputs, root / "analysis", options);
    require(analysis.ok && analysis.master.empty() && analysis.developed.empty());
    require(!fs::exists(root / "analysis/.aligned-work"));
    options.quality.overrides.assign(inputs.size(), FrameSelectionOverride::Reject);
    auto rejected = DeepSkyWorkflow().run(inputs, root / "rejected", options);
    require(!rejected.ok && rejected.errorCode == "SelectionNeedsReview" && rejected.frames.size() == inputs.size());
    require(!fs::exists(root / "rejected/.aligned-work"));
    require(!fs::exists(root / "rejected/stack-linear.fits"));
    options.quality.overrides.clear();
    options.stack.alignSimilarity = true;
    options.stack.registration.minimumMatches = 10000;
    const auto badRegistration = DeepSkyWorkflow().run(inputs, root / "bad-registration", options);
    require(!badRegistration.ok && badRegistration.errorCode == "SelectionNeedsReview");
    require(badRegistration.selectedCount == 1);
    for (std::size_t i = 0; i < inputs.size(); ++i)
        if (i != badRegistration.referenceIndex)
            require(badRegistration.frames[i].errorCode == "RegistrationRejected" && !badRegistration.frames[i].assessment.usable);
    require(!fs::exists(root / "bad-registration/.aligned-work"));
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        require(fs::file_size(inputs[i]) == sizes[i] && fs::last_write_time(inputs[i]) == modified[i]);
        require(fileSHA256(inputs[i]) == hashes[i]);
    }
}
