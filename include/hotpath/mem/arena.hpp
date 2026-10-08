#ifndef HOTPATH_MEM_ARENA_HPP
#define HOTPATH_MEM_ARENA_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

#include "hotpath/core/contract.hpp"
#include "hotpath/mem/region.hpp"

namespace hotpath::mem {

class Arena {
public:
    constexpr Arena() noexcept = default;

    Arena(void* base, std::size_t len) noexcept
        : base_{static_cast<std::byte*>(base)}, len_{len} {
        HOTPATH_ASSERT(base != nullptr && len > 0);
    }

    explicit Arena(const Region& region) noexcept
        : Arena{region.data(), region.size()} {
        HOTPATH_ASSERT(region.prefaulted());
    }

    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t align) noexcept {
        HOTPATH_ASSERT(!frozen_);
        HOTPATH_ASSERT(bytes > 0 && std::has_single_bit(align));

        const auto addr = reinterpret_cast<std::uintptr_t>(base_) + used_;
        const std::size_t pad = static_cast<std::size_t>((align - (addr & (align - 1))) & (align - 1));
        const std::size_t room = len_ - used_;
        if (pad > room || bytes > room - pad) {
            return nullptr;
        }
        void* out = base_ + used_ + pad;
        used_ += pad + bytes;
        return out;
    }

    template <typename T>
        requires std::is_trivially_destructible_v<T> && std::is_default_constructible_v<T>
    [[nodiscard]] T* allocate_array(std::size_t count) noexcept {
        if (count == 0 || count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            return nullptr;
        }
        void* raw = allocate(count * sizeof(T), alignof(T));
        if (raw == nullptr) {
            return nullptr;
        }
        T* first = static_cast<T*>(raw);
        std::uninitialized_value_construct_n(first, count);
        return first;
    }

    void freeze() noexcept { frozen_ = true; }

    [[nodiscard]] bool frozen() const noexcept { return frozen_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return len_; }
    [[nodiscard]] std::size_t used() const noexcept { return used_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return len_ - used_; }

private:
    std::byte* base_{nullptr};
    std::size_t len_{0};
    std::size_t used_{0};
    bool frozen_{false};
};

} // namespace hotpath::mem

#endif // HOTPATH_MEM_ARENA_HPP
