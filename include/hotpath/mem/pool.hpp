#ifndef HOTPATH_MEM_POOL_HPP
#define HOTPATH_MEM_POOL_HPP

#include <cstdint>
#include <type_traits>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/mem/arena.hpp"

namespace hotpath::mem {

using PoolIndex = std::uint32_t;

inline constexpr PoolIndex kNilIndex = 0xFFFF'FFFFU;

template <typename T>
    requires std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> &&
             std::is_default_constructible_v<T>
class Pool {
public:
    constexpr Pool() noexcept = default;

    [[nodiscard]] Status init(Arena& arena, PoolIndex capacity) noexcept {
        HOTPATH_ASSERT(slots_ == nullptr);
        if (capacity == 0 || capacity == kNilIndex) {
            return Status{EINVAL};
        }

        T* slots = arena.allocate_array<T>(capacity);
        PoolIndex* free_stack = arena.allocate_array<PoolIndex>(capacity);
        if (slots == nullptr || free_stack == nullptr) {
            return Status{ENOMEM};
        }

        for (PoolIndex i = 0; i < capacity; ++i) {
            free_stack[i] = capacity - 1U - i;
        }

        slots_ = slots;
        free_ = free_stack;
        capacity_ = capacity;
        free_count_ = capacity;
        return Status{};
    }

    [[nodiscard]] PoolIndex acquire() noexcept {
        HOTPATH_HOT_ASSERT(slots_ != nullptr);
        if (free_count_ == 0) {
            return kNilIndex;
        }
        free_count_ -= 1U;
        return free_[free_count_];
    }

    void release(PoolIndex index) noexcept {
        HOTPATH_HOT_ASSERT(index < capacity_);
        HOTPATH_HOT_ASSERT(free_count_ < capacity_);
        free_[free_count_] = index;
        free_count_ += 1U;
    }

    [[nodiscard]] T& operator[](PoolIndex index) noexcept {
        HOTPATH_HOT_ASSERT(index < capacity_);
        return slots_[index];
    }

    [[nodiscard]] const T& operator[](PoolIndex index) const noexcept {
        HOTPATH_HOT_ASSERT(index < capacity_);
        return slots_[index];
    }

    [[nodiscard]] PoolIndex capacity() const noexcept { return capacity_; }
    [[nodiscard]] PoolIndex available() const noexcept { return free_count_; }
    [[nodiscard]] PoolIndex in_use() const noexcept { return capacity_ - free_count_; }

private:
    T* slots_{nullptr};
    PoolIndex* free_{nullptr};
    PoolIndex capacity_{0};
    PoolIndex free_count_{0};
};

} // namespace hotpath::mem

#endif // HOTPATH_MEM_POOL_HPP
