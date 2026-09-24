#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <vector>

namespace photonstack::detail {

// Independent, nonthrowing ranges only. Limit workers and keep small images
// serial. jthread joins already-started workers even if thread creation fails.
template <typename Callback>
void parallelRanges(std::size_t count, std::size_t minimumPerWorker, Callback callback) {
    static_assert(std::is_nothrow_invocable_v<Callback&, std::size_t, std::size_t>);
    const auto workers = std::max<std::size_t>(1, std::min<std::size_t>({
        8, std::thread::hardware_concurrency(), count / std::max<std::size_t>(1, minimumPerWorker)}));
    if (workers == 1) { callback(0, count); return; }
    const auto block = count / workers + (count % workers != 0);
    std::vector<std::jthread> threads;
    threads.reserve(workers - 1);
    for (std::size_t worker = 1; worker < workers; ++worker) {
        const auto begin = std::min(count, worker * block);
        const auto end = begin + std::min(block, count - begin);
        threads.emplace_back([&, begin, end]() noexcept { callback(begin, end); });
    }
    callback(0, std::min(count, block));
}

} // namespace photonstack::detail
