#include "photonstack/WorkerPool.hpp"
#include "photonstack/ParallelRanges.hpp"
#include "photonstack/StageProfile.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace photonstack::detail {
const WorkerTopology& workerTopology() {
    static const WorkerTopology topology = [] {
        WorkerTopology result;
        result.logical = std::max(1u, std::thread::hardware_concurrency());
        result.physical = result.logical;
#ifdef __APPLE__
        const auto query = [](const char* name, unsigned fallback) {
            unsigned value = 0;
            auto size = sizeof(value);
            return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value ? value : fallback;
        };
        result.logical = query("hw.logicalcpu", result.logical);
        result.physical = query("hw.physicalcpu", result.physical);
        const auto levels = query("hw.nperflevels", 1);
        if (levels > 1) {
            result.performanceLevel0Physical = query("hw.perflevel0.physicalcpu", 0);
            if (result.performanceLevel0Physical && result.performanceLevel0Physical <= result.physical)
                result.otherPerformancePhysical = result.physical - result.performanceLevel0Physical;
        }
#endif
        return result;
    }();
    return topology;
}

namespace {
// One stack-owned completion group replaces each worker's packaged_task,
// shared_ptr and future. It outlives every callback, including on failure.
struct WorkerGroup {
    const std::function<void(std::size_t)>& work;
    const StageContext stage = currentStage;
    const std::size_t rangeBudget = rangeWorkerLimit;
    std::mutex mutex;
    std::condition_variable completed;
    std::size_t remaining;
    std::exception_ptr error;
    std::size_t errorIndex = std::size_t(-1);

    WorkerGroup(std::size_t count, const std::function<void(std::size_t)>& callback)
        : work(callback), remaining(count) {}

    void run(std::size_t index) noexcept {
        std::exception_ptr failure;
        try {
            const StageScope context(stage);
            const ScopedRangeWorkerLimit budget(rangeBudget);
            work(index);
        } catch (...) {
            failure = std::current_exception();
        }
        // Notify while holding the completion mutex: a waiter cannot destroy
        // the stack group while the final worker is still notifying it.
        std::lock_guard lock(mutex);
        if (failure && index < errorIndex) {
            error = std::move(failure);
            errorIndex = index;
        }
        if (--remaining == 0) completed.notify_one();
    }

    void wait() {
        std::unique_lock lock(mutex);
        completed.wait(lock, [&] { return remaining == 0; });
        if (error) std::rethrow_exception(error);
    }
};

class WorkerPool {
    struct Task {
        std::function<void()> owned;
        WorkerGroup* group = nullptr;
        std::size_t index = 0;
        void run() { if (group) group->run(index); else owned(); }
    };
    std::mutex mutex;
    std::condition_variable ready;
    // Reservation bounds the queue by the worker count (at most 31). A fixed
    // ring also avoids deque block allocations for repeated short stripes.
    std::array<Task, 32> queue;
    std::size_t head = 0, tail = 0, queued = 0;
    std::vector<std::thread> threads;
    std::size_t free = 0;
    bool stopping = false;

  public:
    WorkerPool() {
        const auto cores = hardwareWorkerLimit();
        threads.reserve(cores - 1);
        for (std::size_t i = 1; i < cores; ++i) {
            try { threads.emplace_back([this] { loop(); }); }
            catch (const std::system_error&) { break; }
            ++free;
        }
    }
    ~WorkerPool() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_all();
        for (auto& t : threads) t.join();
    }
    bool submit(std::function<void()> task) { return submitTask({std::move(task), nullptr, 0}); }
    bool submit(WorkerGroup& group, std::size_t index) { return submitTask({{}, &group, index}); }

  private:
    bool submitTask(Task task) {
        {
            std::lock_guard lock(mutex);
            if (stopping || !free) return false;
            queue[tail] = std::move(task);
            tail = (tail + 1) % queue.size();
            ++queued;
            --free;
        }
        ready.notify_one();
        return true;
    }
    void loop() {
        std::unique_lock lock(mutex);
        while (true) {
            ready.wait(lock, [&] { return stopping || queued; });
            if (!queued) return;
            auto task = std::move(queue[head]);
            queue[head] = {};
            head = (head + 1) % queue.size();
            --queued;
            lock.unlock();
            task.run();
            lock.lock();
            ++free;
        }
    }
};
WorkerPool& sharedPool() { static WorkerPool pool; return pool; }
} // namespace
bool trySubmitWorker(std::function<void()> task) { return sharedPool().submit(std::move(task)); }
void runWorkers(std::size_t count, const std::function<void(std::size_t)>& work) {
    if (count == 0) return;
    if (count == 1) { work(0); return; }
    auto& pool = sharedPool();
    WorkerGroup group(count, work);
    for (std::size_t i = 1; i < count; ++i) {
        if (!pool.submit(group, i)) group.run(i);
    }
    group.run(0);
    group.wait();
}
} // namespace photonstack::detail
