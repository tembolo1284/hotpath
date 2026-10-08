#ifndef HOTPATH_BOOK_LEVEL_SIDE_HPP
#define HOTPATH_BOOK_LEVEL_SIDE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/mem/arena.hpp"

namespace hotpath::book {

struct Level {
    std::uint32_t ticks{0};
    std::uint32_t orders{0};
    std::uint64_t qty{0};

    [[nodiscard]] constexpr Price price() const noexcept {
        return Price{static_cast<std::int64_t>(ticks)};
    }

    constexpr bool operator==(const Level&) const noexcept = default;
};

static_assert(sizeof(Level) == 16);

template <Side S>
class LevelSide {
public:
    static constexpr std::uint32_t kLinearScan = 8;

    [[nodiscard]] static constexpr std::size_t bytes_for(std::uint32_t capacity) noexcept {
        return std::size_t{capacity} * sizeof(Level) + kCacheLine;
    }

    [[nodiscard]] static constexpr bool better(std::uint32_t a, std::uint32_t b) noexcept {
        return S == Side::Buy ? a > b : a < b;
    }

    [[nodiscard]] Status init(mem::Arena& arena, std::uint32_t capacity) noexcept {
        HOTPATH_ASSERT(levels_ == nullptr);
        if (capacity == 0) {
            return Status{EINVAL};
        }
        void* raw = arena.allocate(std::size_t{capacity} * sizeof(Level), kCacheLine);
        if (raw == nullptr) {
            return Status{ENOMEM};
        }
        levels_ = static_cast<Level*>(raw);
        std::uninitialized_value_construct_n(levels_, capacity);
        capacity_ = capacity;
        return Status{};
    }

    void add(std::uint32_t ticks, std::uint32_t qty) noexcept {
        HOTPATH_HOT_ASSERT(levels_ != nullptr && qty != 0);
        if (beyond_horizon(ticks)) {
            beyond_ += 1U;
            return;
        }

        const std::uint32_t pos = not_better_count(ticks);
        if (pos > 0 && levels_[pos - 1U].ticks == ticks) {
            levels_[pos - 1U].qty += qty;
            levels_[pos - 1U].orders += 1U;
            return;
        }

        if (size_ < capacity_) {
            std::memmove(levels_ + pos + 1U, levels_ + pos,
                         std::size_t{size_ - pos} * sizeof(Level));
            levels_[pos] = Level{ticks, 1U, qty};
            size_ += 1U;
            return;
        }

        if (pos == 0) {
            beyond_ += 1U;
        } else {
            beyond_ += levels_[0].orders;
            std::memmove(levels_, levels_ + 1U, std::size_t{pos - 1U} * sizeof(Level));
            levels_[pos - 1U] = Level{ticks, 1U, qty};
        }
        has_horizon_ = true;
        horizon_ = levels_[0].ticks;
    }

    void reduce(std::uint32_t ticks, std::uint32_t qty, bool order_gone) noexcept {
        HOTPATH_HOT_ASSERT(levels_ != nullptr);
        if (beyond_horizon(ticks)) {
            if (order_gone) {
                HOTPATH_HOT_ASSERT(beyond_ > 0);
                beyond_ -= 1U;
            }
            return;
        }

        const std::uint32_t pos = not_better_count(ticks);
        const bool found = pos > 0 && levels_[pos - 1U].ticks == ticks;
        HOTPATH_HOT_ASSERT(found);
        if (!found) {
            return;
        }

        Level& level = levels_[pos - 1U];
        HOTPATH_HOT_ASSERT(qty <= level.qty);
        level.qty -= qty;
        if (order_gone) {
            HOTPATH_HOT_ASSERT(level.orders > 0);
            level.orders -= 1U;
        }
        if (level.orders == 0) {
            std::memmove(levels_ + pos - 1U, levels_ + pos,
                         std::size_t{size_ - pos} * sizeof(Level));
            size_ -= 1U;
        }
    }

    [[nodiscard]] Level best() const noexcept {
        return size_ > 0 ? levels_[size_ - 1U] : Level{};
    }

    [[nodiscard]] const Level& at(std::uint32_t depth_index) const noexcept {
        HOTPATH_HOT_ASSERT(depth_index < size_);
        return levels_[size_ - 1U - depth_index];
    }

    [[nodiscard]] std::uint32_t depth() const noexcept { return size_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool has_horizon() const noexcept { return has_horizon_; }
    [[nodiscard]] std::uint32_t horizon_ticks() const noexcept { return horizon_; }
    [[nodiscard]] std::uint64_t orders_beyond_horizon() const noexcept { return beyond_; }
    [[nodiscard]] bool degraded() const noexcept { return size_ == 0 && beyond_ > 0; }

private:
    [[nodiscard]] bool beyond_horizon(std::uint32_t ticks) const noexcept {
        return has_horizon_ && better(horizon_, ticks);
    }

    [[nodiscard]] std::uint32_t not_better_count(std::uint32_t ticks) const noexcept {
        std::uint32_t hi = size_;
        for (std::uint32_t step = 0; step < kLinearScan && hi > 0; ++step) {
            if (!better(levels_[hi - 1U].ticks, ticks)) {
                return hi;
            }
            hi -= 1U;
        }

        std::uint32_t lo = 0;
        for (unsigned iter = 0; iter < 32U && lo < hi; ++iter) {
            const std::uint32_t mid = lo + (hi - lo) / 2U;
            if (better(levels_[mid].ticks, ticks)) {
                hi = mid;
            } else {
                lo = mid + 1U;
            }
        }
        return lo;
    }

    Level* levels_{nullptr};
    std::uint32_t size_{0};
    std::uint32_t capacity_{0};
    std::uint32_t horizon_{0};
    bool has_horizon_{false};
    std::uint64_t beyond_{0};
};

} // namespace hotpath::book

#endif // HOTPATH_BOOK_LEVEL_SIDE_HPP
