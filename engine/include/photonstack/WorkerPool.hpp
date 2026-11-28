#pragma once

#include <cstddef>
#include <functional>

namespace photonstack::detail {
struct WorkerTopology {
    unsigned logical = 1;
    unsigned physical = 1;
    unsigned performanceLevel0Physical = 0;
    unsigned otherPerformancePhysical = 0;
};
// Performance tiers are diagnostics, not affinity. Level 1 need not mean E
// cores (M5 Pro reports Super + Performance). Dynamic ranges use all tiers;
// the operating system remains responsible for placement and migration.
const WorkerTopology& workerTopology();

// A submission reserves an idle worker before it can be queued. Nested callers
// execute locally when all workers are reserved, so a waiting frame never
// depends on a task queued behind another waiting frame.
bool trySubmitWorker(std::function<void()> task);
// All callbacks finish before return, including after an exception. The lowest
// failing callback index is rethrown after the completion group is drained.
void runWorkers(std::size_t count, const std::function<void(std::size_t)>& work);
} // namespace photonstack::detail
