#include "photonstack/DeepSkyWorkflow.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/FileDigest.hpp"
#include "../../engine/src/DeepSkyAnalysisScheduling.hpp"

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
    {
        const auto megapixel = 1024ULL * 1024;
        const auto gib = 1024ULL * megapixel;
        require(detail::analysisFrameCapacity(8 * megapixel, 8 * megapixel, 711, 192, 3 * gib, 12) == 4);
        require(detail::analysisFrameCapacity(2 * megapixel, 2 * megapixel, 200, 128, 3 * gib, 12) == 8);
        require(detail::analysisFrameCapacity(32 * megapixel, 0, 711, 192, 3 * gib, 12) == 1);
        require(detail::analysisFrameCapacity(8 * megapixel, 8 * megapixel, 711, 192, gib, 12) == 1);
        require(detail::analysisFrameCapacity(0, 0, 20, 192, 3 * gib, 12) == 1);

        // Frame 0 waits for frame 1 to finish. Results must nevertheless
        // be committed in source order, on the thread issuing progress.
        const auto caller = std::this_thread::get_id();
        std::promise<void> later;
        auto finished = later.get_future();
        std::vector<std::size_t> committed;
        detail::orderedAnalysisFrames(5,
            [&](std::size_t) { require(std::this_thread::get_id() == caller); return true; },
            [](std::size_t i) -> std::size_t { return i < 2 ? 2 : 1; },
            [&](std::size_t i) {
                if (i == 0) require(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
                if (i == 1) later.set_value();
                if (i >= 2) require(std::this_thread::get_id() == caller);
                return i;
            },
            [&](std::size_t i, std::size_t result) {
                require(std::this_thread::get_id() == caller && result == i);
                committed.push_back(i);
            });
        require(committed == std::vector<std::size_t>({0, 1, 2, 3, 4}));
        std::atomic<unsigned> ended{0};
        try {
            detail::orderedAnalysisFrames(4,
                [](std::size_t i) { if (i == 2) throw std::runtime_error("later input"); return true; },
                [](std::size_t) { return 2U; },
                [&](std::size_t i) {
                    ++ended;
                    if (i == 0) throw std::runtime_error("first input");
                    return i;
                }, [](std::size_t, std::size_t) {});
            require(false);
        } catch (const std::runtime_error& e) {
            require(std::string(e.what()) == "first input" && ended == 2);
        }
    }
    {
        std::vector<fs::path> stars;
        for (unsigned frame = 0; frame < 6; ++frame) {
            ImageBuffer image;
            image.width = 131; image.height = 119; image.channels = 1;
            image.pixels.assign(image.sampleCount(), 100 + frame);
            std::mt19937 rng(51);
            for (unsigned k = 0; k < 42; ++k) {
                const int cx = 10 + rng() % 106 + frame % 3, cy = 10 + rng() % 95 + frame / 2;
                for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx)
                    image.pixels[(cy + dy) * image.width + cx + dx] += float((100 + k) * std::exp(-(dx*dx + dy*dy) / 3.));
            }
            stars.push_back(root / ("stars-" + std::to_string(frame) + ".fits"));
            require(FitsCodec().write(image, stars.back()).ok);
        }
        DeepSkyWorkflowOptions o;
        o.analysisOnly = true;
        o.quality.sampleSide = 32;
        o.stack.registration.minimumMatches = 4;
        o.analysisWorkers = 1;
        const auto serial = DeepSkyWorkflow().run(stars, root / "serial-stars", o);
        o.analysisWorkers = 4;
        std::vector<std::size_t> measureSteps, alignSteps;
        const auto parallel = DeepSkyWorkflow().run(stars, root / "parallel-stars", o,
            [&](const auto& stage, double, std::size_t step, std::size_t) {
                if (stage == "measure") measureSteps.push_back(step);
                if (stage == "align") alignSteps.push_back(step);
            });
        require(serial.ok && parallel.ok && serial.referenceIndex == parallel.referenceIndex);
        require(serial.selectedCount == parallel.selectedCount && serial.selectedCount >= 3);
        require(measureSteps == std::vector<std::size_t>({1, 2, 3, 4, 5, 6}) && measureSteps == alignSteps);
        for (std::size_t i = 0; i < stars.size(); ++i) {
            const auto& a = serial.frames[i]; const auto& b = parallel.frames[i];
            require(a.native.starCount == b.native.starCount && a.native.medianFwhm == b.native.medianFwhm);
            require(a.registration.ok && b.registration.ok && a.registration.matches == b.registration.matches);
            require(a.registration.transform.dx == b.registration.transform.dx && a.registration.transform.dy == b.registration.transform.dy);
            require(a.assessment.residualRms == b.assessment.residualRms && a.assessment.reasons == b.assessment.reasons);
        }
    }
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
    // The report is persistent scalar evidence, not a second copy of the
    // per-exposure temporal grids. Large sequences must not retain that storage.
    for (const auto& frame : result.frames) {
        require(frame.native.ok && frame.native.noise > 0);
        require(frame.native.luminance.empty() && frame.native.luminance.capacity() == 0);
        require(frame.native.coverage.empty() && frame.native.coverage.capacity() == 0);
    }
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
    require(!result.denoiseStretchLocked && !finish.denoiseStretchLocked);
    auto lockedOptions = options;
    lockedOptions.lockDenoiseStretch = true;
    lockedOptions.develop.toneScale = 0; lockedOptions.develop.whitePoint = 0;
    lockedOptions.develop.starExposure = 1; lockedOptions.develop.starColorCleanup = 0;
    lockedOptions.backgroundDenoiseStrength = 0;
    lockedOptions.polynomial.exclusions = {{48,64,15,12,25}};
    const auto lockedBefore = DeepSkyWorkflow().finishMaster(result.master, root / "locked-before", lockedOptions);
    lockedOptions.backgroundDenoiseStrength = .8;
    const auto lockedAfter = DeepSkyWorkflow().finishMaster(result.master, root / "locked-after", lockedOptions);
    require(lockedBefore.ok && lockedAfter.ok && lockedBefore.denoiseStretchLocked && lockedAfter.denoiseStretchLocked);
    require(lockedBefore.development.sky == lockedAfter.development.sky);
    require(lockedBefore.development.gains == lockedAfter.development.gains);
    require(lockedBefore.development.toneScale == lockedAfter.development.toneScale);
    require(lockedBefore.development.whitePoint == lockedAfter.development.whitePoint);
    require(lockedBefore.development.shadowNoise == lockedAfter.development.shadowNoise);
    require(fileSHA256(lockedBefore.backgroundMaster) == fileSHA256(lockedAfter.backgroundMaster));
    const auto lockedA = ImageCodec().read(lockedBefore.developed), lockedB = ImageCodec().read(lockedAfter.developed);
    require(lockedA.ok && lockedB.ok && lockedA.image.pixels != lockedB.image.pixels);
    for(unsigned y=60;y<=68;++y)for(unsigned x=39;x<=47;++x)for(unsigned c=0;c<4;++c) {
        const auto p=(std::size_t(y)*lockedA.image.width+x)*4+c;
        require(lockedA.image.pixels[p] == lockedB.image.pixels[p]);
    }
    // All enabled denoisers use the same earlier statistics, including NLM.
    lockedOptions.denoiseH = 2;
    lockedOptions.multiscale.luminance = .9;
    lockedOptions.multiscale.chroma = 1;
    const auto lockedAll = DeepSkyWorkflow().finishMaster(result.master, root / "locked-all", lockedOptions);
    require(lockedAll.ok && lockedAll.denoiseStretchLocked);
    require(lockedAll.development.sky == lockedBefore.development.sky);
    require(lockedAll.development.toneScale == lockedBefore.development.toneScale);
    require(lockedAll.development.shadowNoise == lockedBefore.development.shadowNoise);
    const auto lockedRun = DeepSkyWorkflow().run(inputs, root / "locked-run", lockedOptions);
    require(lockedRun.ok && lockedRun.denoiseStretchLocked && !lockedRun.masterReused);
    require(fileSHA256(lockedRun.developed) == fileSHA256(lockedAll.developed));
    // A successful development followed by a failed export is not a finished
    // image, and must not advertise verified finishing provenance to the App.
    for (bool reuse : {false, true}) {
        const auto directory = root / (reuse ? "locked-export-failure-reuse" : "locked-export-failure-run");
        auto failExport = [&](const std::string& stage, double, std::size_t, std::size_t) {
            if (stage == "develop") fs::create_directory(directory / "developed.tiff");
        };
        const auto failed = reuse
            ? DeepSkyWorkflow().finishMaster(result.master, directory, lockedOptions, failExport)
            : DeepSkyWorkflow().run(inputs, directory, lockedOptions, failExport);
        require(!failed.ok && failed.development.ok && !failed.denoiseStretchLocked);
    }
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
    independentOptions.stack.method = StackMethod::SigmaClip;
    independentOptions.stack.sigma.medianMad = true;
    independentOptions.noiseModel = DeepSkyNoiseModel::IndependentLuminance;
    independentOptions.lockDenoiseStretch = true;
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
    require(independent.denoiseStretchLocked && replay.denoiseStretchLocked);
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
    require(analysis.referenceIndex == result.referenceIndex && analysis.selectedCount == result.selectedCount);
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto& analyzed = analysis.frames[i];
        const auto& stacked = result.frames[i];
        require(analyzed.native.luminance.capacity() == 0 && analyzed.native.coverage.capacity() == 0);
        require(analyzed.registration.ok && analyzed.alignedCoverage == stacked.alignedCoverage);
        require(analyzed.assessment.comparedSamples == stacked.assessment.comparedSamples);
        require(analyzed.assessment.residualRms == stacked.assessment.residualRms);
        require(analyzed.assessment.positiveFraction == stacked.assessment.positiveFraction);
        require(analyzed.assessment.negativeFraction == stacked.assessment.negativeFraction);
        require(analyzed.assessment.reasons == stacked.assessment.reasons &&
                analyzed.assessment.warnings == stacked.assessment.warnings);
    }
    // Reusing native grids must still report an unusable light, and must
    // fail if a previously measured input is replaced before assessment.
    {
        auto partial = inputs;
        partial.back() = root / "invalid.fits";
        std::ofstream(partial.back()) << "bad FITS";
        const auto invalid = DeepSkyWorkflow().run(partial, root / "one-invalid", options);
        require(invalid.ok && invalid.selectedCount == partial.size() - 1);
        require(!invalid.frames.back().native.ok && !invalid.frames.back().assessment.usable &&
                !invalid.frames.back().errorCode.empty());
        partial.back() = root / "changed.fits";
        fs::copy_file(inputs.back(), partial.back());
        const auto changed = DeepSkyWorkflow().run(partial, root / "changed-after-measure", options,
            [&](const std::string& stage, double, std::size_t step, std::size_t) {
                if (stage == "align" && step == 1)
                    std::ofstream(partial.back(), std::ios::app) << "source changed";
            });
        require(!changed.ok && changed.errorCode == "InputChanged");
        require(!fs::exists(root / "changed-after-measure/.aligned-work"));
#ifndef _WIN32
        partial.back() = root / "linked.fits";
        fs::create_symlink(inputs.back(), partial.back());
        const auto retargeted = DeepSkyWorkflow().run(partial, root / "retargeted", options,
            [&](const std::string& stage, double, std::size_t step, std::size_t) {
                if (stage == "align" && step == 1) {
                    fs::remove(partial.back());
                    fs::create_symlink(inputs.front(), partial.back());
                }
            });
        require(!retargeted.ok && retargeted.errorCode == "InputChanged");
#endif
    }
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
