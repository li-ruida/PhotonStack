#include "photonstack/StageProfile.hpp"
#include <algorithm>
#include <ctime>
#include <string_view>
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#endif

namespace photonstack::detail {
thread_local StageContext currentStage;
namespace {
thread_local double since;
double threadCPU() {
#ifdef CLOCK_THREAD_CPUTIME_ID
    timespec value{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value);
    return value.tv_sec + value.tv_nsec * 1.e-9;
#else
    return 0;
#endif
}
std::uint64_t residentBytes() {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.resident_size;
#elif defined(__linux__)
    if (auto* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long pages = 0;
        int fields = std::fscanf(f, "%*u %lu", &pages);
        std::fclose(f);
        if (fields == 1)
            return pages * std::uint64_t(sysconf(_SC_PAGESIZE));
    }
#endif
    return 0;
}
void transition(StageContext next) {
    const auto cpu = threadCPU();
    const auto rss = residentBytes();
    const auto update = [&](StageContext ctx, bool enter) {
        if (!ctx.profile)
            return;
        std::lock_guard lock(ctx.profile->mutex);
        auto& m = ctx.profile->stages[unsigned(ctx.stage)];
        const auto now = std::chrono::steady_clock::now();
        m.peakResidentBytes = std::max(m.peakResidentBytes, rss);
        if (enter) {
            if (!m.active++)
                m.started = now;
            m.peakWorkers = std::max(m.peakWorkers, m.active);
            ++m.visits;
        } else {
            m.cpu += cpu - since;
            if (!--m.active)
                m.wall += std::chrono::duration<double>(now - m.started).count();
        }
    };
    update(currentStage, false);
    currentStage = next;
    update(next, true);
    since = cpu;
}
} // namespace
void StageProfile::sampleResidentMemory() {
    const auto bytes = residentBytes();
    std::lock_guard lock(mutex);
    for (auto& stage : stages) if (stage.active)
        stage.peakResidentBytes = std::max(stage.peakResidentBytes, bytes);
}
StageScope::StageScope(StageContext next)
    : previous(currentStage),
      switched(next.preferMetal != previous.preferMetal || next.profile != previous.profile || (next.profile && next.stage != previous.stage)) {
    if (switched)
        transition(next);
}
StageScope::~StageScope() {
    if (switched)
        transition(previous);
}
FrameActivity::FrameActivity() {
    if (!profile)
        return;
    std::lock_guard lock(profile->mutex);
    profile->analysisPeak = std::max(profile->analysisPeak, ++profile->activeFrames);
}
FrameActivity::~FrameActivity() {
    if (!profile)
        return;
    std::lock_guard lock(profile->mutex);
    --profile->activeFrames;
}
void recordBackend(const char* backend, const char* reason, double gpuSeconds, const char* operation) {
    if (!currentStage.profile)
        return;
    std::lock_guard lock(currentStage.profile->mutex);
    auto& m = currentStage.profile->stages[unsigned(currentStage.stage)];
    m.backend = m.backendReported && std::string_view(m.backend) != backend ? "mixed" : backend;
    m.backendReported = true;
    if (*reason)
        m.fallback = reason;
    m.gpu += gpuSeconds;
    if (operation) ++m.metalOperations[operation];
}
} // namespace photonstack::detail
