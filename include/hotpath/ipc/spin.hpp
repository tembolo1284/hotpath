#ifndef HOTPATH_IPC_SPIN_HPP
#define HOTPATH_IPC_SPIN_HPP

#if !defined(__x86_64__)
#error "hotpath/ipc/spin.hpp requires x86-64"
#endif

#include <immintrin.h>

#include <concepts>
#include <cstdint>

namespace hotpath::ipc {

[[gnu::always_inline]] inline void cpu_relax() noexcept {
    _mm_pause();
}

template <std::predicate F>
[[nodiscard]] inline bool spin_until(F&& ready, std::uint64_t max_spins) noexcept {
    for (std::uint64_t i = 0; i < max_spins; ++i) {
        if (ready()) {
            return true;
        }
        cpu_relax();
    }
    return ready();
}

} // namespace hotpath::ipc

#endif // HOTPATH_IPC_SPIN_HPP
