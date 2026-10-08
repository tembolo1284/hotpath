#ifndef HOTPATH_PERF_REPORT_HPP
#define HOTPATH_PERF_REPORT_HPP

#include <cstdint>
#include <cstdio>

#include "hotpath/core/clock.hpp"
#include "hotpath/perf/histogram.hpp"

namespace hotpath::perf {

struct LatencyReport {
    std::uint64_t count{0};
    std::uint64_t min_ns{0};
    std::uint64_t mean_ns{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p90_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
    std::uint64_t max_ns{0};
};

[[nodiscard]] constexpr LatencyReport summarize(const Histogram& histogram,
                                                const TscScale& scale) noexcept {
    return LatencyReport{histogram.count(),
                         scale.to_ns(histogram.min()),
                         scale.to_ns(histogram.mean()),
                         scale.to_ns(histogram.p50()),
                         scale.to_ns(histogram.p90()),
                         scale.to_ns(histogram.p99()),
                         scale.to_ns(histogram.p999()),
                         scale.to_ns(histogram.max())};
}

inline void print(std::FILE* out, const char* name, const LatencyReport& report) noexcept {
    static_cast<void>(std::fprintf(
        out,
        "%-18s n=%-9llu min=%-7llu mean=%-7llu p50=%-7llu p90=%-7llu p99=%-7llu "
        "p99.9=%-7llu max=%llu (ns)\n",
        name, static_cast<unsigned long long>(report.count),
        static_cast<unsigned long long>(report.min_ns),
        static_cast<unsigned long long>(report.mean_ns),
        static_cast<unsigned long long>(report.p50_ns),
        static_cast<unsigned long long>(report.p90_ns),
        static_cast<unsigned long long>(report.p99_ns),
        static_cast<unsigned long long>(report.p999_ns),
        static_cast<unsigned long long>(report.max_ns)));
}

} // namespace hotpath::perf

#endif // HOTPATH_PERF_REPORT_HPP
