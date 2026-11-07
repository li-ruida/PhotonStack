#pragma once

#include "photonstack/ParallelRanges.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <future>

namespace photonstack::detail {

inline std::size_t analysisFrameCapacity(std::uint64_t pixels, std::uint64_t referencePixels,
                                         std::size_t frames, std::uint32_t sampleSide,
                                         std::uint64_t memoryBudget, std::size_t cpuLimit) {
    constexpr std::uint64_t mib = 1024 * 1024;
    if (!pixels || pixels > 32 * 1024 * 1024 || referencePixels > 32 * 1024 * 1024 ||
        frames > 2000 || sampleSide > 512) return 1;
    // Keep room for all retained temporal grids and the shared float RGBA reference.
    // Per worker allow decoded pixels, Bayer/stellar scratch and matching bins.
    const auto shared = 64 * mib + referencePixels * 16 + frames * std::uint64_t(sampleSide) * sampleSide * 8;
    // Projected star matching builds indexes on the reference canvas, even
    // when an individual light has different dimensions.
    const auto perFrame = 128 * mib + std::max(pixels, referencePixels) * 64;
    const auto byMemory = memoryBudget > shared ? (memoryBudget - shared) / perFrame : 0;
    return std::max<std::size_t>(1, std::min<std::size_t>({4, cpuLimit, byMemory}));
}

// Preparation and commit run on the caller in input order. Workers return compact
// results by value. Futures from std::launch::async join on unwind, before any
// captured image, calibration buffer or run-owned directory can be destroyed.
template <typename Prepare, typename Capacity, typename Work, typename Commit>
void orderedAnalysisFrames(std::size_t count, Prepare prepare, Capacity capacity, Work work, Commit commit) {
    using Result = std::invoke_result_t<Work, std::size_t>;
    struct Job { std::size_t index, limit; std::future<Result> result; };
    std::deque<Job> pending;
    const auto finishFirst = [&] {
        auto job = std::move(pending.front());
        pending.pop_front();
        commit(job.index, job.result.get());
    };
    const auto drain = [&] { while (!pending.empty()) finishFirst(); };
    for (std::size_t i = 0; i < count; ++i) {
        if (pending.size() == 4) finishFirst();
        std::size_t limit = 1;
        try {
            if (!prepare(i)) continue;
            limit = capacity(i);
        } catch (...) {
            // A prior input's error takes precedence over later validation.
            drain();
            throw;
        }
        while (!pending.empty() && (pending.size() >= limit ||
               std::any_of(pending.begin(), pending.end(), [&](const Job& old) { return pending.size() >= old.limit; })))
            finishFirst();
        if (limit == 1) {
            commit(i, work(i));
            continue;
        }
        try {
            pending.push_back({i, limit, std::async(std::launch::async, [&, i, limit] {
                const ScopedRangeWorkerLimit inner(std::max<std::size_t>(1, std::thread::hardware_concurrency() / limit));
                return work(i);
            })});
        } catch (const std::system_error&) {
            // Thread resources may be temporarily exhausted. Complete this
            // input on the caller after all earlier jobs have been consumed.
            drain();
            commit(i, work(i));
        }
    }
    drain();
}

} // namespace photonstack::detail
