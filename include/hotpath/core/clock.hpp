#ifndef HOTPATH_CORE_CLOCK_HPP
#define HOTPATH_CORE_CLOCK_HPP

#if !defined(__x86_64__)
#error "hotpath/core/clock.hpp requires x86-64"
#endif

#include <cpuid.h>
#include <time.h>
#include <x86intrin.h>

#include <cstdint>

#include "hotpath/core/contract.hpp"

namespace hotpath {

namespace detail {
__extension__ typedef unsigned __int128 u128;
} // namespace detail

[[nodiscard]] [[gnu::always_inline]] inline std::uint64_t tsc_now() noexcept {
    return __rdtsc();
}

[[nodiscard]] [[gnu::always_inline]] inline std::uint64_t tsc_begin() noexcept {
    _mm_lfence();
    return __rdtsc();
}

[[nodiscard]] [[gnu::always_inline]] inline std::uint64_t tsc_end() noexcept {
    unsigned int aux = 0;
    const std::uint64_t ticks = __rdtscp(&aux);
    _mm_lfence();
    return ticks;
}

[[nodiscard]] inline bool has_invariant_tsc() noexcept {
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    const int ok = __get_cpuid(0x80000007U, &eax, &ebx, &ecx, &edx);
    return ok != 0 && (edx & (1U << 8)) != 0;
}

[[nodiscard]] inline std::uint64_t monotonic_ns() noexcept {
    timespec ts{};
    const int rc = ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    HOTPATH_ASSERT(rc == 0);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

struct TscClock {
    [[nodiscard]] [[gnu::always_inline]] static std::uint64_t now() noexcept { return tsc_now(); }
};

struct TscScale {
    std::uint64_t ns_per_tick_q32{0};

    [[nodiscard]] constexpr std::uint64_t to_ns(std::uint64_t ticks) const noexcept {
        return static_cast<std::uint64_t>(
            (static_cast<detail::u128>(ticks) * ns_per_tick_q32) >> 32);
    }
};

[[nodiscard]] inline TscScale calibrate_tsc(
    std::uint64_t interval_ns = 50'000'000ULL) noexcept {
    HOTPATH_ASSERT(interval_ns > 0 && interval_ns < 1'000'000'000ULL);
    HOTPATH_ASSERT(has_invariant_tsc());

    timespec req{};
    req.tv_sec = 0;
    req.tv_nsec = static_cast<long>(interval_ns);

    const std::uint64_t ns0 = monotonic_ns();
    const std::uint64_t c0 = tsc_begin();
    static_cast<void>(::nanosleep(&req, nullptr));
    const std::uint64_t c1 = tsc_end();
    const std::uint64_t ns1 = monotonic_ns();

    HOTPATH_ASSERT(c1 > c0);
    HOTPATH_ASSERT(ns1 > ns0 && (ns1 - ns0) < (1ULL << 32));

    return TscScale{((ns1 - ns0) << 32) / (c1 - c0)};
}

} // namespace hotpath

#endif // HOTPATH_CORE_CLOCK_HPP
