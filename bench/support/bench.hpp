#ifndef HOTPATH_BENCH_SUPPORT_BENCH_HPP
#define HOTPATH_BENCH_SUPPORT_BENCH_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "hotpath/core/clock.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/perf/counters.hpp"
#include "hotpath/perf/histogram.hpp"
#include "hotpath/perf/report.hpp"
#include "hotpath/perf/timer.hpp"
#include "hotpath/sys/affinity.hpp"
#include "hotpath/sys/isolation.hpp"

namespace hotpath::bench {

template <typename T>
[[gnu::always_inline]] inline void keep(const T& value) noexcept {
    asm volatile("" : : "r,m"(value) : "memory");
}

[[noreturn]] inline void die(const char* what) noexcept {
    static_cast<void>(std::fprintf(stderr, "bench: %s\n", what));
    std::exit(1);
}

inline void require(const Status& status, const char* what) noexcept {
    if (!status.ok()) {
        static_cast<void>(std::fprintf(stderr, "bench: %s: %s\n", what, std::strerror(status.err())));
        std::exit(1);
    }
}

class Xorshift {
public:
    explicit constexpr Xorshift(std::uint64_t seed) noexcept : state_{seed == 0 ? 1 : seed} {}

    constexpr std::uint64_t next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }

    constexpr std::uint32_t below(std::uint32_t bound) noexcept {
        return static_cast<std::uint32_t>((next() >> 16) % bound);
    }

private:
    std::uint64_t state_;
};

class Session {
public:
    static constexpr int kNoCpu = -1;

    Session(int argc, char** argv, const char* title) noexcept {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{argv[i]};
            if (arg == "--help") {
                static_cast<void>(std::printf(
                    "%s\n"
                    "  --cpu=N       pin the measuring thread\n"
                    "  --cpu2=N      pin the second thread, for cross-core cases\n"
                    "  --ops=N       operations per case (default varies by case)\n"
                    "  --filter=TEXT run only the cases whose name contains TEXT\n",
                    title));
                std::exit(0);
            } else if (arg.starts_with("--cpu=")) {
                cpu_ = static_cast<int>(number(arg.substr(6)));
            } else if (arg.starts_with("--cpu2=")) {
                cpu2_ = static_cast<int>(number(arg.substr(7)));
            } else if (arg.starts_with("--ops=")) {
                ops_ = number(arg.substr(6));
            } else if (arg.starts_with("--filter=")) {
                filter_ = arg.substr(9);
            } else {
                die("unknown option, try --help");
            }
        }

        if (!has_invariant_tsc()) {
            die("this machine has no invariant TSC");
        }
        if (cpu_ != kNoCpu) {
            require(sys::pin_current_thread(static_cast<unsigned>(cpu_)), "cannot pin --cpu");
        }
        scale_ = calibrate_tsc();
        overhead_ = perf::measure_clock_overhead();
        const Status counters = counters_.open();

        static_cast<void>(std::printf("== %s ==\n", title));
        if (cpu_ == kNoCpu) {
            static_cast<void>(std::printf("not pinned (pass --cpu=N for steadier numbers)\n"));
        } else {
            const bool isolated = sys::is_cpu_isolated(static_cast<unsigned>(cpu_)).isolated;
            static_cast<void>(std::printf("pinned to cpu %d (%s)\n", cpu_,
                                          isolated ? "isolated" : "not isolated"));
        }
        static_cast<void>(std::printf("clock overhead %llu ns, subtracted from each sample\n",
                                      static_cast<unsigned long long>(scale_.to_ns(overhead_))));
        if (!counters.ok()) {
            static_cast<void>(std::printf("hardware counters unavailable (%s)\n",
                                          std::strerror(counters.err())));
        }
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] bool selected(std::string_view name) const noexcept {
        return filter_.empty() || name.find(filter_) != std::string_view::npos;
    }

    [[nodiscard]] std::uint64_t ops(std::uint64_t fallback) const noexcept {
        return ops_ != 0 ? ops_ : fallback;
    }

    [[nodiscard]] int cpu() const noexcept { return cpu_; }
    [[nodiscard]] int cpu2() const noexcept { return cpu2_; }
    [[nodiscard]] const TscScale& scale() const noexcept { return scale_; }
    [[nodiscard]] std::uint64_t ticks_per_second() const noexcept {
        return (std::uint64_t{1'000'000'000} << 32) / scale_.ns_per_tick_q32;
    }

    template <typename Setup, typename Op>
    void run(const char* name, std::uint64_t count, Setup&& setup, Op&& op) noexcept {
        if (!selected(name) || count == 0) {
            return;
        }

        setup();
        for (std::uint64_t i = 0; i < count; ++i) {
            op(i);
        }

        perf::Histogram histogram;
        setup();
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t t0 = tsc_begin();
            op(i);
            const std::uint64_t t1 = tsc_end();
            const std::uint64_t ticks = t1 - t0;
            histogram.record(ticks > overhead_ ? ticks - overhead_ : 0);
        }

        setup();
        perf::CounterSample before{};
        perf::CounterSample after{};
        const bool counting = counters_.is_open() && counters_.start().ok() &&
                              counters_.read(before).ok();
        const std::uint64_t c0 = tsc_begin();
        for (std::uint64_t i = 0; i < count; ++i) {
            op(i);
        }
        const std::uint64_t c1 = tsc_end();
        const bool counted = counting && counters_.read(after).ok() && counters_.stop().ok();

        report(name, histogram);
        const std::uint64_t tenths = scale_.to_ns((c1 - c0) * 10U) / count;
        static_cast<void>(std::printf("  unprobed avg=%llu.%llu ns/op",
                                      static_cast<unsigned long long>(tenths / 10U),
                                      static_cast<unsigned long long>(tenths % 10U)));
        if (counted) {
            const perf::CounterSample d = after - before;
            static_cast<void>(std::printf(
                "  cycles/op=%llu  ins/op=%llu  ipc=%llu.%02llu  cache-miss/1k=%llu  "
                "branch-miss/1k=%llu",
                static_cast<unsigned long long>(d.cycles / count),
                static_cast<unsigned long long>(d.instructions / count),
                static_cast<unsigned long long>(d.instructions_per_kilocycle() / 1000U),
                static_cast<unsigned long long>(d.instructions_per_kilocycle() % 1000U / 10U),
                static_cast<unsigned long long>(d.cache_misses * 1000U / count),
                static_cast<unsigned long long>(d.branch_misses * 1000U / count)));
        }
        static_cast<void>(std::printf("\n"));
        static_cast<void>(std::fflush(stdout));
    }

    void report(const char* name, const perf::Histogram& histogram) const noexcept {
        perf::print(stdout, name, perf::summarize(histogram, scale_));
    }

    void note(const char* text) const noexcept { static_cast<void>(std::printf("%s\n", text)); }

private:
    [[nodiscard]] static std::uint64_t number(std::string_view text) noexcept {
        if (text.empty() || text.size() > 18) {
            die("bad number");
        }
        std::uint64_t value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                die("bad number");
            }
            value = value * 10U + static_cast<std::uint64_t>(c - '0');
        }
        return value;
    }

    perf::HardwareCounters counters_{};
    TscScale scale_{};
    std::uint64_t overhead_{0};
    std::uint64_t ops_{0};
    std::string_view filter_{};
    int cpu_{kNoCpu};
    int cpu2_{kNoCpu};
};

} // namespace hotpath::bench

#endif // HOTPATH_BENCH_SUPPORT_BENCH_HPP
