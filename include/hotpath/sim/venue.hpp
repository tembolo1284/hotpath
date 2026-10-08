#ifndef HOTPATH_SIM_VENUE_HPP
#define HOTPATH_SIM_VENUE_HPP

#include <cstdint>
#include <span>

#include "hotpath/core/types.hpp"
#include "hotpath/sim/matching_engine.hpp"
#include "hotpath/sim/token_map.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/itch.hpp"
#include "hotpath/wire/ouch.hpp"

namespace hotpath::sim {

struct VenueStats {
    std::uint64_t orders_entered{0};
    std::uint64_t orders_rejected{0};
    std::uint64_t duplicate_tokens{0};
    std::uint64_t cancels{0};
    std::uint64_t replaces{0};
    std::uint64_t ignored{0};
    std::uint64_t trades{0};
    std::uint64_t client_fills{0};
    std::uint64_t house_orders{0};
};

template <typename Feed, typename Session>
class Venue {
public:
    static constexpr std::uint32_t kMaxOrderShares = 999'999;

    Venue(MatchingEngine& engine, TokenMap& tokens, Feed& feed, Session& session,
          std::span<const wire::Alpha<8>> tickers) noexcept
        : engine_{&engine}, tokens_{&tokens}, feed_{&feed}, session_{&session}, tickers_{tickers} {}

    void set_time(std::uint64_t ns) noexcept { now_ns_ = ns; }
    void set_tickers(std::span<const wire::Alpha<8>> tickers) noexcept { tickers_ = tickers; }

    [[nodiscard]] static constexpr std::uint16_t locate_of(std::uint16_t symbol) noexcept {
        return static_cast<std::uint16_t>(symbol + 1U);
    }

    void open_market() noexcept {
        for (const char code : {'O', 'S', 'Q'}) {
            auto event = itch<wire::itch::SystemEvent>(0);
            event.hdr.stock_locate.set(0);
            event.event_code = code;
            feed_->publish(event);
        }
        for (std::uint16_t symbol = 0; symbol < tickers_.size(); ++symbol) {
            auto directory = itch<wire::itch::StockDirectory>(symbol);
            directory.stock = tickers_[symbol];
            directory.market_category = 'Q';
            directory.financial_status = 'N';
            directory.round_lot_size.set(100);
            directory.round_lots_only = 'N';
            directory.issue_classification = 'C';
            directory.issue_sub_type = wire::Alpha<2>{"Z"};
            directory.authenticity = 'P';
            directory.short_sale_threshold = 'N';
            directory.ipo_flag = 'N';
            directory.luld_reference_price_tier = '1';
            directory.etp_flag = 'N';
            directory.inverse_indicator = 'N';
            feed_->publish(directory);

            auto action = itch<wire::itch::StockTradingAction>(symbol);
            action.stock = tickers_[symbol];
            action.trading_state = 'T';
            action.reserved = ' ';
            feed_->publish(action);
        }
        auto start = wire::ouch::make<wire::ouch::SystemEvent>();
        start.timestamp_ns.set(now_ns_);
        start.event_code = 'S';
        session_->send_sequenced(start);
    }

    void set_trading_state(std::uint16_t symbol, char state) noexcept {
        if (symbol >= tickers_.size()) {
            return;
        }
        auto action = itch<wire::itch::StockTradingAction>(symbol);
        action.stock = tickers_[symbol];
        action.trading_state = state;
        action.reserved = ' ';
        feed_->publish(action);
    }

    OrderHandle house_submit(std::uint16_t symbol, Side side, std::uint32_t qty,
                             std::uint32_t ticks, bool immediate_or_cancel) noexcept {
        stats_.house_orders += 1;
        return engine_->submit(*this, symbol, side, qty, ticks, immediate_or_cancel, 0).resting;
    }

    void house_cancel(OrderHandle handle) noexcept {
        static_cast<void>(engine_->reduce(*this, handle, 0));
    }

    void on(const wire::ouch::EnterOrder& m) noexcept {
        if (tokens_->find(m.token) != nullptr) {
            stats_.duplicate_tokens += 1;
            return;
        }
        TokenEntry* entry = tokens_->insert(m.token);
        if (entry == nullptr) {
            reject(m.token, 'T');
            return;
        }
        const std::uint16_t symbol = find_symbol(m.stock);
        const std::uint32_t shares = m.shares.get();
        const std::uint32_t price = m.price.get();
        if (symbol == kNoSymbol) {
            reject(m.token, 'S');
            return;
        }
        if (m.side != 'B' && m.side != 'S' && m.side != 'T' && m.side != 'E') {
            reject(m.token, 'Y');
            return;
        }
        if (shares == 0 || shares > kMaxOrderShares) {
            reject(m.token, 'Z');
            return;
        }
        if (price == 0 || price > wire::ouch::kMaxPrice) {
            reject(m.token, 'X');
            return;
        }
        entry->symbol = symbol;
        entry->side = m.side == 'B' ? Side::Buy : Side::Sell;
        stats_.orders_entered += 1;

        auto accepted = wire::ouch::make<wire::ouch::Accepted>();
        accepted.timestamp_ns.set(now_ns_);
        accepted.token = m.token;
        accepted.side = m.side;
        accepted.shares = m.shares;
        accepted.stock = m.stock;
        accepted.price = m.price;
        accepted.time_in_force = m.time_in_force;
        accepted.firm = m.firm;
        accepted.display = m.display;
        accepted.order_ref.set(engine_->next_ref());
        accepted.capacity = m.capacity;
        accepted.intermarket_sweep = m.intermarket_sweep;
        accepted.minimum_quantity = m.minimum_quantity;
        accepted.cross_type = m.cross_type;
        accepted.order_state = 'L';
        accepted.bbo_weight = ' ';
        session_->send_sequenced(accepted);

        const bool ioc = m.time_in_force.get() == wire::ouch::kTifImmediateOrCancel;
        work(*entry, shares, price, ioc);
    }

    void on(const wire::ouch::CancelOrder& m) noexcept {
        TokenEntry* entry = tokens_->find(m.token);
        if (entry == nullptr || !entry->live) {
            stats_.ignored += 1;
            return;
        }
        const std::uint32_t removed = engine_->reduce(*this, entry->handle, m.shares.get());
        if (removed == 0) {
            stats_.ignored += 1;
            return;
        }
        stats_.cancels += 1;
        if (engine_->find(entry->handle) == nullptr) {
            entry->live = false;
        }
        canceled(m.token, removed, 'U');
    }

    void on(const wire::ouch::ReplaceOrder& m) noexcept {
        TokenEntry* existing = tokens_->find(m.existing_token);
        if (existing == nullptr || !existing->live ||
            tokens_->find(m.replacement_token) != nullptr) {
            stats_.ignored += 1;
            return;
        }
        const std::uint16_t symbol = existing->symbol;
        const Side side = existing->side;
        TokenEntry* entry = tokens_->insert(m.replacement_token);
        const std::uint32_t shares = m.shares.get();
        const std::uint32_t price = m.price.get();
        if (entry == nullptr || shares == 0 || shares > kMaxOrderShares || price == 0 ||
            price > wire::ouch::kMaxPrice) {
            reject(m.replacement_token, 'X');
            return;
        }
        static_cast<void>(engine_->reduce(*this, existing->handle, 0));
        existing->live = false;
        entry->symbol = symbol;
        entry->side = side;
        stats_.replaces += 1;

        auto replaced = wire::ouch::make<wire::ouch::Replaced>();
        replaced.timestamp_ns.set(now_ns_);
        replaced.replacement_token = m.replacement_token;
        replaced.side = side == Side::Buy ? 'B' : 'S';
        replaced.shares = m.shares;
        replaced.stock = tickers_[symbol];
        replaced.price = m.price;
        replaced.time_in_force = m.time_in_force;
        replaced.display = m.display;
        replaced.order_ref.set(engine_->next_ref());
        replaced.capacity = 'P';
        replaced.intermarket_sweep = m.intermarket_sweep;
        replaced.minimum_quantity = m.minimum_quantity;
        replaced.cross_type = 'N';
        replaced.order_state = 'L';
        replaced.previous_token = m.existing_token;
        replaced.bbo_weight = ' ';
        session_->send_sequenced(replaced);

        const bool ioc = m.time_in_force.get() == wire::ouch::kTifImmediateOrCancel;
        work(*entry, shares, price, ioc);
    }

    void on_trade(const RestingOrder& resting, std::uint64_t, std::uint64_t aggressor_user, Side,
                  std::uint32_t qty, std::uint32_t ticks, std::uint64_t match, bool done) noexcept {
        stats_.trades += 1;
        auto executed = itch<wire::itch::OrderExecuted>(resting.symbol);
        executed.order_ref.set(resting.ref);
        executed.executed_shares.set(qty);
        executed.match_number.set(match);
        feed_->publish(executed);

        if (TokenEntry* maker = tokens_->by_user(resting.user); maker != nullptr) {
            fill(maker->token, qty, ticks, 'A', match);
            if (done) {
                maker->live = false;
            }
        }
        if (TokenEntry* taker = tokens_->by_user(aggressor_user); taker != nullptr) {
            fill(taker->token, qty, ticks, 'R', match);
        }
    }

    void on_rest(const RestingOrder& order) noexcept {
        auto add = itch<wire::itch::AddOrder>(order.symbol);
        add.order_ref.set(order.ref);
        add.side = order.side == Side::Buy ? 'B' : 'S';
        add.shares.set(order.qty);
        add.stock = tickers_[order.symbol];
        add.price.set(order.ticks);
        feed_->publish(add);
    }

    void on_reduce(const RestingOrder& order, std::uint32_t removed, bool gone) noexcept {
        if (gone) {
            auto del = itch<wire::itch::OrderDelete>(order.symbol);
            del.order_ref.set(order.ref);
            feed_->publish(del);
            return;
        }
        auto cancel = itch<wire::itch::OrderCancel>(order.symbol);
        cancel.order_ref.set(order.ref);
        cancel.cancelled_shares.set(removed);
        feed_->publish(cancel);
    }

    [[nodiscard]] const VenueStats& stats() const noexcept { return stats_; }

private:
    static constexpr std::uint16_t kNoSymbol = 0xFFFF;

    template <wire::WireMessage M>
    [[nodiscard]] M itch(std::uint16_t symbol) const noexcept {
        M msg = wire::itch::make<M>();
        msg.hdr.stock_locate.set(locate_of(symbol));
        msg.hdr.timestamp_ns.set(now_ns_);
        return msg;
    }

    [[nodiscard]] std::uint16_t find_symbol(const wire::Alpha<8>& stock) const noexcept {
        for (std::uint16_t i = 0; i < tickers_.size(); ++i) {
            if (tickers_[i] == stock) {
                return i;
            }
        }
        return kNoSymbol;
    }

    void work(TokenEntry& entry, std::uint32_t shares, std::uint32_t price, bool ioc) noexcept {
        const wire::ouch::Token token = entry.token;
        const std::uint64_t user = tokens_->user_of(entry);
        const Submitted result =
            engine_->submit(*this, entry.symbol, entry.side, shares, price, ioc, user);
        if (result.resting.valid()) {
            entry.handle = result.resting;
            entry.live = true;
        }
        if (result.canceled > 0) {
            canceled(token, result.canceled, ioc ? 'I' : 'Z');
        }
    }

    void fill(const wire::ouch::Token& token, std::uint32_t qty, std::uint32_t ticks,
              char liquidity, std::uint64_t match) noexcept {
        auto executed = wire::ouch::make<wire::ouch::Executed>();
        executed.timestamp_ns.set(now_ns_);
        executed.token = token;
        executed.executed_shares.set(qty);
        executed.execution_price.set(ticks);
        executed.liquidity_flag = liquidity;
        executed.match_number.set(match);
        session_->send_sequenced(executed);
        stats_.client_fills += 1;
    }

    void canceled(const wire::ouch::Token& token, std::uint32_t shares, char reason) noexcept {
        auto msg = wire::ouch::make<wire::ouch::Canceled>();
        msg.timestamp_ns.set(now_ns_);
        msg.token = token;
        msg.decrement_shares.set(shares);
        msg.reason = reason;
        session_->send_sequenced(msg);
    }

    void reject(const wire::ouch::Token& token, char reason) noexcept {
        stats_.orders_rejected += 1;
        auto msg = wire::ouch::make<wire::ouch::Rejected>();
        msg.timestamp_ns.set(now_ns_);
        msg.token = token;
        msg.reason = reason;
        session_->send_sequenced(msg);
    }

    MatchingEngine* engine_;
    TokenMap* tokens_;
    Feed* feed_;
    Session* session_;
    std::span<const wire::Alpha<8>> tickers_;
    std::uint64_t now_ns_{0};
    VenueStats stats_{};
};

} // namespace hotpath::sim

#endif // HOTPATH_SIM_VENUE_HPP
