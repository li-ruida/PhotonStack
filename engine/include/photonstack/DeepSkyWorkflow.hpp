#pragma once
#include "photonstack/MultiscaleDenoiser.hpp"
#include "photonstack/ChannelAlignment.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "photonstack/AstroDevelop.hpp"
#include "photonstack/BackgroundExtractor.hpp"
#include "photonstack/SequenceQualityAnalyzer.hpp"
#include "photonstack/Stacker.hpp"
#include "photonstack/MasterFrameBuilder.hpp"
#include "photonstack/Calibrator.hpp"

namespace photonstack {

enum class DeepSkyBackground { None, Polynomial, Grid, PolynomialAndGrid };
enum class DeepSkyNoiseModel { Scene, IndependentLuminance };

struct DeepSkyWorkflowOptions {
    StackOptions stack;
    SequenceQualityOptions quality;
    std::vector<std::filesystem::path> darks, biases, flats;
    MasterFrameMethod masterMethod = MasterFrameMethod::Median;
    CalibrationBiasState darkBiasState = CalibrationBiasState::Unknown;
    CalibrationBiasState flatBiasState = CalibrationBiasState::Unknown;
    // -1 selects a reference by measured stellar width/shape and coverage.
    int referenceIndex = -1;
    bool analysisOnly = false;
    // Estimate and subtract sensor-fixed high-frequency residuals before CFA
    // interpolation. Requires uncalibrated FITS and sufficient field movement.
    bool sensorPattern = false;
    DeepSkyBackground background = DeepSkyBackground::PolynomialAndGrid;
    BackgroundPolynomialOptions polynomial;
    BackgroundGridOptions grid;
    // Largest axis-aligned rectangle meeting this coverage, without changing
    // orientation/aspect. Zero retains the complete reference canvas.
    double cropCoverage = 1;
    std::uint32_t cropInset = 0;
    // 0 disables, -1 estimates h from scientific noise, positive sets input units.
    double denoiseH = 0;
    double denoiseBlend = .8;
    MultiscaleDenoiseOptions multiscale;
    DeepSkyNoiseModel noiseModel = DeepSkyNoiseModel::Scene;
    double backgroundDenoiseStrength = 0;
    // Optional diagnostic snapshots; never needed to reuse the scientific master.
    // Background-corrected input and final developed output remain mandatory.
    bool saveDenoiseStages = false;
    bool alignChannels = true;
    AstroDevelopOptions develop;
    DeepSkyWorkflowOptions();
};

struct DeepSkyFrameReport {
    std::filesystem::path input;
    std::uintmax_t bytes = 0;
    std::int64_t modifiedTicks = 0;
    std::string sha256;
    int sensorPatternModel = -1;
    SequenceFrameSample native;
    RegistrationResult registration;
    SequenceFrameAssessment assessment;
    bool reference = false;
    double alignedCoverage = 0;
    std::string errorCode, message;
    std::uint64_t rejectedLowSamples = 0, rejectedHighSamples = 0, comparedSamples = 0;
};

struct DeepSkyWorkflowResult {
    bool ok = false;
    bool masterReused = false;
    std::vector<DeepSkyFrameReport> frames;
    std::size_t selectedCount = 0;
    std::size_t referenceIndex = 0;
    std::uint32_t cropX = 0, cropY = 0, cropWidth = 0, cropHeight = 0;
    std::filesystem::path master, backgroundMaster, developed, rejectionLow, rejectionHigh;
    std::filesystem::path sensorPatternReport;
    std::filesystem::path noiseReference, noiseReferenceManifest, noiseReferenceInputs;
    bool independentLuminanceNoise = false;
    double denoiseH = 0;
    ChannelAlignmentResult channelAlignment;
    AstroDevelopResult development;
    std::string errorCode, message;
};

// Fraction is an overall phase-weighted indicator, not a time estimate. Step
// counts exposures except during stack-combine, where it counts completed rows.
using DeepSkyProgress = std::function<void(const std::string& stage, double fraction,
                                         std::size_t step, std::size_t total)>;

// Both desktop and command-line frontends invoke this same implementation.
// Optional calibration masters are built once; FITS sensor calibration precedes CFA decoding.
// No input is edited/deleted. Output directory must be new or empty. Aligned
// work files are owned by this run and removed on return, including failure.
class DeepSkyWorkflow {
  public:
    // Reuse an existing scientific stack; no calibration/registration is repeated.
    DeepSkyWorkflowResult finishMaster(const std::filesystem::path& master,
        const std::filesystem::path& outputDirectory, const DeepSkyWorkflowOptions& options,
        const DeepSkyProgress& progress = {}) const;
    DeepSkyWorkflowResult run(const std::vector<std::filesystem::path>& inputs,
                             const std::filesystem::path& outputDirectory,
                             const DeepSkyWorkflowOptions& options = {},
                             const DeepSkyProgress& progress = {}) const;
};
} // namespace photonstack
