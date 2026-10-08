#ifndef HOTPATH_BOOK_ORDER_MAP_HPP
#define HOTPATH_BOOK_ORDER_MAP_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/mem/arena.hpp"

namespace hotpath::book {

inline constexpr std::uint32_t kMaxTicks = 0x7FFF'FFFFU;

struct OrderEntry {
    std::uint64_t id{0};
    std::uint32_t packed{0};
    std::uint32_t qty{0};

    [[nodiscard]] constexpr std::uint32_t ticks() const noexcept { return packed & kMaxTicks; }

    [[nodiscard]] constexpr Side side() const noexcept {
        return (packed >> 31) != 0 ? Side::Sell : Side::Buy;
    }

    [[nodiscard]] constexpr bool live() const noexcept { return qty != 0; }

    [[nodiscard]] static constexpr std::uint32_t pack(std::uint32_t ticks, Side side) noexcept {
        return (ticks & kMaxTicks) | (side == Side::Sell ? 0x8000'0000U : 0U);
    }
};

static_assert(sizeof(OrderEntry) == 16);
static_assert(kCacheLine % sizeof(OrderEntry) == 0);

class OrderMap {
public:
    static constexpr std::uint32_t kMinCapacity = 16;

    struct Insert {
        OrderEntry* entry{nullptr};
        bool existed{false};
    };

    [[nodiscard]] static constexpr std::size_t bytes_for(std::uint32_t capacity) noexcept {
        return std::size_t{capacity} * sizeof(OrderEntry) + kCacheLine;
    }

    [[nodiscard]] Status init(mem::Arena& arena, std::uint32_t capacity) noexcept {
        HOTPATH_ASSERT(slots_ == nullptr);
        if (capacity < kMinCapacity || !std::has_single_bit(capacity)) {
            return Status{EINVAL};
        }
        void* raw = arena.allocate(std::size_t{capacity} * sizeof(OrderEntry), kCacheLine);
        if (raw == nullptr) {
            return Status{ENOMEM};
        }
        slots_ = static_cast<OrderEntry*>(raw);
        std::uninitialized_value_construct_n(slots_, capacity);
        capacity_ = capacity;
        mask_ = capacity - 1U;
        shift_ = 64U - static_cast<unsigned>(std::countr_zero(capacity));
        max_load_ = capacity - capacity / 8U;
        return Status{};
    }

    [[gnu::always_inline]] void prefetch(std::uint64_t id) const noexcept {
        __builtin_prefetch(slots_ + home(id), 1, 3);
    }

    [[nodiscard]] OrderEntry* find(std::uint64_t id) noexcept {
        std::uint32_t index = home(id);
        for (std::uint32_t probe = 0; probe < capacity_; ++probe) {
            OrderEntry& entry = slots_[index];
            if (!entry.live()) {
                return nullptr;
            }
            if (entry.id == id) {
                return &entry;
            }
            index = (index + 1U) & mask_;
        }
        return nullptr;
    }

    [[nodiscard]] Insert insert(std::uint64_t id, std::uint32_t ticks, Side side,
                                std::uint32_t qty) noexcept {
        HOTPATH_HOT_ASSERT(qty != 0 && ticks <= kMaxTicks);
        std::uint32_t index = home(id);
        for (std::uint32_t probe = 0; probe < capacity_; ++probe) {
            OrderEntry& entry = slots_[index];
            if (!entry.live()) {
                if (size_ >= max_load_) {
                    return Insert{nullptr, false};
                }
                entry.id = id;
                entry.packed = OrderEntry::pack(ticks, side);
                entry.qty = qty;
                size_ += 1U;
                return Insert{&entry, false};
            }
            if (entry.id == id) {
                return Insert{&entry, true};
            }
            index = (index + 1U) & mask_;
        }
        return Insert{nullptr, false};
    }

    void erase(OrderEntry* entry) noexcept {
        HOTPATH_HOT_ASSERT(entry != nullptr && entry >= slots_ && entry < slots_ + capacity_);
        HOTPATH_HOT_ASSERT(entry->live());

        std::uint32_t hole = static_cast<std::uint32_t>(entry - slots_);
        std::uint32_t next = hole;
        for (std::uint32_t step = 0; step < capacity_; ++step) {
            next = (next + 1U) & mask_;
            const OrderEntry& candidate = slots_[next];
            if (!candidate.live()) {
                break;
            }
            const std::uint32_t from_home = (next - home(candidate.id)) & mask_;
            const std::uint32_t from_hole = (next - hole) & mask_;
            if (from_home >= from_hole) {
                slots_[hole] = candidate;
                hole = next;
            }
        }
        slots_[hole] = OrderEntry{};
        size_ -= 1U;
    }

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t max_load() const noexcept { return max_load_; }

private:
    [[nodiscard]] [[gnu::always_inline]] std::uint32_t home(std::uint64_t id) const noexcept {
        return static_cast<std::uint32_t>((id * 0x9E37'79B9'7F4A'7C15ULL) >> shift_);
    }

    OrderEntry* slots_{nullptr};
    std::uint32_t capacity_{0};
    std::uint32_t mask_{0};
    std::uint32_t size_{0};
    std::uint32_t max_load_{0};
    unsigned shift_{0};
};

} // namespace hotpath::book

#endif // HOTPATH_BOOK_ORDER_MAP_HPP
