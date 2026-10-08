#ifndef HOTPATH_PERF_TIMER_HPP
#define HOTPATH_PERF_TIMER_HPP

#include <cstdint>

#include "hotpath/core/clock.hpp"
#include "hotpath/perf/histogram.hpp"

namespace hotpath::perf {

template <typename Clock = TscClock>
class ScopedTimer {
public:
    explicit ScopedTimer(Histogram& histogram) noexcept
        : histogram_{&histogram}, start_{Clock::now()} {}

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    ~ScopedTimer() {
        const std::uint64_t end = Clock::now();
        histogram_->record(end >= start_ ? end - start_ : 0);
    }

private:
    Histogram* histogram_;
    std::uint64_t start_;
};

struct PreciseTscClock {
    [[nodiscard]] [[gnu::always_inline]] static std::uint64_t now() noexcept { return tsc_end(); }
};

[[nodiscard]] inline std::uint64_t measure_clock_overhead(unsigned samples = 10'000) noexcept {
    Histogram overhead;
    for (unsigned i = 0; i < samples; ++i) {
        const std::uint64_t t0 = tsc_begin();
        const std::uint64_t t1 = tsc_end();
        overhead.record(t1 >= t0 ? t1 - t0 : 0);
    }
    return overhead.p50();
}

} // namespace hotpath::perf

#endif // HOTPATH_PERF_TIMER_HPP
