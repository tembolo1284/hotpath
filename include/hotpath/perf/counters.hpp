#ifndef HOTPATH_PERF_COUNTERS_HPP
#define HOTPATH_PERF_COUNTERS_HPP

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"

namespace hotpath::perf {

struct CounterSample {
    std::uint64_t cycles{0};
    std::uint64_t instructions{0};
    std::uint64_t cache_misses{0};
    std::uint64_t branch_misses{0};

    [[nodiscard]] constexpr CounterSample operator-(const CounterSample& earlier) const noexcept {
        return CounterSample{cycles - earlier.cycles, instructions - earlier.instructions,
                             cache_misses - earlier.cache_misses,
                             branch_misses - earlier.branch_misses};
    }

    [[nodiscard]] constexpr std::uint64_t instructions_per_kilocycle() const noexcept {
        return cycles == 0 ? 0 : instructions * 1000U / cycles;
    }
};

class HardwareCounters {
public:
    static constexpr std::size_t kEvents = 4;

    HardwareCounters() noexcept = default;
    HardwareCounters(const HardwareCounters&) = delete;
    HardwareCounters& operator=(const HardwareCounters&) = delete;
    ~HardwareCounters() { close(); }

    [[nodiscard]] Status open() noexcept {
        HOTPATH_ASSERT(fds_[0] < 0);
        constexpr std::array<std::uint64_t, kEvents> kConfigs{
            PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_CACHE_MISSES,
            PERF_COUNT_HW_BRANCH_MISSES};

        for (std::size_t i = 0; i < kEvents; ++i) {
            perf_event_attr attr{};
            attr.type = PERF_TYPE_HARDWARE;
            attr.size = sizeof(attr);
            attr.config = kConfigs[i];
            attr.disabled = i == 0 ? 1U : 0U;
            attr.exclude_kernel = 1;
            attr.exclude_hv = 1;
            attr.read_format = PERF_FORMAT_GROUP;
            const long fd = ::syscall(SYS_perf_event_open, &attr, 0, -1, fds_[0], 0UL);
            if (fd < 0) {
                const Status st = Status::from_errno();
                close();
                return st;
            }
            fds_[i] = static_cast<int>(fd);
        }
        return Status{};
    }

    [[nodiscard]] Status start() noexcept {
        if (fds_[0] < 0) {
            return Status{EBADF};
        }
        if (::ioctl(fds_[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0 ||
            ::ioctl(fds_[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
            return Status::from_errno();
        }
        return Status{};
    }

    [[nodiscard]] Status stop() noexcept {
        if (fds_[0] < 0) {
            return Status{EBADF};
        }
        if (::ioctl(fds_[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != 0) {
            return Status::from_errno();
        }
        return Status{};
    }

    [[nodiscard]] Status read(CounterSample& out) noexcept {
        if (fds_[0] < 0) {
            return Status{EBADF};
        }
        struct {
            std::uint64_t count;
            std::uint64_t values[kEvents];
        } raw{};
        const ssize_t n = ::read(fds_[0], &raw, sizeof(raw));
        if (n < 0) {
            return Status::from_errno();
        }
        if (n != static_cast<ssize_t>(sizeof(raw)) || raw.count != kEvents) {
            return Status{EIO};
        }
        out = CounterSample{raw.values[0], raw.values[1], raw.values[2], raw.values[3]};
        return Status{};
    }

    void close() noexcept {
        for (std::size_t i = 0; i < kEvents; ++i) {
            if (fds_[i] >= 0) {
                static_cast<void>(::close(fds_[i]));
                fds_[i] = -1;
            }
        }
    }

    [[nodiscard]] bool is_open() const noexcept { return fds_[0] >= 0; }

private:
    std::array<int, kEvents> fds_{-1, -1, -1, -1};
};

} // namespace hotpath::perf

#endif // HOTPATH_PERF_COUNTERS_HPP
