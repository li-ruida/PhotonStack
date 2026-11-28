#pragma once

#include "photonstack/WorkerPool.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <type_traits>

namespace photonstack::detail {

// A frame worker shares the available cores with its sibling frames.
inline std::size_t hardwareWorkerLimit() {
    static const auto limit = std::clamp(workerTopology().logical, 1U, 32U);
    return limit;
}
inline thread_local std::size_t rangeWorkerLimit = hardwareWorkerLimit();
struct ScopedRangeWorkerLimit {
    const std::size_t previous = rangeWorkerLimit;
    explicit ScopedRangeWorkerLimit(std::size_t limit) { rangeWorkerLimit = std::max<std::size_t>(1, std::min(previous, limit)); }
    ~ScopedRangeWorkerLimit() { rangeWorkerLimit = previous; }
    ScopedRangeWorkerLimit(const ScopedRangeWorkerLimit&) = delete;
    ScopedRangeWorkerLimit& operator=(const ScopedRangeWorkerLimit&) = delete;
};

// Independent ranges only. Limit workers and keep small images
// serial. The shared pool joins submitted ranges before releasing the callback.
template <typename Callback>
void parallelRanges(std::size_t count, std::size_t minimumPerWorker, Callback callback) {
    const auto workers = std::max<std::size_t>(1, std::min<std::size_t>({
        rangeWorkerLimit, count / std::max<std::size_t>(1, minimumPerWorker)}));
    if (workers == 1) { callback(0, count); return; }
    // Several chunks per participant let faster cores and less expensive rows
    // take more work. Keep chunks substantial; this is not a per-pixel atomic.
    const auto targetChunks = workers * 8;
    const auto block = std::max<std::size_t>(
        count / targetChunks + (count % targetChunks != 0),
        minimumPerWorker / 4 + (minimumPerWorker % 4 != 0));
    const auto chunks = count / block + (count % block != 0);
    std::atomic<std::size_t> next{0};
    const auto nestedBudget = std::max<std::size_t>(1, rangeWorkerLimit / workers);
    runWorkers(workers, [&](std::size_t) {
        const ScopedRangeWorkerLimit budget(nestedBudget);
        while (true) {
            const auto chunk = next.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= chunks) break;
            const auto begin = chunk * block;
            callback(begin, begin + std::min(block, count - begin));
        }
    });
}

template <typename Callback>
void parallelRows(int width, int height, Callback row) {
    static_assert(std::is_nothrow_invocable_v<Callback&, int>);
    parallelRanges(height, std::max(1, 65536 / std::max(1, width)), [&](std::size_t first, std::size_t last) noexcept {
        for (int y = int(first); y < int(last); ++y) row(y);
    });
}

} // namespace photonstack::detail
