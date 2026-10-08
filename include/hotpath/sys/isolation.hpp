#ifndef HOTPATH_SYS_ISOLATION_HPP
#define HOTPATH_SYS_ISOLATION_HPP

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <string_view>

#include "hotpath/core/status.hpp"

namespace hotpath::sys {

[[nodiscard]] constexpr bool cpu_list_contains(std::string_view list, unsigned cpu) noexcept {
    constexpr unsigned kMaxCpu = 1U << 20;

    unsigned lo = 0;
    unsigned cur = 0;
    bool have_lo = false;
    bool have_digit = false;

    for (const char c : list) {
        if (c >= '0' && c <= '9') {
            if (cur > kMaxCpu) {
                return false;
            }
            cur = cur * 10U + static_cast<unsigned>(c - '0');
            have_digit = true;
        } else if (c == '-') {
            if (!have_digit || have_lo) {
                return false;
            }
            lo = cur;
            cur = 0;
            have_lo = true;
            have_digit = false;
        } else if (c == ',' || c == '\n') {
            if (have_digit && cpu >= (have_lo ? lo : cur) && cpu <= cur) {
                return true;
            }
            lo = 0;
            cur = 0;
            have_lo = false;
            have_digit = false;
        } else {
            return false;
        }
    }
    return have_digit && cpu >= (have_lo ? lo : cur) && cpu <= cur;
}

static_assert(cpu_list_contains("2-5,8,10-11\n", 2));
static_assert(cpu_list_contains("2-5,8,10-11\n", 5));
static_assert(cpu_list_contains("2-5,8,10-11\n", 8));
static_assert(cpu_list_contains("2-5,8,10-11", 11));
static_assert(!cpu_list_contains("2-5,8,10-11\n", 1));
static_assert(!cpu_list_contains("2-5,8,10-11\n", 9));
static_assert(!cpu_list_contains("\n", 0));
static_assert(!cpu_list_contains("", 0));
static_assert(!cpu_list_contains("2-x", 2));

struct IsolationResult {
    bool isolated{false};
    Status status{};
};

[[nodiscard]] inline IsolationResult is_cpu_isolated(unsigned cpu) noexcept {
    constexpr std::size_t kBufBytes = 4096;
    constexpr int kMaxAttempts = 4;

    const int fd = ::open("/sys/devices/system/cpu/isolated", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return IsolationResult{false, Status::from_errno()};
    }

    char buf[kBufBytes];
    ssize_t n = -1;
    int err = EINTR;
    for (int attempt = 0; attempt < kMaxAttempts && n < 0 && err == EINTR; ++attempt) {
        n = ::read(fd, buf, sizeof(buf));
        err = (n < 0) ? errno : 0;
    }
    static_cast<void>(::close(fd));

    if (n < 0) {
        return IsolationResult{false, Status{err}};
    }
    const std::string_view list{buf, static_cast<std::size_t>(n)};
    return IsolationResult{cpu_list_contains(list, cpu), Status{}};
}

} // namespace hotpath::sys

#endif // HOTPATH_SYS_ISOLATION_HPP
