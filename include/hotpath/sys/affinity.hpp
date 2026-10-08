#ifndef HOTPATH_SYS_AFFINITY_HPP
#define HOTPATH_SYS_AFFINITY_HPP

#include <pthread.h>
#include <sched.h>

#include "hotpath/core/status.hpp"

namespace hotpath::sys {

struct CpuResult {
    unsigned cpu{0};
    Status status{};
};

[[nodiscard]] inline CpuResult current_cpu() noexcept {
    const int cpu = ::sched_getcpu();
    if (cpu < 0) {
        return CpuResult{0, Status::from_errno()};
    }
    return CpuResult{static_cast<unsigned>(cpu), Status{}};
}

[[nodiscard]] inline Status pin_current_thread(unsigned cpu) noexcept {
    if (cpu >= static_cast<unsigned>(CPU_SETSIZE)) {
        return Status{EINVAL};
    }

    cpu_set_t want;
    CPU_ZERO(&want);
    CPU_SET(cpu, &want);

    const int set_rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(want), &want);
    if (set_rc != 0) {
        return Status{set_rc};
    }

    cpu_set_t got;
    CPU_ZERO(&got);
    const int get_rc = ::pthread_getaffinity_np(::pthread_self(), sizeof(got), &got);
    if (get_rc != 0) {
        return Status{get_rc};
    }
    if (CPU_COUNT(&got) != 1 || !CPU_ISSET(cpu, &got)) {
        return Status{EINVAL};
    }
    return Status{};
}

} // namespace hotpath::sys

#endif // HOTPATH_SYS_AFFINITY_HPP
