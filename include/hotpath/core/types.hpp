#ifndef HOTPATH_CORE_TYPES_HPP
#define HOTPATH_CORE_TYPES_HPP

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "hotpath/core/contract.hpp"

namespace hotpath {

inline constexpr std::size_t kCacheLine = 64;

template <typename Tag, std::unsigned_integral Rep>
class Id {
public:
    using rep_type = Rep;

    constexpr Id() noexcept = default;
    constexpr explicit Id(Rep value) noexcept : value_{value} {}

    [[nodiscard]] constexpr Rep raw() const noexcept { return value_; }

    constexpr auto operator<=>(const Id&) const noexcept = default;

private:
    Rep value_{};
};

using SymbolId = Id<struct SymbolIdTag, std::uint16_t>;
using OrderId  = Id<struct OrderIdTag, std::uint64_t>;
using SeqNo    = Id<struct SeqNoTag, std::uint64_t>;

class Price {
public:
    constexpr Price() noexcept = default;
    constexpr explicit Price(std::int64_t ticks) noexcept : ticks_{ticks} {}

    [[nodiscard]] constexpr std::int64_t ticks() const noexcept { return ticks_; }

    [[nodiscard]] constexpr Price offset(std::int64_t n) const noexcept {
        return Price{ticks_ + n};
    }

    constexpr auto operator<=>(const Price&) const noexcept = default;

private:
    std::int64_t ticks_{};
};

[[nodiscard]] constexpr std::int64_t operator-(Price a, Price b) noexcept {
    return a.ticks() - b.ticks();
}

class Qty {
public:
    constexpr Qty() noexcept = default;
    constexpr explicit Qty(std::uint32_t units) noexcept : units_{units} {}

    [[nodiscard]] constexpr std::uint32_t units() const noexcept { return units_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return units_ == 0; }

    constexpr Qty& operator+=(Qty rhs) noexcept {
        HOTPATH_HOT_ASSERT(rhs.units_ <= std::numeric_limits<std::uint32_t>::max() - units_);
        units_ += rhs.units_;
        return *this;
    }

    constexpr Qty& operator-=(Qty rhs) noexcept {
        HOTPATH_HOT_ASSERT(rhs.units_ <= units_);
        units_ -= rhs.units_;
        return *this;
    }

    constexpr auto operator<=>(const Qty&) const noexcept = default;

private:
    std::uint32_t units_{};
};

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

template <typename T>
concept ShmSafe = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

static_assert(ShmSafe<SymbolId> && sizeof(SymbolId) == 2);
static_assert(ShmSafe<OrderId> && sizeof(OrderId) == 8);
static_assert(ShmSafe<SeqNo> && sizeof(SeqNo) == 8);
static_assert(ShmSafe<Price> && sizeof(Price) == 8);
static_assert(ShmSafe<Qty> && sizeof(Qty) == 4);
static_assert(sizeof(Side) == 1);

} // namespace hotpath

#endif // HOTPATH_CORE_TYPES_HPP
