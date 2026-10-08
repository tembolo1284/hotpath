#ifndef HOTPATH_SYS_MEMORY_HPP
#define HOTPATH_SYS_MEMORY_HPP

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <limits>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"

namespace hotpath::sys {

enum class PageKind : std::uint8_t { Small, Huge2M, Huge1G };

[[nodiscard]] constexpr unsigned page_shift(PageKind kind) noexcept {
    switch (kind) {
        case PageKind::Small:  return 12U;
        case PageKind::Huge2M: return 21U;
        case PageKind::Huge1G: return 30U;
    }
    return 12U;
}

[[nodiscard]] constexpr std::size_t page_bytes(PageKind kind) noexcept {
    return std::size_t{1} << page_shift(kind);
}

struct Mapping {
    void* addr{nullptr};
    std::size_t len{0};
    PageKind kind{PageKind::Small};
    Status status{};
};

[[nodiscard]] inline Mapping map_anonymous(std::size_t len, PageKind kind) noexcept {
    const std::size_t page = page_bytes(kind);
    if (len == 0 || len > std::numeric_limits<std::size_t>::max() - (page - 1)) {
        return Mapping{nullptr, 0, kind, Status{EINVAL}};
    }
    const std::size_t rounded = (len + page - 1) & ~(page - 1);

    unsigned flags = static_cast<unsigned>(MAP_PRIVATE | MAP_ANONYMOUS);
    if (kind != PageKind::Small) {
        flags |= static_cast<unsigned>(MAP_HUGETLB);
        flags |= page_shift(kind) << static_cast<unsigned>(MAP_HUGE_SHIFT);
    }

    void* addr = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                        static_cast<int>(flags), -1, 0);
    if (addr == MAP_FAILED) {
        return Mapping{nullptr, 0, kind, Status::from_errno()};
    }
    return Mapping{addr, rounded, kind, Status{}};
}

inline void prefault(const Mapping& m) noexcept {
    HOTPATH_ASSERT(m.addr != nullptr && m.status.ok());
    const std::size_t page = page_bytes(m.kind);
    const std::size_t pages = m.len / page;
    auto* base = static_cast<volatile unsigned char*>(m.addr);
    for (std::size_t i = 0; i < pages; ++i) {
        const unsigned char byte = base[i * page];
        base[i * page] = byte;
    }
}

[[nodiscard]] inline Status unmap(Mapping& m) noexcept {
    if (m.addr == nullptr) {
        return Status{EINVAL};
    }
    if (::munmap(m.addr, m.len) != 0) {
        return Status::from_errno();
    }
    m = Mapping{};
    return Status{};
}

[[nodiscard]] inline Status lock_all_memory() noexcept {
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        return Status::from_errno();
    }
    return Status{};
}

} // namespace hotpath::sys

#endif // HOTPATH_SYS_MEMORY_HPP
