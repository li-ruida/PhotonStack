#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <map>
#include <string>

namespace photonstack::detail {

enum class Stage : unsigned {
    Other,
    FitsDecode,
    RawDecode,
    Demosaic,
    Stars,
    Quality,
    Reference,
    Matching,
    Solve,
    Warp,
    Normalize,
    Combine,
    Background,
    Channels,
    Develop,
    Write,
    Calibration,
    Count
};
constexpr std::array<const char*, unsigned(Stage::Count)> stageNames{"workflow",
                                                                     "fits-decode",
                                                                     "raw-decode",
                                                                     "demosaic",
                                                                     "star-detection",
                                                                     "quality",
                                                                     "reference-selection",
                                                                     "star-matching",
                                                                     "transform-solve",
                                                                     "registration-resample",
                                                                     "background-normalization",
                                                                     "robust-stack",
                                                                     "background-model",
                                                                     "channel-alignment",
                                                                     "stretch-and-finish",
                                                                     "file-write", "calibration"};

struct StageMetrics {
    unsigned active = 0, peakWorkers = 0;
    std::uint64_t peakResidentBytes = 0, visits = 0;
    double wall = 0, cpu = 0, gpu = 0;
    std::chrono::steady_clock::time_point started;
    const char* backend = "cpu";
    bool backendReported = false;
    std::map<std::string, std::uint64_t> metalOperations;
    const char* fallback = "";
};

struct StageProfile {
    void sampleResidentMemory();
    std::mutex mutex;
    std::array<StageMetrics, unsigned(Stage::Count)> stages{};
    unsigned activeFrames = 0, analysisPeak = 0;
    const char* admissionReason = "";
};

struct StageContext {
    StageProfile* profile = nullptr;
    Stage stage = Stage::Other;
    bool preferMetal = false;
};
extern thread_local StageContext currentStage;

// Exclusive stages on each thread. Concurrent visits to the same stage share
// wall time, while their thread CPU times are additive.
class StageScope {
  public:
    explicit StageScope(StageContext next);
    explicit StageScope(Stage next) : StageScope({currentStage.profile, next, currentStage.preferMetal}) {}
    ~StageScope();
    StageScope(const StageScope&) = delete;

  private:
    StageContext previous;
    bool switched;
};

class FrameActivity {
    StageProfile* profile = currentStage.profile;

  public:
    FrameActivity();
    ~FrameActivity();
};

void recordBackend(const char* backend, const char* reason, double gpuSeconds = 0, const char* operation = nullptr);
} // namespace photonstack::detail
