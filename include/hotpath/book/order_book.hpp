#ifndef HOTPATH_BOOK_ORDER_BOOK_HPP
#define HOTPATH_BOOK_ORDER_BOOK_HPP

#include <cstddef>
#include <cstdint>

#include "hotpath/book/level_side.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/mem/arena.hpp"

namespace hotpath::book {

struct Top {
    Level bid{};
    Level ask{};

    [[nodiscard]] constexpr bool has_bid() const noexcept { return bid.orders != 0; }
    [[nodiscard]] constexpr bool has_ask() const noexcept { return ask.orders != 0; }
    [[nodiscard]] constexpr bool two_sided() const noexcept { return has_bid() && has_ask(); }

    constexpr bool operator==(const Top&) const noexcept = default;
};

class OrderBook {
public:
    using Bids = LevelSide<Side::Buy>;
    using Asks = LevelSide<Side::Sell>;

    [[nodiscard]] static constexpr std::size_t bytes_for(std::uint32_t levels_per_side) noexcept {
        return Bids::bytes_for(levels_per_side) + Asks::bytes_for(levels_per_side);
    }

    [[nodiscard]] Status init(mem::Arena& arena, std::uint32_t levels_per_side) noexcept {
        const Status st = bids_.init(arena, levels_per_side);
        if (!st.ok()) {
            return st;
        }
        return asks_.init(arena, levels_per_side);
    }

    [[nodiscard]] bool add(Side side, std::uint32_t ticks, std::uint32_t qty) noexcept {
        if (side == Side::Buy) {
            const Level before = bids_.best();
            bids_.add(ticks, qty);
            return before != bids_.best();
        }
        const Level before = asks_.best();
        asks_.add(ticks, qty);
        return before != asks_.best();
    }

    [[nodiscard]] bool reduce(Side side, std::uint32_t ticks, std::uint32_t qty,
                              bool order_gone) noexcept {
        if (side == Side::Buy) {
            const Level before = bids_.best();
            bids_.reduce(ticks, qty, order_gone);
            return before != bids_.best();
        }
        const Level before = asks_.best();
        asks_.reduce(ticks, qty, order_gone);
        return before != asks_.best();
    }

    [[nodiscard]] Top top() const noexcept { return Top{bids_.best(), asks_.best()}; }
    [[nodiscard]] const Bids& bids() const noexcept { return bids_; }
    [[nodiscard]] const Asks& asks() const noexcept { return asks_; }
    [[nodiscard]] bool degraded() const noexcept { return bids_.degraded() || asks_.degraded(); }

private:
    Bids bids_{};
    Asks asks_{};
};

} // namespace hotpath::book

#endif // HOTPATH_BOOK_ORDER_BOOK_HPP
