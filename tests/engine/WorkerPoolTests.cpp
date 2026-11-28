#include "photonstack/ParallelRanges.hpp"
#include "photonstack/StageProfile.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace photonstack::detail;
using namespace std::chrono_literals;
namespace {
void require(bool valid, const std::source_location where = std::source_location::current()) {
    if (!valid) throw std::runtime_error("Worker scheduling test line " + std::to_string(where.line()));
}
void rangesAndBudgets() {
    unsigned calls = 0;
    runWorkers(0, [&](std::size_t) { ++calls; });
    require(calls == 0);
    parallelRanges(0, 1, [&](std::size_t first, std::size_t last) { require(first == 0 && last == 0); ++calls; });
    require(calls == 1);
    const auto initial = rangeWorkerLimit;
    {
        ScopedRangeWorkerLimit limit(4);
        std::vector<std::atomic<unsigned>> visits(10003);
        parallelRanges(visits.size(), 67, [&](std::size_t first, std::size_t last) {
            require(first < last && last <= visits.size());
            require(rangeWorkerLimit == 1);
            const auto thread = std::this_thread::get_id();
            parallelRanges(last - first, 1, [&](std::size_t begin, std::size_t end) {
                require(thread == std::this_thread::get_id());
                for (auto i = begin; i < end; ++i) visits[first + i].fetch_add(1);
            });
        });
        for (const auto& count : visits) require(count == 1);
        require(rangeWorkerLimit == std::min<std::size_t>(4, initial));
    }
    require(rangeWorkerLimit == initial);
    {
        ScopedRangeWorkerLimit limit(2);
        runWorkers(6, [](std::size_t) { require(rangeWorkerLimit <= 2); });
    }
    // A last partial chunk near SIZE_MAX must not overflow its atomic cursor.
    std::mutex mutex;
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    const auto total = std::numeric_limits<std::size_t>::max();
    parallelRanges(total, total / 4, [&](std::size_t first, std::size_t last) {
        std::lock_guard lock(mutex);
        spans.emplace_back(first, last);
    });
    std::sort(spans.begin(), spans.end());
    require(spans.front().first == 0 && spans.back().second == total);
    for (std::size_t i = 1; i < spans.size(); ++i) require(spans[i - 1].second == spans[i].first);
}
void saturationAndConcurrentCallers() {
    std::atomic<unsigned> completed = 0;
    const auto outer = hardwareWorkerLimit() * 3;
    runWorkers(outer, [&](std::size_t) {
        runWorkers(5, [&](std::size_t) {
            ScopedRangeWorkerLimit limit(1);
            parallelRanges(19, 1, [&](std::size_t first, std::size_t last) { completed += unsigned(last - first); });
        });
    });
    require(completed == outer * 5 * 19);
    completed = 0;
    std::barrier start(6);
    {
        std::vector<std::jthread> callers;
        for (int i = 0; i < 6; ++i) callers.emplace_back([&] {
            start.arrive_and_wait();
            for (int repeat = 0; repeat < 80; ++repeat)
                runWorkers(7, [&](std::size_t) { completed.fetch_add(1); });
        });
    }
    require(completed == 6 * 80 * 7);
}
void exceptionsAndLifetimes() {
    StageProfile profile;
    for (const auto throwing : {std::size_t(0), std::size_t(1)}) {
        std::atomic<unsigned> finished = 0;
        bool caught = false;
        {
            StageScope stage({&profile, Stage::Quality, true});
            ScopedRangeWorkerLimit limit(2);
            try {
                runWorkers(4, [&](std::size_t worker) {
                    require(currentStage.profile == &profile && currentStage.stage == Stage::Quality && currentStage.preferMetal);
                    require(rangeWorkerLimit <= 2);
                    if (worker == throwing) throw std::runtime_error("expected worker exception");
                    std::this_thread::sleep_for(2ms);
                    finished.fetch_add(1);
                });
            } catch (const std::runtime_error& error) {
                caught = std::string(error.what()) == "expected worker exception";
            }
            require(caught && finished == 3);
            std::lock_guard lock(profile.mutex);
            require(profile.stages[unsigned(Stage::Quality)].active == 1);
        }
        require(currentStage.profile == nullptr);
        // If runWorkers escaped before joining, a callback could still use the
        // stack state. All three nonthrowing callbacks must already have exited.
        require(finished == 3 && profile.stages[unsigned(Stage::Quality)].active == 0);
    }
    bool deterministicFailure = false;
    try {
        runWorkers(5, [](std::size_t worker) {
            if (worker == 0) { std::this_thread::sleep_for(2ms); throw std::runtime_error("first-index"); }
            if (worker == 3) throw std::runtime_error("earlier-in-time");
        });
    } catch (const std::runtime_error& error) {
        deterministicFailure = std::string(error.what()) == "first-index";
    }
    require(deterministicFailure);
    std::atomic<unsigned> recovered = 0;
    runWorkers(8, [&](std::size_t) { ++recovered; });
    require(recovered == 8);
}
void profileAccounting() {
    StageProfile profile;
    {
        StageScope stage({&profile, Stage::Warp, false});
        {
            StageScope backend({&profile, Stage::Warp, true});
            require(currentStage.preferMetal);
            recordBackend("metal", "", .001, "warp");
        }
        require(!currentStage.preferMetal);
        {
            StageScope inner(Stage::Quality);
            require(profile.stages[unsigned(Stage::Warp)].active == 0);
            require(profile.stages[unsigned(Stage::Quality)].active == 1);
        }
        profile.sampleResidentMemory();
    }
    const auto& warp = profile.stages[unsigned(Stage::Warp)];
    require(warp.visits == 2 && warp.peakWorkers == 1 && warp.active == 0);
    require(warp.gpu == .001 && warp.metalOperations.at("warp") == 1);
    require(profile.stages[unsigned(Stage::Quality)].visits == 1);
    StageProfile overlap;
    std::barrier rendezvous(2);
    {
        std::jthread one([&] { StageScope s({&overlap, Stage::Combine, false}); rendezvous.arrive_and_wait(); std::this_thread::sleep_for(10ms); });
        std::jthread two([&] { StageScope s({&overlap, Stage::Combine, false}); rendezvous.arrive_and_wait(); std::this_thread::sleep_for(10ms); });
    }
    const auto& combined = overlap.stages[unsigned(Stage::Combine)];
    require(combined.active == 0 && combined.visits == 2 && combined.peakWorkers == 2);
    require(combined.wall >= .005 && combined.wall < 1);
    require(combined.cpu >= 0 && combined.peakResidentBytes > 0);
    for (int i = 0; i < 100; ++i) {
        StageScope disabled({nullptr, Stage::Quality, true});
        require(currentStage.profile == nullptr && currentStage.preferMetal);
    }
    require(currentStage.profile == nullptr && !currentStage.preferMetal);
}
}
int main() {
    rangesAndBudgets();
    saturationAndConcurrentCallers();
    exceptionsAndLifetimes();
    profileAccounting();
    std::cout << "Worker scheduling, nested budgets, exceptions and stage accounting passed\n";
    // Exercise shutdown with an accepted task still alive. The pool destructor
    // joins it; nested submission during draining must run locally, not queue
    // work for a worker that has already stopped.
    trySubmitWorker([] {
        std::this_thread::sleep_for(2ms);
        std::atomic<unsigned> visits = 0;
        runWorkers(5, [&](std::size_t) { ++visits; });
        require(visits == 5);
    });
}
