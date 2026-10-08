#ifndef HOTPATH_SIM_MATCHING_ENGINE_HPP
#define HOTPATH_SIM_MATCHING_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/pool.hpp"
#include "hotpath/wire/itch.hpp"

namespace hotpath::sim {

struct RestingOrder {
    std::uint64_t ref{0};
    std::uint64_t user{0};
    std::uint32_t qty{0};
    std::uint32_t ticks{0};
    mem::PoolIndex prev{mem::kNilIndex};
    mem::PoolIndex next{mem::kNilIndex};
    std::uint16_t symbol{0};
    Side side{Side::Buy};
    bool live{false};
};

struct OrderHandle {
    std::uint64_t ref{0};
    mem::PoolIndex index{mem::kNilIndex};

    [[nodiscard]] constexpr bool valid() const noexcept { return index != mem::kNilIndex; }
};

struct PriceLevel {
    std::uint32_t ticks{0};
    std::uint32_t orders{0};
    std::uint64_t qty{0};
    mem::PoolIndex head{mem::kNilIndex};
    mem::PoolIndex tail{mem::kNilIndex};
};

struct EngineConfig {
    std::uint16_t symbols{0};
    std::uint32_t max_orders{0};
    std::uint32_t levels_per_side{0};
};

struct Submitted {
    bool accepted{false};
    std::uint64_t ref{0};
    OrderHandle resting{};
    std::uint32_t filled{0};
    std::uint32_t canceled{0};
};

class MatchingEngine {
public:
    [[nodiscard]] Status init(mem::Arena& arena, const EngineConfig& config) noexcept {
        HOTPATH_ASSERT(sides_ == nullptr);
        if (config.symbols == 0 || config.levels_per_side == 0) {
            return Status{EINVAL};
        }
        Status st = orders_.init(arena, config.max_orders);
        if (!st.ok()) {
            return st;
        }
        const std::uint32_t side_count = std::uint32_t{config.symbols} * 2U;
        SideBook* sides = arena.allocate_array<SideBook>(side_count);
        if (sides == nullptr) {
            return Status{ENOMEM};
        }
        for (std::uint32_t i = 0; i < side_count; ++i) {
            sides[i].levels = arena.allocate_array<PriceLevel>(config.levels_per_side);
            if (sides[i].levels == nullptr) {
                return Status{ENOMEM};
            }
        }
        sides_ = sides;
        symbols_ = config.symbols;
        max_orders_ = config.max_orders;
        levels_per_side_ = config.levels_per_side;
        return Status{};
    }

    [[nodiscard]] std::uint64_t next_ref() const noexcept { return next_ref_; }
    [[nodiscard]] std::uint64_t next_match() const noexcept { return next_match_; }
    [[nodiscard]] std::uint16_t symbols() const noexcept { return symbols_; }
    [[nodiscard]] std::uint32_t resting_orders() const noexcept { return orders_.in_use(); }

    template <typename L>
    Submitted submit(L& listener, std::uint16_t symbol, Side side, std::uint32_t qty,
                     std::uint32_t ticks, bool immediate_or_cancel, std::uint64_t user) noexcept {
        Submitted out{};
        if (symbol >= symbols_ || qty == 0 || ticks == 0 || ticks > wire::itch::kMaxPrice) {
            return out;
        }
        out.accepted = true;
        out.ref = next_ref_;
        next_ref_ += 1;

        std::uint32_t remaining = qty;
        SideBook& against = book(symbol, opposite(side));
        for (std::uint32_t i = 0; i < max_orders_ && remaining > 0 && against.size > 0; ++i) {
            const std::uint32_t level_index = against.size - 1U;
            PriceLevel& level = against.levels[level_index];
            const bool crosses = side == Side::Buy ? level.ticks <= ticks : level.ticks >= ticks;
            if (!crosses) {
                break;
            }
            const mem::PoolIndex index = level.head;
            RestingOrder& resting = orders_[index];
            const std::uint32_t fill = remaining < resting.qty ? remaining : resting.qty;
            const std::uint32_t price = level.ticks;
            resting.qty -= fill;
            level.qty -= fill;
            remaining -= fill;
            const bool done = resting.qty == 0;
            const std::uint64_t match = next_match_;
            next_match_ += 1;
            listener.on_trade(resting, out.ref, user, side, fill, price, match, done);
            if (done) {
                remove(against, level_index, index);
            }
        }

        out.filled = qty - remaining;
        if (remaining == 0) {
            return out;
        }
        if (immediate_or_cancel) {
            out.canceled = remaining;
            return out;
        }

        SideBook& own = book(symbol, side);
        const std::uint32_t pos = not_better_count(own, side, ticks);
        const bool exists = pos > 0 && own.levels[pos - 1U].ticks == ticks;
        if (!exists && own.size == levels_per_side_) {
            out.canceled = remaining;
            return out;
        }
        const mem::PoolIndex index = orders_.acquire();
        if (index == mem::kNilIndex) {
            out.canceled = remaining;
            return out;
        }

        std::uint32_t level_index = pos - 1U;
        if (!exists) {
            std::memmove(own.levels + pos + 1U, own.levels + pos,
                         std::size_t{own.size - pos} * sizeof(PriceLevel));
            own.levels[pos] = PriceLevel{ticks, 0, 0, mem::kNilIndex, mem::kNilIndex};
            own.size += 1U;
            level_index = pos;
        }
        PriceLevel& level = own.levels[level_index];
        RestingOrder& node = orders_[index];
        node = RestingOrder{out.ref, user, remaining, ticks, level.tail, mem::kNilIndex,
                            symbol, side, true};
        if (level.tail != mem::kNilIndex) {
            orders_[level.tail].next = index;
        } else {
            level.head = index;
        }
        level.tail = index;
        level.orders += 1U;
        level.qty += remaining;

        out.resting = OrderHandle{out.ref, index};
        listener.on_rest(node);
        return out;
    }

    template <typename L>
    std::uint32_t reduce(L& listener, OrderHandle handle, std::uint32_t new_qty) noexcept {
        RestingOrder* node = lookup(handle);
        if (node == nullptr || new_qty >= node->qty) {
            return 0;
        }
        SideBook& own = book(node->symbol, node->side);
        const std::uint32_t pos = not_better_count(own, node->side, node->ticks);
        HOTPATH_ASSERT(pos > 0 && own.levels[pos - 1U].ticks == node->ticks);

        const std::uint32_t removed = node->qty - new_qty;
        node->qty = new_qty;
        own.levels[pos - 1U].qty -= removed;
        const bool gone = new_qty == 0;
        listener.on_reduce(*node, removed, gone);
        if (gone) {
            remove(own, pos - 1U, handle.index);
        }
        return removed;
    }

    [[nodiscard]] const RestingOrder* find(OrderHandle handle) const noexcept {
        if (!handle.valid() || handle.index >= orders_.capacity()) {
            return nullptr;
        }
        const RestingOrder& node = orders_[handle.index];
        return node.live && node.ref == handle.ref ? &node : nullptr;
    }

    [[nodiscard]] PriceLevel best(std::uint16_t symbol, Side side) const noexcept {
        HOTPATH_ASSERT(symbol < symbols_);
        const SideBook& own = sides_[std::size_t{symbol} * 2U + (side == Side::Sell ? 1U : 0U)];
        return own.size > 0 ? own.levels[own.size - 1U] : PriceLevel{};
    }

    [[nodiscard]] std::uint32_t depth(std::uint16_t symbol, Side side) const noexcept {
        HOTPATH_ASSERT(symbol < symbols_);
        return sides_[std::size_t{symbol} * 2U + (side == Side::Sell ? 1U : 0U)].size;
    }

private:
    struct SideBook {
        PriceLevel* levels{nullptr};
        std::uint32_t size{0};
    };

    [[nodiscard]] SideBook& book(std::uint16_t symbol, Side side) noexcept {
        return sides_[std::size_t{symbol} * 2U + (side == Side::Sell ? 1U : 0U)];
    }

    [[nodiscard]] RestingOrder* lookup(OrderHandle handle) noexcept {
        if (!handle.valid() || handle.index >= orders_.capacity()) {
            return nullptr;
        }
        RestingOrder& node = orders_[handle.index];
        return node.live && node.ref == handle.ref ? &node : nullptr;
    }

    [[nodiscard]] static bool better(Side side, std::uint32_t a, std::uint32_t b) noexcept {
        return side == Side::Buy ? a > b : a < b;
    }

    [[nodiscard]] static std::uint32_t not_better_count(const SideBook& own, Side side,
                                                        std::uint32_t ticks) noexcept {
        std::uint32_t lo = 0;
        std::uint32_t hi = own.size;
        for (unsigned iter = 0; iter < 32U && lo < hi; ++iter) {
            const std::uint32_t mid = lo + (hi - lo) / 2U;
            if (better(side, own.levels[mid].ticks, ticks)) {
                hi = mid;
            } else {
                lo = mid + 1U;
            }
        }
        return lo;
    }

    void remove(SideBook& own, std::uint32_t level_index, mem::PoolIndex index) noexcept {
        PriceLevel& level = own.levels[level_index];
        RestingOrder& node = orders_[index];
        if (node.prev != mem::kNilIndex) {
            orders_[node.prev].next = node.next;
        } else {
            level.head = node.next;
        }
        if (node.next != mem::kNilIndex) {
            orders_[node.next].prev = node.prev;
        } else {
            level.tail = node.prev;
        }
        node.live = false;
        orders_.release(index);
        level.orders -= 1U;
        if (level.orders == 0) {
            std::memmove(own.levels + level_index, own.levels + level_index + 1U,
                         std::size_t{own.size - level_index - 1U} * sizeof(PriceLevel));
            own.size -= 1U;
        }
    }

    mem::Pool<RestingOrder> orders_{};
    SideBook* sides_{nullptr};
    std::uint64_t next_ref_{1};
    std::uint64_t next_match_{1};
    std::uint32_t max_orders_{0};
    std::uint32_t levels_per_side_{0};
    std::uint16_t symbols_{0};
};

} // namespace hotpath::sim

#endif // HOTPATH_SIM_MATCHING_ENGINE_HPP
