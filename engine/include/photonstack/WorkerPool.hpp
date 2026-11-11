#pragma once

#include <cstddef>
#include <functional>

namespace photonstack::detail {
// A submission reserves an idle worker before it can be queued. Nested callers
// execute locally when all workers are reserved, so a waiting frame never
// depends on a task queued behind another waiting frame.
bool trySubmitWorker(std::function<void()> task);
void runWorkers(std::size_t count, const std::function<void(std::size_t)>& work);
} // namespace photonstack::detail
