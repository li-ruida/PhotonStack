#pragma once
#include <algorithm>
#include <cstdint>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace photonstack::detail {
inline std::uint64_t workingMemoryBudget() {
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const auto pages = sysconf(_SC_PHYS_PAGES), pageBytes = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pageBytes <= 0) return 0;
    auto budget = std::min<std::uint64_t>(3ULL << 30, std::uint64_t(pages) * pageBytes / 4);
#ifdef __APPLE__
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t length = HOST_VM_INFO64_COUNT;
    const auto host = mach_host_self();
    const auto status = host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &length);
    mach_port_deallocate(mach_task_self(), host);
    if (status != KERN_SUCCESS) return 0;
    const auto available = std::uint64_t(stats.free_count) + stats.inactive_count;
    budget = std::min(budget, available * pageBytes / 2);
#elif defined(_SC_AVPHYS_PAGES)
    const auto available = sysconf(_SC_AVPHYS_PAGES);
    if (available <= 0) return 0;
    budget = std::min(budget, std::uint64_t(available) * pageBytes / 2);
#endif
    return budget;
#else
    return 0;
#endif
}
} // namespace photonstack::detail
