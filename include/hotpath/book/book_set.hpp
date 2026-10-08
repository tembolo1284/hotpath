#ifndef HOTPATH_BOOK_BOOK_SET_HPP
#define HOTPATH_BOOK_BOOK_SET_HPP

#include <cstddef>
#include <cstdint>

#include "hotpath/book/order_book.hpp"
#include "hotpath/book/order_map.hpp"
#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/mem/arena.hpp"

namespace hotpath::book {

enum class Result : std::uint8_t {
    Ok,
    UnknownOrder,
    DuplicateOrder,
    OrdersFull,
    BadSymbol,
    Invalid,
};

struct Update {
    Result result{Result::Ok};
    bool top_changed{false};

    [[nodiscard]] constexpr bool ok() const noexcept { return result == Result::Ok; }
};

struct BookConfig {
    std::uint32_t max_symbols{0};
    std::uint32_t max_orders{0};
    std::uint32_t levels_per_side{0};
};

class BookSet {
public:
    [[nodiscard]] static constexpr std::size_t bytes_for(const BookConfig& config) noexcept {
        return OrderMap::bytes_for(config.max_orders) +
               std::size_t{config.max_symbols} *
                   (sizeof(OrderBook) + OrderBook::bytes_for(config.levels_per_side)) +
               kCacheLine;
    }

    [[nodiscard]] Status init(mem::Arena& arena, const BookConfig& config) noexcept {
        HOTPATH_ASSERT(books_ == nullptr);
        if (config.max_symbols == 0 || config.max_symbols > 65'536U || config.levels_per_side == 0) {
            return Status{EINVAL};
        }

        Status st = orders_.init(arena, config.max_orders);
        if (!st.ok()) {
            return st;
        }

        OrderBook* books = arena.allocate_array<OrderBook>(config.max_symbols);
        if (books == nullptr) {
            return Status{ENOMEM};
        }
        for (std::uint32_t i = 0; i < config.max_symbols; ++i) {
            st = books[i].init(arena, config.levels_per_side);
            if (!st.ok()) {
                return st;
            }
        }

        books_ = books;
        symbols_ = config.max_symbols;
        return Status{};
    }

    [[gnu::always_inline]] void prefetch(OrderId id) const noexcept { orders_.prefetch(id.raw()); }

    [[nodiscard]] Update add(SymbolId symbol, OrderId id, Side side, Qty qty, Price price) noexcept {
        if (symbol.raw() >= symbols_) {
            return Update{Result::BadSymbol, false};
        }
        if (qty.is_zero() || price.ticks() < 0 || price.ticks() > kMaxTicks) {
            return Update{Result::Invalid, false};
        }
        const auto ticks = static_cast<std::uint32_t>(price.ticks());

        const OrderMap::Insert slot = orders_.insert(id.raw(), ticks, side, qty.units());
        if (slot.existed) {
            return Update{Result::DuplicateOrder, false};
        }
        if (slot.entry == nullptr) {
            return Update{Result::OrdersFull, false};
        }
        return Update{Result::Ok, books_[symbol.raw()].add(side, ticks, qty.units())};
    }

    [[nodiscard]] Update reduce(SymbolId symbol, OrderId id, Qty qty) noexcept {
        if (symbol.raw() >= symbols_) {
            return Update{Result::BadSymbol, false};
        }
        OrderEntry* entry = orders_.find(id.raw());
        if (entry == nullptr) {
            return Update{Result::UnknownOrder, false};
        }
        if (qty.is_zero() || qty.units() > entry->qty) {
            return Update{Result::Invalid, false};
        }

        const bool gone = qty.units() == entry->qty;
        const bool changed =
            books_[symbol.raw()].reduce(entry->side(), entry->ticks(), qty.units(), gone);
        if (gone) {
            orders_.erase(entry);
        } else {
            entry->qty -= qty.units();
        }
        return Update{Result::Ok, changed};
    }

    [[nodiscard]] Update remove(SymbolId symbol, OrderId id) noexcept {
        if (symbol.raw() >= symbols_) {
            return Update{Result::BadSymbol, false};
        }
        OrderEntry* entry = orders_.find(id.raw());
        if (entry == nullptr) {
            return Update{Result::UnknownOrder, false};
        }
        const bool changed =
            books_[symbol.raw()].reduce(entry->side(), entry->ticks(), entry->qty, true);
        orders_.erase(entry);
        return Update{Result::Ok, changed};
    }

    [[nodiscard]] Update replace(SymbolId symbol, OrderId old_id, OrderId new_id, Qty qty,
                                 Price price) noexcept {
        if (symbol.raw() >= symbols_) {
            return Update{Result::BadSymbol, false};
        }
        if (qty.is_zero() || price.ticks() < 0 || price.ticks() > kMaxTicks) {
            return Update{Result::Invalid, false};
        }
        OrderEntry* old_entry = orders_.find(old_id.raw());
        if (old_entry == nullptr) {
            return Update{Result::UnknownOrder, false};
        }
        if (new_id != old_id && orders_.find(new_id.raw()) != nullptr) {
            return Update{Result::DuplicateOrder, false};
        }

        const Side side = old_entry->side();
        const auto ticks = static_cast<std::uint32_t>(price.ticks());
        OrderBook& book = books_[symbol.raw()];

        bool changed = book.reduce(side, old_entry->ticks(), old_entry->qty, true);
        orders_.erase(old_entry);

        const OrderMap::Insert slot = orders_.insert(new_id.raw(), ticks, side, qty.units());
        HOTPATH_HOT_ASSERT(slot.entry != nullptr && !slot.existed);
        if (slot.entry == nullptr || slot.existed) {
            return Update{Result::OrdersFull, changed};
        }
        changed = book.add(side, ticks, qty.units()) || changed;
        return Update{Result::Ok, changed};
    }

    [[nodiscard]] const OrderBook* book(SymbolId symbol) const noexcept {
        return symbol.raw() < symbols_ ? &books_[symbol.raw()] : nullptr;
    }

    [[nodiscard]] const OrderEntry* order(OrderId id) noexcept { return orders_.find(id.raw()); }
    [[nodiscard]] std::uint32_t open_orders() const noexcept { return orders_.size(); }
    [[nodiscard]] std::uint32_t symbols() const noexcept { return symbols_; }

private:
    OrderMap orders_{};
    OrderBook* books_{nullptr};
    std::uint32_t symbols_{0};
};

} // namespace hotpath::book

#endif // HOTPATH_BOOK_BOOK_SET_HPP
