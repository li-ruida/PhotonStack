#pragma once

#include "photonstack/FitsCodec.hpp"
#include "photonstack/ParallelRanges.hpp"
#include "photonstack/StageProfile.hpp"
#include "photonstack/WorkingMemory.hpp"
#include <future>

namespace photonstack::detail {

// Decode at most one future frame while the caller registers/combines the
// current one. User readers and destructive input-consumed callbacks stay serial.
class FrameReadAhead {
    std::future<ImageReadResult> pending;
    std::uint32_t width, height;
    bool allowed;
  public:
    FrameReadAhead(std::uint32_t w, std::uint32_t h, bool enabled, std::uint64_t retainedBytes = 0)
        : width(w), height(h), allowed(false) {
        const auto pixels = std::uint64_t(w) * h;
        if (!enabled || !pixels || pixels > (1ULL << 32)) return;
        const auto budget = workingMemoryBudget();
        // Two full decode/registration workspaces plus catalog and caller storage.
        allowed = budget > retainedBytes && budget - retainedBytes >= (256ULL << 20) + pixels * 192;
    }
    ~FrameReadAhead() { if (pending.valid()) pending.wait(); }

    template <typename Reader>
    ImageReadResult read(const std::filesystem::path& path, const Reader& reader) {
        return pending.valid() ? pending.get() : reader(path);
    }

    template <typename Reader>
    void schedule(const std::filesystem::path& path, Reader reader) {
        if (!allowed || !isFitsPath(path)) return;
        const auto size = FitsCodec().inspect(path);
        if (!size.ok || size.metadata.width != width || size.metadata.height != height) return;
        auto task = std::make_shared<std::packaged_task<ImageReadResult()>>([reader, path, stage = currentStage] {
            StageScope scope(stage);
            ScopedRangeWorkerLimit limit(4);
            return reader(path);
        });
        auto future = task->get_future();
        if (trySubmitWorker([task] { (*task)(); })) pending = std::move(future);
    }
};
} // namespace photonstack::detail
