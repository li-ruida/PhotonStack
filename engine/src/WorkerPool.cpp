#include "photonstack/WorkerPool.hpp"
#include "photonstack/StageProfile.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace photonstack::detail {
namespace {
class WorkerPool {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> queue;
    std::vector<std::thread> threads;
    std::size_t free = 0;
    bool stopping = false;

  public:
    WorkerPool() {
        const auto cores = std::max(1u, std::thread::hardware_concurrency());
        threads.reserve(std::min(cores - 1, 31u));
        for (unsigned i = 1; i < std::min(cores, 32u); ++i) {
            try {
                threads.emplace_back([this] { loop(); });
            } catch (const std::system_error&) {
                break;
            }
            ++free;
        }
    }
    ~WorkerPool() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_all();
        for (auto& t : threads)
            t.join();
    }
    bool submit(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            if (!free)
                return false;
            queue.push_back(std::move(task));
            --free;
        }
        ready.notify_one();
        return true;
    }

  private:
    void loop() {
        std::unique_lock lock(mutex);
        while (true) {
            ready.wait(lock, [&] { return stopping || !queue.empty(); });
            if (queue.empty())
                return;
            auto task = std::move(queue.front());
            queue.pop_front();
            lock.unlock();
            task();
            lock.lock();
            ++free;
        }
    }
};
} // namespace
bool trySubmitWorker(std::function<void()> task) {
    static WorkerPool pool;
    return pool.submit(std::move(task));
}
void runWorkers(std::size_t count, const std::function<void(std::size_t)>& work) {
    if (count == 1) {
        work(0);
        return;
    }
    std::vector<std::future<void>> pending;
    pending.reserve(count - 1);
    const auto ctx = currentStage;
    try {
        for (std::size_t i = 1; i < count; ++i) {
            auto task = std::make_shared<std::packaged_task<void()>>([&, i, ctx] {
                const StageScope scope(ctx);
                work(i);
            });
            auto future = task->get_future();
            if (trySubmitWorker([task] { (*task)(); }))
                pending.push_back(std::move(future));
            else
                work(i);
        }
        work(0);
        for (auto& f : pending)
            f.get();
    } catch (...) {
        for (auto& f : pending)
            if (f.valid())
                f.wait();
        throw;
    }
}
} // namespace photonstack::detail
