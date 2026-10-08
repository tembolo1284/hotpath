#ifndef HOTPATH_FEED_BOOK_BUILDER_HPP
#define HOTPATH_FEED_BOOK_BUILDER_HPP

#include <cstdint>

#include "hotpath/book/book_set.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/feed/symbol_table.hpp"
#include "hotpath/wire/itch.hpp"

namespace hotpath::feed {

struct BuilderStats {
    std::uint64_t messages{0};
    std::uint64_t published{0};
    std::uint64_t dropped{0};
    std::uint64_t unknown_orders{0};
    std::uint64_t rejected{0};
};

template <UpdateSink Sink>
class BookBuilder {
public:
    BookBuilder(book::BookSet& books, SymbolTable& symbols, Sink& sink) noexcept
        : books_{&books}, symbols_{&symbols}, sink_{&sink} {}

    void begin_message(std::uint64_t sequence, std::uint64_t rx_tsc) noexcept {
        sequence_ = sequence;
        rx_tsc_ = rx_tsc;
        stats_.messages += 1;
    }

    void set_feed_flags(std::uint8_t flags) noexcept { feed_flags_ = flags; }
    [[nodiscard]] std::uint8_t feed_flags() const noexcept { return feed_flags_; }

    void publish_feed_status(std::uint64_t sequence, std::uint64_t rx_tsc) noexcept {
        MarketUpdate* out = sink_->claim();
        if (out == nullptr) {
            stats_.dropped += 1;
            return;
        }
        *out = MarketUpdate{};
        out->rx_tsc = rx_tsc;
        out->sequence = sequence;
        out->kind = Kind::FeedStatus;
        out->flags = feed_flags_;
        sink_->publish();
        stats_.published += 1;
    }

    void on(const wire::itch::StockDirectory& m) noexcept {
        static_cast<void>(symbols_->resolve(m.hdr.stock_locate.get(), m.stock));
    }

    void on(const wire::itch::StockTradingAction& m) noexcept {
        const std::uint16_t sym = symbols_->lookup(m.hdr.stock_locate.get());
        if (sym == SymbolTable::kNone) {
            return;
        }
        symbols_->set_trading_state(SymbolId{sym}, m.trading_state);
        publish(Kind::TradingStatus, sym, m.hdr, 0, 0);
    }

    void on(const wire::itch::AddOrder& m) noexcept {
        add(m.hdr, m.order_ref.get(), m.side, m.shares.get(), m.price.get());
    }

    void on(const wire::itch::AddOrderMpid& m) noexcept {
        add(m.hdr, m.order_ref.get(), m.side, m.shares.get(), m.price.get());
    }

    void on(const wire::itch::OrderExecuted& m) noexcept {
        const std::uint16_t sym = symbols_->lookup(m.hdr.stock_locate.get());
        if (sym == SymbolTable::kNone) {
            return;
        }
        const OrderId id{m.order_ref.get()};
        const book::OrderEntry* entry = books_->order(id);
        const std::uint32_t ticks = entry != nullptr ? entry->ticks() : 0U;
        const std::uint32_t shares = m.executed_shares.get();
        if (accepted(books_->reduce(SymbolId{sym}, id, Qty{shares}))) {
            publish(Kind::Trade, sym, m.hdr, ticks, shares);
        }
    }

    void on(const wire::itch::OrderExecutedWithPrice& m) noexcept {
        const std::uint16_t sym = symbols_->lookup(m.hdr.stock_locate.get());
        if (sym == SymbolTable::kNone) {
            return;
        }
        const std::uint32_t shares = m.executed_shares.get();
        const book::Update up = books_->reduce(SymbolId{sym}, OrderId{m.order_ref.get()}, Qty{shares});
        if (!accepted(up)) {
            return;
        }
        if (m.printable == 'Y') {
            publish(Kind::Trade, sym, m.hdr, m.execution_price.get(), shares);
        } else if (up.top_changed) {
            publish(Kind::TopOfBook, sym, m.hdr, 0, 0);
        }
    }

    void on(const wire::itch::OrderCancel& m) noexcept {
        const std::uint16_t sym = symbols_->lookup(m.hdr.stock_locate.get());
        if (sym == SymbolTable::kNone) {
            return;
        }
        top_update(sym, m.hdr,
                   books_->reduce(SymbolId{sym}, OrderId{m.order_ref.get()},
                                  Qty{m.cancelled_shares.get()}));
    }

    void on(const wire::itch::OrderDelete& m) noexcept {
        const std::uint16_t sym = symbols_->lookup(m.hdr.stock_locate.get());
        if (sym == SymbolTable::kNone) {
            return;
        }
        top_update(sym, m.hdr, books_->remove(SymbolId{sym}, OrderId{m.order_ref.get()}));
    }

    void on(const wire::itch::OrderReplace& m) noexcept {
        const std::uint16_t sym = symbols_->lookup(m.hdr.stock_locate.get());
        if (sym == SymbolTable::kNone) {
            return;
        }
        top_update(sym, m.hdr,
                   books_->replace(SymbolId{sym}, OrderId{m.original_order_ref.get()},
                                   OrderId{m.new_order_ref.get()}, Qty{m.shares.get()},
                                   Price{static_cast<std::int64_t>(m.price.get())}));
    }

    void on(const wire::itch::Trade& m) noexcept {
        const std::uint16_t sym = symbols_->lookup(m.hdr.stock_locate.get());
        if (sym == SymbolTable::kNone) {
            return;
        }
        publish(Kind::Trade, sym, m.hdr, m.price.get(), m.shares.get());
    }

    [[nodiscard]] const BuilderStats& stats() const noexcept { return stats_; }

private:
    void add(const wire::itch::Header& hdr, std::uint64_t ref, char side_code,
             std::uint32_t shares, std::uint32_t price) noexcept {
        const std::uint16_t sym = symbols_->lookup(hdr.stock_locate.get());
        if (sym == SymbolTable::kNone) {
            return;
        }
        if (side_code != 'B' && side_code != 'S') {
            stats_.rejected += 1;
            return;
        }
        const Side side = side_code == 'B' ? Side::Buy : Side::Sell;
        top_update(sym, hdr,
                   books_->add(SymbolId{sym}, OrderId{ref}, side, Qty{shares},
                               Price{static_cast<std::int64_t>(price)}));
    }

    [[nodiscard]] bool accepted(const book::Update& up) noexcept {
        if (up.ok()) [[likely]] {
            return true;
        }
        if (up.result == book::Result::UnknownOrder) {
            stats_.unknown_orders += 1;
        } else {
            stats_.rejected += 1;
        }
        return false;
    }

    void top_update(std::uint16_t sym, const wire::itch::Header& hdr,
                    const book::Update& up) noexcept {
        if (accepted(up) && up.top_changed) {
            publish(Kind::TopOfBook, sym, hdr, 0, 0);
        }
    }

    void publish(Kind kind, std::uint16_t sym, const wire::itch::Header& hdr,
                 std::uint32_t last_ticks, std::uint32_t last_qty) noexcept {
        MarketUpdate* out = sink_->claim();
        if (out == nullptr) {
            stats_.dropped += 1;
            return;
        }
        const book::OrderBook* book = books_->book(SymbolId{sym});
        const book::Top top = book->top();
        std::uint8_t flags = feed_flags_;
        if (book->degraded()) {
            flags |= kFlagDegraded;
        }
        if (symbols_->halted(SymbolId{sym})) {
            flags |= kFlagHalted;
        }

        out->rx_tsc = rx_tsc_;
        out->exchange_ns = hdr.timestamp_ns.get();
        out->sequence = sequence_;
        out->bid_qty = top.bid.qty;
        out->ask_qty = top.ask.qty;
        out->bid_ticks = top.bid.ticks;
        out->ask_ticks = top.ask.ticks;
        out->last_ticks = last_ticks;
        out->last_qty = last_qty;
        out->symbol = SymbolId{sym};
        out->kind = kind;
        out->flags = flags;
        sink_->publish();
        stats_.published += 1;
    }

    book::BookSet* books_;
    SymbolTable* symbols_;
    Sink* sink_;
    std::uint64_t sequence_{0};
    std::uint64_t rx_tsc_{0};
    std::uint8_t feed_flags_{0};
    BuilderStats stats_{};
};

} // namespace hotpath::feed

#endif // HOTPATH_FEED_BOOK_BUILDER_HPP
