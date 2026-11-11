#pragma once

#include "photonstack/WorkerPool.hpp"

#include <algorithm>
#include <cstddef>
#include <thread>
#include <type_traits>

namespace photonstack::detail {

// A frame worker shares the available cores with its sibling frames.
inline thread_local std::size_t rangeWorkerLimit = 8;
struct ScopedRangeWorkerLimit {
    const std::size_t previous = rangeWorkerLimit;
    explicit ScopedRangeWorkerLimit(std::size_t limit) { rangeWorkerLimit = std::min(previous, limit); }
    ~ScopedRangeWorkerLimit() { rangeWorkerLimit = previous; }
    ScopedRangeWorkerLimit(const ScopedRangeWorkerLimit&) = delete;
    ScopedRangeWorkerLimit& operator=(const ScopedRangeWorkerLimit&) = delete;
};

// Independent, nonthrowing ranges only. Limit workers and keep small images
// serial. The shared pool joins submitted ranges before releasing the callback.
template <typename Callback>
void parallelRanges(std::size_t count, std::size_t minimumPerWorker, Callback callback) {
    static_assert(std::is_nothrow_invocable_v<Callback&, std::size_t, std::size_t>);
    const auto workers = std::max<std::size_t>(1, std::min<std::size_t>({
        rangeWorkerLimit, std::thread::hardware_concurrency(), count / std::max<std::size_t>(1, minimumPerWorker)}));
    if (workers == 1) { callback(0, count); return; }
    const auto block = count / workers + (count % workers != 0);
    runWorkers(workers, [&](std::size_t worker) {
        const auto begin = std::min(count, worker * block);
        callback(begin, begin + std::min(block, count - begin));
    });
}

} // namespace photonstack::detail
