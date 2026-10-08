#ifndef HOTPATH_SYS_NUMA_HPP
#define HOTPATH_SYS_NUMA_HPP

#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstddef>

#include "hotpath/core/status.hpp"

namespace hotpath::sys {

struct NodeResult {
    unsigned node{0};
    Status status{};
};

[[nodiscard]] inline NodeResult current_node() noexcept {
    unsigned cpu = 0;
    unsigned node = 0;
    const long rc = ::syscall(SYS_getcpu, &cpu, &node, static_cast<void*>(nullptr));
    if (rc != 0) {
        return NodeResult{0, Status::from_errno()};
    }
    return NodeResult{node, Status{}};
}

[[nodiscard]] inline Status bind_to_node(void* addr, std::size_t len, unsigned node) noexcept {
    constexpr unsigned kMaskBits = sizeof(unsigned long) * 8U;
    if (addr == nullptr || len == 0 || node >= kMaskBits) {
        return Status{EINVAL};
    }

    const unsigned long mask = 1UL << node;
    const long rc = ::syscall(SYS_mbind, addr, len,
                              static_cast<unsigned long>(MPOL_BIND), &mask,
                              static_cast<unsigned long>(kMaskBits + 1U),
                              static_cast<unsigned long>(MPOL_MF_STRICT | MPOL_MF_MOVE));
    if (rc != 0) {
        return Status::from_errno();
    }
    return Status{};
}

} // namespace hotpath::sys

#endif // HOTPATH_SYS_NUMA_HPP
