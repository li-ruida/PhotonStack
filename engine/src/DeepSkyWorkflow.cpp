#include "photonstack/DeepSkyWorkflow.hpp"
#include "photonstack/BackgroundDenoiser.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/FileDigest.hpp"
#include "photonstack/SensorPatternBuilder.hpp"
#include "photonstack/DeepSkyRecipe.hpp"
#include "DeepSkyNoiseReference.hpp"
#include <fstream>
#include <iomanip>

#include "photonstack/ImageCodec.hpp"
#include "photonstack/NonlocalDenoiser.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>

namespace photonstack {
namespace fs = std::filesystem;
namespace {
struct WorkflowFailure : std::runtime_error {
    std::string code;
    WorkflowFailure(std::string code, std::string message) : std::runtime_error(message), code(std::move(code)) {}
};
void ensure(bool ok, const std::string& code, const std::string& message) {
    if (!ok) throw WorkflowFailure(code, message);
}
struct OwnedWorkDirectory {
    fs::path path;
    ~OwnedWorkDirectory() { if (!path.empty()) { std::error_code e; fs::remove_all(path, e); } }
};
void checkIdentity(const DeepSkyFrameReport& frame) {
    ensure(fs::is_regular_file(frame.input) && fs::file_size(frame.input) == frame.bytes &&
        fs::last_write_time(frame.input).time_since_epoch().count() == frame.modifiedTicks,
        "InputChanged", "Source changed during processing: " + frame.input.string());
}
void write(const ImageBuffer& image, const fs::path& output) {
    ImageWriteOptions options;
    options.bitDepth = ImageWriteBitDepth::Sixteen;
    const auto r = ImageCodec().write(image, output, options);
    ensure(r.ok, r.errorCode, r.message);
}
struct Rectangle { std::uint32_t x = 0, y = 0, w = 0, h = 0; };
Rectangle coverageRectangle(const ImageBuffer& image, double threshold) {
    if (threshold <= 0) return {0, 0, image.width, image.height};
    Rectangle best;
    std::vector<std::uint32_t> heights(image.width + 1, 0), positions;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto p = (std::size_t(y) * image.width + x) * image.channels;
            bool valid = image.channels != 4 || (std::isfinite(image.pixels[p + 3]) && image.pixels[p + 3] >= threshold);
            for (std::uint16_t c = 0; c < std::min<std::uint16_t>(image.channels, 3); ++c)
                valid = valid && std::isfinite(image.pixels[p + c]);
            heights[x] = valid ? heights[x] + 1 : 0;
        }
        positions.clear();
        for (std::uint32_t x = 0; x <= image.width; ++x) {
            while (!positions.empty() && heights[positions.back()] > heights[x]) {
                const auto height = heights[positions.back()]; positions.pop_back();
                const auto left = positions.empty() ? 0 : positions.back() + 1;
                const auto width = x - left;
                if (std::uint64_t(width) * height > std::uint64_t(best.w) * best.h)
                    best = {left, y + 1 - height, width, height};
            }
            positions.push_back(x);
        }
    }
    return best;
}
ImageBuffer crop(const ImageBuffer& image, const Rectangle& r) {
    ImageBuffer out;
    out.width = r.w; out.height = r.h; out.channels = image.channels;
    out.format = image.format; out.colorEncoding = image.colorEncoding;
    out.sourceBitsPerChannel = image.sourceBitsPerChannel;
    out.pixels.resize(out.sampleCount());
    for (std::uint32_t y = 0; y < r.h; ++y) {
        const auto source = (std::size_t(y + r.y) * image.width + r.x) * image.channels;
        std::copy_n(image.pixels.begin() + source, std::size_t(r.w) * image.channels,
            out.pixels.begin() + std::size_t(y) * r.w * image.channels);
    }
    return out;
}

ImageBuffer denoiseMap(const ImageBuffer& image, double noise, double amount) {
    const auto w = image.width, h = image.height;
    std::vector<double> summed(std::size_t(w + 1) * (h + 1), 0);
    std::vector<float> residual(image.pixelCount(), 0);
    const auto luma = [&](std::size_t p) {
        return .2126 * image.pixels[p * image.channels] + .7152 * image.pixels[p * image.channels + 1] +
            .0722 * image.pixels[p * image.channels + 2];
    };
    for (std::uint32_t y = 0; y < h; ++y) {
        double row = 0;
        for (std::uint32_t x = 0; x < w; ++x) {
            row += luma(std::size_t(y) * w + x);
            summed[std::size_t(y + 1) * (w + 1) + x + 1] = row + summed[std::size_t(y) * (w + 1) + x + 1];
        }
    }
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto x0 = x > 2 ? x - 2 : 0, y0 = y > 2 ? y - 2 : 0;
            const auto x1 = std::min(w, x + 3), y1 = std::min(h, y + 3);
            const double sum = summed[std::size_t(y1) * (w + 1) + x1] - summed[std::size_t(y0) * (w + 1) + x1] -
                summed[std::size_t(y1) * (w + 1) + x0] + summed[std::size_t(y0) * (w + 1) + x0];
            residual[std::size_t(y) * w + x] = float(std::max(0.0, luma(std::size_t(y) * w + x) - sum / ((x1 - x0) * (y1 - y0))));
        }
    ImageBuffer blend;
    blend.width = w; blend.height = h; blend.channels = 1; blend.format = PixelFormat::Float32Gray;
    blend.pixels.resize(blend.sampleCount());
    const double start = std::max(1.e-6, 5 * noise), span = std::max(1.e-6, 12.5 * noise);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            double peak = 0;
            for (std::uint32_t yy = y > 3 ? y - 3 : 0; yy < std::min(h, y + 4); ++yy)
                for (std::uint32_t xx = x > 3 ? x - 3 : 0; xx < std::min(w, x + 4); ++xx)
                    peak = std::max(peak, double(residual[std::size_t(yy) * w + xx]));
            blend.pixels[std::size_t(y) * w + x] = float(amount * (1 - std::clamp((peak - start) / span, 0.0, 1.0)));
        }
    return blend;
}
void finishImage(ImageBuffer stacked, const fs::path& directory, const DeepSkyWorkflowOptions& o,
                 DeepSkyWorkflowResult& result, const std::function<void(const std::string&, double)>& emit,
                 ImageBuffer noise = {}) {
        const bool independent = o.noiseModel==DeepSkyNoiseModel::IndependentLuminance;
        ensure(o.noiseModel==DeepSkyNoiseModel::Scene || independent, "ArgumentInvalid", "Invalid luminance noise model");
        ensure(!independent || (o.denoiseH==0 && !noise.empty() && noise.width==stacked.width && noise.height==stacked.height),
               "NoiseReferenceInvalid", "Independent luminance noise requires a matching stack diagnostic and NLM disabled");
        ensure(std::isfinite(o.denoiseH) && (o.denoiseH == -1 || o.denoiseH >= 0) &&
               std::isfinite(o.denoiseBlend) && o.denoiseBlend >= 0 && o.denoiseBlend <= 1 &&
               std::isfinite(o.multiscale.luminance) && o.multiscale.luminance >= 0 && o.multiscale.luminance <= 1 &&
               std::isfinite(o.multiscale.chroma) && o.multiscale.chroma >= 0 && o.multiscale.chroma <= 1 &&
               o.multiscale.scales >= 1 && o.multiscale.scales <= 6 &&
               std::isfinite(o.backgroundDenoiseStrength) && o.backgroundDenoiseStrength >= 0 && o.backgroundDenoiseStrength <= 1,
               "ArgumentInvalid", "Invalid denoise options");
        emit("crop", .86);
        auto rectangle = coverageRectangle(stacked, o.cropCoverage);
        ensure(rectangle.w > 2ULL * o.cropInset + 15 && rectangle.h > 2ULL * o.cropInset + 15,
            "CoverageInsufficient", "No sufficiently large common-coverage rectangle; review registration and crop settings");
        rectangle.x += o.cropInset; rectangle.y += o.cropInset;
        rectangle.w -= 2 * o.cropInset; rectangle.h -= 2 * o.cropInset;
        result.cropX = rectangle.x; result.cropY = rectangle.y; result.cropWidth = rectangle.w; result.cropHeight = rectangle.h;
        auto image = crop(stacked, rectangle);
        if(independent) noise=crop(noise,rectangle);
        stacked = {};
        if (o.alignChannels) {
            emit("align-color-channels", .87);
            result.channelAlignment = ChannelAlignment().apply(image);
            ensure(result.channelAlignment.ok, result.channelAlignment.errorCode, result.channelAlignment.message);
            if(independent) {
                auto transformed=ChannelAlignment().applyMeasured(noise,result.channelAlignment);
                ensure(transformed.ok,transformed.errorCode,transformed.message);
                noise=std::move(transformed.image);
            }
            image = std::move(result.channelAlignment.image);
        }
        emit("background", .88);
        if (o.background == DeepSkyBackground::Polynomial || o.background == DeepSkyBackground::PolynomialAndGrid) {
            auto options = o.polynomial;
            for (auto& e : options.exclusions) { e.x -= rectangle.x; e.y -= rectangle.y; }
            auto background = BackgroundExtractor().extractPolynomial(image, options);
            ensure(background.ok, background.errorCode, background.message); image = std::move(background.image);
        }
        if (o.background == DeepSkyBackground::Grid || o.background == DeepSkyBackground::PolynomialAndGrid) {
            auto options = o.grid;
            options.exclusions = o.polynomial.exclusions;
            for (auto& e : options.exclusions) { e.x -= rectangle.x; e.y -= rectangle.y; }
            auto background = BackgroundExtractor().extractGrid(image, options);
            ensure(background.ok, background.errorCode, background.message); image = std::move(background.image);
        }
        result.backgroundMaster = directory / "stack-background.fits"; write(image, result.backgroundMaster);
        if (o.denoiseH != 0) {
            emit("denoise", .91);
            auto q = o.quality; q.measureStars = false;
            const auto measured = SequenceQualityAnalyzer().sample(image, q);
            ensure(measured.ok, measured.errorCode, measured.message);
            result.denoiseH = o.denoiseH < 0 ? std::max(1.e-6, measured.noise * 3) : o.denoiseH;
            auto blend = denoiseMap(image, measured.noise, o.denoiseBlend);
            auto denoised = NonlocalDenoiser().apply(image, blend, {.h = result.denoiseH});
            ensure(denoised.ok, denoised.errorCode, denoised.message); image = std::move(denoised.image);
            if (o.saveDenoiseStages) write(image, directory / "stack-denoised.fits");
        }
        if (o.multiscale.luminance > 0 || o.multiscale.chroma > 0) {
            emit("multiscale-denoise", .94);
            auto q = o.quality; q.measureStars = false;
            const auto measured = SequenceQualityAnalyzer().sample(image, q);
            ensure(measured.ok, measured.errorCode, measured.message);
            auto blend = denoiseMap(image, measured.noise, 1);
            auto multiscale=o.multiscale;
            if(independent) multiscale.noiseReferenceScope=MultiscaleNoiseReferenceScope::LuminanceOnly;
            // The fitted additive background is held fixed for this conditional
            // noise estimate. Subtracting the scene's model from a zero-signal
            // diagnostic would introduce false structure. Background-fit error,
            // shared patterns and PSF leakage are not modeled by this reference.
            auto reduced = MultiscaleDenoiser().apply(image, blend, multiscale, independent?&noise:nullptr);
            ensure(reduced.ok, reduced.errorCode, reduced.message); image = std::move(reduced.image);
            result.independentLuminanceNoise=independent && o.multiscale.luminance>0;
            if (o.saveDenoiseStages) write(image, directory / "stack-multiscale.fits");
        }
        if (o.backgroundDenoiseStrength > 0) {
            emit("background-denoise", .945);
            auto q = o.quality; q.measureStars = false;
            const auto measured = SequenceQualityAnalyzer().sample(image, q);
            ensure(measured.ok, measured.errorCode, measured.message);
            auto blend = denoiseMap(image, measured.noise, 1);
            // The same explicit target regions used for background extraction
            // retain their pixels exactly, with a smooth transition outside.
            for (const auto& e : o.polynomial.exclusions) {
                ensure(std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.major) &&
                    std::isfinite(e.minor) && std::isfinite(e.angleDegrees) && e.major > 0 && e.minor > 0,
                    "ArgumentInvalid", "Invalid background denoise target region");
                const double a = e.angleDegrees * 3.14159265358979323846 / 180;
                const double ca = std::cos(a), sa = std::sin(a);
                for (unsigned y=0;y<image.height;++y) for (unsigned x=0;x<image.width;++x) {
                    const double dx = x + double(rectangle.x) - e.x, dy = y + double(rectangle.y) - e.y;
                    const double radius = std::hypot((ca*dx+sa*dy)/e.major, (-sa*dx+ca*dy)/e.minor);
                    const double t = std::clamp((radius-1)*5, 0., 1.);
                    blend.pixels[std::size_t(y)*image.width+x] *= float(t*t*(3-2*t));
                }
            }
            auto reduced = BackgroundDenoiser().apply(image, blend, o.backgroundDenoiseStrength);
            ensure(reduced.ok, reduced.errorCode, reduced.message); image = std::move(reduced.image);
            if (o.saveDenoiseStages) write(image, directory / "stack-background-denoised.fits");
        }
        emit("develop", .95);
        result.development = AstroDevelop().apply(image, o.develop);
        ensure(result.development.ok, result.development.errorCode, result.development.message);
        result.developed = directory / "developed.tiff"; write(result.development.image, result.developed);
        result.development.image = {};
}
} // namespace

DeepSkyWorkflowOptions::DeepSkyWorkflowOptions() {
    stack.method = StackMethod::SigmaClip;
    stack.sigma = {.sigmaLow = 3, .sigmaHigh = 3, .medianMad = true, .iterations = 3};
    stack.alignSimilarity = true;
    stack.fitsDemosaic = FitsDemosaic::Malvar;
    stack.interpolation = RegistrationInterpolation::Bicubic;
    stack.normalizeBackground = true;
    stack.registration.minimumMatches = 30;
    stack.registration.similarityFallbackToTranslation = false;
    stack.collectRejectionMaps = true;
    quality.applySelection = true;
    polynomial.extraction.preserveBrightness = false;
    polynomial.extraction.clampOutput = false;
    grid.extraction.preserveBrightness = false;
    grid.extraction.clampOutput = false;
    grid.columns = 6; grid.rows = 8;
    grid.extrapolateEdges = true;
    multiscale.luminance = .6; multiscale.chroma = .8;
    develop.toneCurve = AstroToneCurve::Asinh;
    develop.shadowNeutralization = .8F;
    develop.background = .018F; develop.brightness = 1; develop.saturation = .8F;
}

DeepSkyWorkflowResult DeepSkyWorkflow::run(const std::vector<fs::path>& inputs, const fs::path& directory,
                                         const DeepSkyWorkflowOptions& o, const DeepSkyProgress& progress) const {
    DeepSkyWorkflowResult result;
    OwnedWorkDirectory work;
    double lastFraction = 0;
    const auto emit = [&](const std::string& stage, double fraction, std::size_t step = 0, std::size_t total = 0) {
        lastFraction = std::clamp(fraction, lastFraction, 1.);
        if (progress) progress(stage, lastFraction, step, total ? total : inputs.size());
    };
    try {
        const bool independentNoise=o.noiseModel==DeepSkyNoiseModel::IndependentLuminance && !o.analysisOnly;
        ensure(o.noiseModel==DeepSkyNoiseModel::Scene || o.noiseModel==DeepSkyNoiseModel::IndependentLuminance,
               "ArgumentInvalid", "Invalid luminance noise model");
        ensure(!independentNoise || (o.denoiseH==0 && o.stack.method==StackMethod::SigmaClip),
               "NoiseReferenceUnsupported", "Independent luminance estimation requires sigma stacking and NLM disabled");
        ensure(inputs.size() >= 3 && inputs.size() <= 2000, "ArgumentInvalid", "Deep-sky workflow requires 3 to 2000 lights");
        ensure(!directory.empty() && (!fs::exists(directory) || (fs::is_directory(directory) && fs::is_empty(directory))),
            "OutputExists", "Output directory must be new or empty");
        ensure(o.referenceIndex >= -1 && o.referenceIndex < int(inputs.size()) &&
            std::isfinite(o.cropCoverage) && o.cropCoverage >= 0 && o.cropCoverage <= 1 &&
            std::isfinite(o.denoiseH) && (o.denoiseH == -1 || o.denoiseH >= 0) &&
            std::isfinite(o.denoiseBlend) && o.denoiseBlend >= 0 && o.denoiseBlend <= 1,
            "ArgumentInvalid", "Invalid deep-sky workflow options");
        ensure(o.stack.alignmentReference.empty(), "ArgumentInvalid", "Select the reference by input index in this workflow");
        ensure(!o.stack.collectNoiseDiagnostic && o.stack.frameNoiseVariances.empty(), "ArgumentInvalid",
               "Use the workflow luminance noise model instead of supplying low-level diagnostic options");
        ensure(!o.sensorPattern || (o.darks.empty() && o.biases.empty() && o.flats.empty() && o.stack.debayerFits),
            "SensorPatternConflict", "Sensor pattern correction requires uncalibrated CFA FITS with debayer enabled");
        fs::create_directories(directory);
        const auto workPath = directory / ".aligned-work";
        ensure(fs::create_directory(workPath), "OutputExists", "Work directory already exists");
        work.path = workPath;
        std::set<fs::path> unique;
        std::set<std::string> sourceDigests;
        FitsSensorIdentity sensorIdentity;
        result.frames.resize(inputs.size());
        ImageReadOptions readOptions;
        readOptions.raw = o.stack.raw; readOptions.raw.linearOutput = true;
        readOptions.fits.mode = FitsDecodeMode::Scientific;
        readOptions.fits.maskNonFinitePixels = true;
        readOptions.fits.debayer = o.stack.debayerFits;
        readOptions.fits.demosaic = o.stack.fitsDemosaic;
        readOptions.fits.cfaInterpolationGains = o.stack.cfaInterpolationGains;
        const ImageCodec codec;
        ImageBuffer masterDark, masterBias, masterFlat;
        std::vector<DeepSkyFrameReport> calibrationIdentities;
        CalibrationOptions calibration;
        calibration.clampNegativeValues = false;
        calibration.darkBiasState = o.darkBiasState;
        calibration.flatBiasState = o.flatBiasState;
        auto masterOptions = MasterFrameOptions{};
        masterOptions.method = o.masterMethod; masterOptions.raw = readOptions.raw;
        masterOptions.temporaryDirectory = work.path;
        const auto makeMaster = [&](const std::vector<fs::path>& paths, ImageBuffer& image, const char* name) {
            if (paths.empty()) return;
            for (const auto& path : paths) {
                DeepSkyFrameReport identity;
                identity.input = fs::canonical(path);
                ensure(unique.insert(identity.input).second, "DuplicateInput", "A calibration frame was supplied more than once");
                identity.bytes = fs::file_size(identity.input);
                identity.modifiedTicks = fs::last_write_time(identity.input).time_since_epoch().count();
                if(independentNoise) identity.sha256=fileSHA256(identity.input);
                calibrationIdentities.push_back(std::move(identity));
            }
            emit(name, 0);
            auto built = MasterFrameBuilder().build(paths, masterOptions);
            ensure(built.ok, built.errorCode, built.message);
            image = std::move(built.image);
            write(image, directory / (std::string(name) + ".fits"));
        };
        makeMaster(o.darks, masterDark, "master-dark");
        makeMaster(o.biases, masterBias, "master-bias");
        makeMaster(o.flats, masterFlat, "master-flat");
        calibration.dark = masterDark.empty() ? nullptr : &masterDark;
        calibration.bias = masterBias.empty() ? nullptr : &masterBias;
        calibration.flat = masterFlat.empty() ? nullptr : &masterFlat;
        const bool calibrated = calibration.dark || calibration.bias || calibration.flat;
        const auto readLight = [&](const fs::path& path) {
            if (!calibrated) return codec.read(path, readOptions);
            if (isFitsPath(path)) return FitsCodec().read(path, readOptions.fits, &calibration);
            auto read = codec.read(path, readOptions);
            if (!read.ok) return read;
            auto corrected = Calibrator().calibrate(read.image, calibration);
            read.ok = corrected.ok; read.errorCode = corrected.errorCode; read.message = corrected.message;
            read.image = std::move(corrected.image);
            return read;
        };
        const SequenceQualityAnalyzer analyzer;
        std::size_t reference = 0;
        double best = -1;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            auto& f = result.frames[i];
            f.input = fs::canonical(inputs[i]);
            ensure(unique.insert(f.input).second, "DuplicateInput", "A light was supplied more than once");
            ensure(fs::is_regular_file(f.input), "InputInvalid", "Expected a regular image file");
            f.bytes = fs::file_size(f.input); f.modifiedTicks = fs::last_write_time(f.input).time_since_epoch().count();
            if (o.sensorPattern) {
                const auto sensor = FitsCodec().inspectSensor(f.input);
                ensure(sensor.ok, sensor.errorCode, sensor.message);
                if (i == 0) sensorIdentity = sensor;
                ensure(sensor.sameAcquisition(sensorIdentity), "SensorPatternConflict", "All sensor acquisition settings must match");
            }
            if(o.sensorPattern || independentNoise) {
                f.sha256=fileSHA256(f.input);
                ensure(sourceDigests.insert(f.sha256).second,"DuplicateInput","Identical source bytes cannot be independent samples");
                checkIdentity(f);
            }
            emit("measure", .20 * i / inputs.size(), i + 1);
            const auto decoded = readLight(f.input);
            checkIdentity(f);
            if (!decoded.ok) { f.errorCode = decoded.errorCode; f.message = decoded.message; continue; }
            f.native = analyzer.sample(decoded.image, o.quality);
            if (!f.native.ok) { f.errorCode = f.native.errorCode; f.message = f.native.message; continue; }
            // Missing shape measurements must not win by looking like a zero-
            // width star. Keep a coverage-only fallback when all are unknown.
            const double score = f.native.medianFwhm > 0
                ? 1 + f.native.validFraction * std::sqrt(double(f.native.starCount) + 1) *
                    (1 - .5 * f.native.medianEccentricity) / std::max(1.0, f.native.medianFwhm * f.native.medianFwhm)
                : f.native.validFraction;
            if (score > best) { best = score; reference = i; }
        }
        ensure(best >= 0, "NoUsableFrames", "None of the inputs has usable scientific samples");
        if (o.referenceIndex >= 0) reference = std::size_t(o.referenceIndex);
        ensure(result.frames[reference].native.ok, "ReferenceInvalid", "Selected reference is not usable");
        result.referenceIndex = reference; result.frames[reference].reference = true;
        auto ref = readLight(result.frames[reference].input);
        ensure(ref.ok, ref.errorCode, ref.message);
        checkIdentity(result.frames[reference]);
        const Registration registration(o.stack.interpolation);
        std::vector<SequenceFrameSample> samples(inputs.size());
        std::vector<double> registeredVariances(inputs.size(),0);
        std::vector<fs::path> aligned(inputs.size());
        std::vector<DistortionTransform> distortions(inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            auto& f = result.frames[i];
            if (!f.native.ok) { samples[i].errorCode = f.errorCode; continue; }
            emit("align", .20 + .35 * i / inputs.size(), i + 1);
            checkIdentity(f);
            auto decoded = readLight(f.input);
            ensure(decoded.ok, decoded.errorCode, decoded.message);
            ImageBuffer image;
            const bool needsAlignment = i != reference && (o.stack.alignTranslation || o.stack.alignSimilarity || o.stack.alignAffine || o.stack.alignDistortion);
            if (needsAlignment) {
                auto& distortion = distortions[i];
                if (o.stack.alignDistortion) f.registration = registration.estimateDistortion(ref.image, decoded.image, distortion, o.stack.registration);
                else if (o.stack.alignAffine) f.registration = registration.estimateAffine(ref.image, decoded.image, o.stack.registration);
                else if (o.stack.alignSimilarity) f.registration = registration.estimateSimilarity(ref.image, decoded.image, o.stack.registration);
                else f.registration = registration.estimateTranslation(ref.image, decoded.image, o.stack.registration);
                if (!f.registration.ok || f.registration.usedFallback || f.registration.matches < o.stack.registration.minimumMatches) {
                    f.errorCode = "RegistrationRejected"; f.message = f.registration.message.empty() ? "Registration lacks reliable star matches" : f.registration.message;
                    samples[i].errorCode = f.errorCode; continue;
                }
                if (o.stack.alignDistortion) image = registration.applyDistortion(decoded.image, distortion);
                else if (o.stack.alignAffine) image = registration.applyAffine(decoded.image, f.registration.affine);
                else if (o.stack.alignSimilarity) image = registration.applySimilarity(decoded.image, f.registration.transform);
                else image = registration.applyTranslation(decoded.image, f.registration.translation);
            } else { f.registration.ok = true; image = std::move(decoded.image); }
            checkIdentity(f);
            auto sampleOptions = o.quality; sampleOptions.measureStars = false;
            samples[i] = analyzer.sample(image, sampleOptions);
            if(independentNoise) registeredVariances[i]=samples[i].noise*samples[i].noise;
            f.alignedCoverage = samples[i].validFraction;
            // Native PSF/noise metrics remain comparable before interpolation.
            samples[i].noise = f.native.noise;
            samples[i].starsMeasured = f.native.starsMeasured;
            samples[i].starShapeCount = f.native.starShapeCount;
            samples[i].starCount = f.native.starCount;
            samples[i].medianFwhm = f.native.medianFwhm;
            samples[i].medianEccentricity = f.native.medianEccentricity;
            if (!samples[i].ok) { f.errorCode = samples[i].errorCode; f.message = samples[i].message; continue; }
            if (!o.analysisOnly && !o.sensorPattern) {
                aligned[i] = work.path / (std::to_string(i) + ".fits");
                write(image, aligned[i]);
            }
        }
        ref.image.pixels.clear(); ref.image.pixels.shrink_to_fit();
        emit("assess", .56);
        const auto assessment = analyzer.assess(samples, o.quality);
        for (const auto& frame : calibrationIdentities) checkIdentity(frame);
        result.selectedCount = assessment.selectedCount;
        for (std::size_t i = 0; i < assessment.frames.size(); ++i) result.frames[i].assessment = assessment.frames[i];
        ensure(assessment.ok, assessment.errorCode, assessment.message);
        if (o.analysisOnly) {
            for (const auto& frame : result.frames) checkIdentity(frame);
            result.ok = true; emit("complete", 1); return result;
        }
        if (o.sensorPattern) {
            std::vector<std::size_t> usable;
            for (std::size_t i = 0; i < inputs.size(); ++i)
                if (result.frames[i].assessment.selected) usable.push_back(i);
            ensure(usable.size() >= 36, "SensorPatternInsufficient", "Sensor pattern correction needs at least 36 selected lights");
            const auto count = std::min<std::size_t>(60, usable.size());
            SensorPatternOptions patternOptions;
            patternOptions.temporaryDirectory = work.path;
            patternOptions.progress = [&](double p) { emit("sensor-pattern-estimate", .56 + .08 * p); };
            std::vector<fs::path> training;
            std::vector<std::size_t> trainingIndices;
            for (std::size_t slot = 0; slot < count; ++slot) {
                const auto i = usable[slot * (usable.size() - 1) / (count - 1)];
                const auto& f = result.frames[i];
                training.push_back(f.input); trainingIndices.push_back(i);
                // Global motion is an eligibility check, not scene-leakage proof.
                // Distortion models retain their exact warp for the replay below.
                auto transform = o.stack.alignDistortion ? distortions[i].global : f.registration.transform;
                double a=1, b=0, c=0, d=1, dx=0, dy=0;
                if (i != reference) {
                    if (o.stack.alignAffine && !o.stack.alignDistortion) {
                        const auto t = f.registration.affine;
                        a=t.a; b=t.b; c=t.c; d=t.d; dx=t.dx; dy=t.dy;
                    } else if (o.stack.alignSimilarity || o.stack.alignDistortion) {
                        a=d=transform.scale*std::cos(transform.rotationRadians);
                        c=transform.scale*std::sin(transform.rotationRadians); b=-c;
                        dx=transform.dx; dy=transform.dy;
                    } else if (o.stack.alignTranslation) { dx=f.registration.translation.dx; dy=f.registration.translation.dy; }
                }
                const double determinant=a*d-b*c;
                ensure(std::isfinite(determinant) && std::abs(determinant)>1.e-12,
                    "SensorPatternGeometry", "Cannot invert field motion for sensor estimation");
                const double x=sensorIdentity.width*.5-dx, y=sensorIdentity.height*.5-dy;
                patternOptions.sensorPositions.push_back({(d*x-b*y)/determinant, (a*y-c*x)/determinant});
            }
            for (const auto i : trainingIndices) {
                checkIdentity(result.frames[i]);
                ensure(fileSHA256(result.frames[i].input)==result.frames[i].sha256, "InputChanged", "Sensor training source bytes changed");
            }
            auto patterns = SensorPatternBuilder().build(training, patternOptions);
            ensure(patterns.ok, patterns.errorCode, patterns.message);
            // This is a run-owned model. Persist provenance, never load an
            // unrelated on-disk model by matching just camera/size/mtime.
            const auto reportPath = directory / "sensor-pattern.txt";
            std::ofstream provenance(reportPath);
            provenance << std::setprecision(17) << "PHOTONSTACK_SENSOR_PATTERN 1\n"
                << "algorithm cfa-highpass-crossfold-v2-soft-confidence\nminimumGroupSamples 12\nborder 24\nminimumMotion 8\nminimumCorrelation 0.2\n"
                << "camera " << std::quoted(sensorIdentity.camera) << "\nexposure " << sensorIdentity.exposure
                << "\ngain " << sensorIdentity.gain << "\nbayer " << sensorIdentity.bayer << "\n";
            for (std::size_t slot = 0; slot < count; ++slot) {
                const auto i=trainingIndices[slot];
                result.frames[i].sensorPatternModel = int(slot%3);
                provenance << "training " << slot << ' ' << i << ' ' << slot%3 << ' '
                    << result.frames[i].sha256 << ' ' << std::quoted(result.frames[i].input.string()) << ' '
                    << patternOptions.sensorPositions[slot][0] << ' ' << patternOptions.sensorPositions[slot][1] << "\n";
            }
            for (unsigned fold=0; fold<3; ++fold) {
                const auto modelPath=work.path / "sensor-model.fits";
                write(patterns.models[fold].correction, modelPath);
                provenance << "model " << fold << ' ' << fileSHA256(modelPath) << "\n";
                fs::remove(modelPath);
                for (unsigned c=0; c<4; ++c) provenance << "parity " << fold << ' ' << c << ' '
                    << patterns.models[fold].retention[c] << ' ' << patterns.models[fold].correlation[c] << "\n";
            }
            provenance.close(); ensure(bool(provenance), "WriteFailed", "Cannot write sensor provenance");
            result.sensorPatternReport = reportPath;
            for (std::size_t slot=0; slot<usable.size(); ++slot) {
                const auto i=usable[slot]; auto& f=result.frames[i];
                checkIdentity(f);
                ensure(fileSHA256(f.input)==f.sha256, "InputChanged", "Source bytes changed before sensor correction");
                if (f.sensorPatternModel < 0) f.sensorPatternModel = int(slot%3);
                const auto& model=patterns.models[std::size_t(f.sensorPatternModel)].correction;
                auto decoded=FitsCodec().read(f.input, readOptions.fits, nullptr, &model);
                ensure(decoded.ok, decoded.errorCode, decoded.message);
                ImageBuffer image;
                if (i == reference) image=std::move(decoded.image);
                else if (o.stack.alignDistortion) image=registration.applyDistortion(decoded.image, distortions[i]);
                else if (o.stack.alignAffine) image=registration.applyAffine(decoded.image, f.registration.affine);
                else if (o.stack.alignSimilarity) image=registration.applySimilarity(decoded.image, f.registration.transform);
                else if (o.stack.alignTranslation) image=registration.applyTranslation(decoded.image, f.registration.translation);
                else image=std::move(decoded.image);
                checkIdentity(f);
                if(independentNoise) {
                    auto q=o.quality;q.measureStars=false;q.measureTrails=false;
                    const auto measured=analyzer.sample(image,q);
                    ensure(measured.ok,measured.errorCode,measured.message);
                    registeredVariances[i]=measured.noise*measured.noise;
                }
                aligned[i]=work.path/(std::to_string(i)+".fits"); write(image, aligned[i]);
                emit("sensor-pattern-apply", .64 + .06*(slot+1)/usable.size(), slot+1, usable.size());
            }
        }
        std::vector<fs::path> selected;
        std::vector<std::size_t> selectedIndices;
        auto stackOptions = o.stack;
        stackOptions.temporaryDirectory = work.path;
        stackOptions.alignTranslation = stackOptions.alignSimilarity = stackOptions.alignAffine = stackOptions.alignDistortion = false;
        stackOptions.registration.refineSimilarityCentroids = false;
        stackOptions.alignmentReference.clear(); stackOptions.frameWeights.clear();
        for (std::size_t i = 0; i < inputs.size(); ++i) if (result.frames[i].assessment.selected) {
            ensure(!aligned[i].empty(), "SelectionInvalid", "A selected frame has no valid registration output");
            selected.push_back(aligned[i]); selectedIndices.push_back(i);
            if (!o.stack.frameWeights.empty()) {
                ensure(o.stack.frameWeights.size() == inputs.size(), "ArgumentInvalid", "One weight is required per input");
                stackOptions.frameWeights.push_back(o.stack.frameWeights[i]);
            }
        }
        ensure(selected.size() >= 3, "SelectionNeedsReview", "At least three usable lights are required");
        if(independentNoise) {
            ensure(selected.size()>=6,"NoiseReferenceUnsupported","Independent luminance estimation requires at least six selected exposures");
            stackOptions.collectNoiseDiagnostic=true;
            for(const auto i:selectedIndices) {
                ensure(std::isfinite(registeredVariances[i]) && registeredVariances[i]>0,
                       "NoiseReferenceInvalid","Registered exposure noise could not be estimated");
                stackOptions.frameNoiseVariances.push_back(registeredVariances[i]);
            }
        }
        const auto releaseAligned = [&](std::size_t sourceIndex) {
            const auto& path = aligned.at(sourceIndex);
            ensure(path.parent_path() == work.path && path.filename() == std::to_string(sourceIndex) + ".fits" &&
                !fs::is_symlink(path), "TemporaryStorageInvalid", "Aligned input is not an owned work file");
            std::error_code error;
            const bool removed = fs::remove(path, error);
            ensure(removed && !error, "TemporaryStorageReleaseFailed", "Unable to release aligned work file: " + path.string());
        };
        // Rejected images are no longer needed. Selected ones are released only
        // when Stacker guarantees it has consumed every sample from that file.
        for (std::size_t i = 0; i < aligned.size(); ++i)
            if (!aligned[i].empty() && !result.frames[i].assessment.selected) releaseAligned(i);
        stackOptions.inputConsumed = [&](std::size_t index) { releaseAligned(selectedIndices.at(index)); };
        stackOptions.progress = [&](const StackProgress& p) {
            const double fraction = (o.sensorPattern ? .70 : .58) + (o.sensorPattern ? .15 : .27) * p.progress;
            if (p.stage == StackProgressStage::Combining && p.rowCount > 0)
                emit("stack-combine", fraction, p.row, p.rowCount);
            else emit("stack", fraction, p.frame, p.frameCount);
        };
        auto stacked = Stacker().stack(selected, stackOptions);
        ensure(stacked.ok, stacked.errorCode, stacked.message);
        result.master = directory / "stack-linear.fits"; write(stacked.image, result.master);
        ImageBuffer noise;
        if(independentNoise) {
            std::ostringstream provenance;provenance<<std::setprecision(17);
            DeepSkyRecipe original;original.inputs=inputs;original.options=o;
            provenance<<"PHOTONSTACK_NOISE_INPUTS 1\n"<<original.serialize()
                <<"\nvariance-estimator registered-luminance-highpass-mad-squared\n"
                <<"noise-stage registered-linear-rgba\nbackground-model-uncertainty excluded\n";
            for(std::size_t slot=0;slot<selectedIndices.size();++slot) {
                const auto i=selectedIndices[slot];const auto& f=result.frames[i];
                provenance<<"frame "<<i<<' '<<slot%2<<' '<<f.sha256<<' '<<f.bytes<<' '<<f.modifiedTicks<<' '
                    <<registeredVariances[i]<<' '<<(stackOptions.frameWeights.empty()?1:stackOptions.frameWeights[slot])
                    <<' '<<std::quoted(f.input.string())<<'\n';
            }
            for(const auto& f:calibrationIdentities)
                provenance<<"calibration "<<f.sha256<<' '<<std::quoted(f.input.string())<<'\n';
            if(!result.sensorPatternReport.empty()) provenance<<"sensor-models "<<fileSHA256(result.sensorPatternReport)<<'\n';
            auto bundle=detail::saveNoiseReference(result.master,std::move(stacked.noiseDiagnostic),provenance.str());
            result.noiseReference=bundle.imagePath;result.noiseReferenceManifest=bundle.manifestPath;result.noiseReferenceInputs=bundle.inputsPath;
            noise=std::move(bundle.image);
        }
        if (stackOptions.collectRejectionMaps) {
            result.rejectionLow = directory / "rejection-low.fits";
            result.rejectionHigh = directory / "rejection-high.fits";
            write(stacked.rejectionLow, result.rejectionLow); write(stacked.rejectionHigh, result.rejectionHigh);
            for (std::size_t i = 0; i < selectedIndices.size(); ++i) {
                auto& f = result.frames[selectedIndices[i]];
                f.rejectedLowSamples = stacked.rejectedLowSamples[i]; f.rejectedHighSamples = stacked.rejectedHighSamples[i];
                f.comparedSamples = stacked.comparedSamples[i];
            }
        }
        auto masterImage = std::move(stacked.image); stacked = {};
        finishImage(std::move(masterImage), directory, o, result,
            [&](const std::string& stage, double fraction) { emit(stage, fraction); },std::move(noise));
        for (const auto& frame : result.frames) checkIdentity(frame);
        if (o.sensorPattern || independentNoise) for (const auto& f : result.frames)
            ensure(fileSHA256(f.input)==f.sha256, "InputChanged", "Source bytes changed during independent-noise or sensor-corrected processing");
        if(independentNoise) for(const auto& f:calibrationIdentities)
            ensure(fileSHA256(f.input)==f.sha256,"InputChanged","Calibration source bytes changed during processing");
        result.ok = true; emit("complete", 1);
    } catch (const WorkflowFailure& e) { result.errorCode = e.code; result.message = e.what(); }
      catch (const std::exception& e) { result.errorCode = "WorkflowFailed"; result.message = e.what(); }
    return result;
}
DeepSkyWorkflowResult DeepSkyWorkflow::finishMaster(const fs::path& master, const fs::path& directory,
                                                   const DeepSkyWorkflowOptions& options, const DeepSkyProgress& progress) const {
    DeepSkyWorkflowResult result;
    try {
        ensure(options.noiseModel==DeepSkyNoiseModel::Scene || options.noiseModel==DeepSkyNoiseModel::IndependentLuminance,
               "ArgumentInvalid","Invalid luminance noise model");
        ensure(options.noiseModel!=DeepSkyNoiseModel::IndependentLuminance || options.denoiseH==0,
               "NoiseReferenceUnsupported","Independent luminance estimation cannot follow NLM");
        ensure(!options.sensorPattern, "SensorPatternConflict", "Sensor pattern correction requires original CFA lights; it cannot be applied to an existing master");
        ensure(!options.stack.collectNoiseDiagnostic && options.stack.frameNoiseVariances.empty(), "ArgumentInvalid",
               "An existing master has no associated noise diagnostic in this workflow");
        ensure(fs::is_regular_file(master), "InputMissing", "Scientific stack master is missing");
        ensure(!directory.empty() && (!fs::exists(directory) || (fs::is_directory(directory) && fs::is_empty(directory))),
               "OutputExists", "Output directory must be new or empty");
        ensure(std::isfinite(options.cropCoverage) && options.cropCoverage >= 0 && options.cropCoverage <= 1,
               "ArgumentInvalid", "Invalid crop coverage");
        ImageReadOptions readOptions; readOptions.fits.mode = FitsDecodeMode::Scientific;
        readOptions.fits.maskNonFinitePixels = true;
        auto read = ImageCodec().read(master, readOptions);
        ensure(read.ok, read.errorCode, read.message);
        ensure(read.image.colorEncoding == ColorEncoding::Linear && (read.image.channels == 3 || read.image.channels == 4),
               "ScientificImageRequired", "Finishing requires a linear RGB stack, not a CFA raw or display image");
        ImageBuffer noise;
        if(options.noiseModel==DeepSkyNoiseModel::IndependentLuminance) {
            try {
                auto bundle=detail::loadNoiseReference(master,read.image);
                result.noiseReference=bundle.imagePath;result.noiseReferenceManifest=bundle.manifestPath;result.noiseReferenceInputs=bundle.inputsPath;
                noise=std::move(bundle.image);
            } catch(const std::exception& e) { throw WorkflowFailure("NoiseReferenceInvalid",e.what()); }
        }
        fs::create_directories(directory);
        result.master = fs::absolute(master); result.masterReused = true;
        finishImage(std::move(read.image), directory, options, result, [&](const std::string& stage, double fraction) {
            if (progress) progress(stage, std::clamp((fraction-.86)/.14, 0., 1.), 0, 0);
        },std::move(noise));
        if (progress) progress("complete", 1, 0, 0);
        result.ok = true;
    } catch (const WorkflowFailure& e) { result.errorCode = e.code; result.message = e.what(); }
      catch (const std::exception& e) { result.errorCode = "WorkflowFailed"; result.message = e.what(); }
    return result;
}
} // namespace photonstack
