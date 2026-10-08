#ifndef HOTPATH_STRATEGY_IMBALANCE_TAKER_HPP
#define HOTPATH_STRATEGY_IMBALANCE_TAKER_HPP

#include <cstdint>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/strategy/order_table.hpp"
#include "hotpath/wire/ouch.hpp"

namespace hotpath::strategy {

struct TakerConfig {
    std::uint32_t threshold_pct{80};
    std::uint32_t clip_qty{100};
    std::int64_t max_position{500};
    std::uint32_t max_open_orders{1024};
};

struct TakerStats {
    std::uint64_t updates{0};
    std::uint64_t signals{0};
    std::uint64_t orders{0};
    std::uint64_t refused{0};
    std::uint64_t fills{0};
    std::uint64_t rejects{0};
    std::uint64_t untracked_reports{0};
};

class ImbalanceTaker {
public:
    [[nodiscard]] Status init(mem::Arena& arena, std::uint16_t symbols,
                              const TakerConfig& config) noexcept {
        HOTPATH_ASSERT(busy_ == nullptr);
        if (config.threshold_pct <= 50 || config.threshold_pct > 100 || config.clip_qty == 0 ||
            config.max_position < static_cast<std::int64_t>(config.clip_qty)) {
            return Status{EINVAL};
        }
        const Status st = orders_.init(arena, config.max_open_orders, symbols);
        if (!st.ok()) {
            return st;
        }
        bool* busy = arena.allocate_array<bool>(symbols);
        if (busy == nullptr) {
            return Status{ENOMEM};
        }
        busy_ = busy;
        config_ = config;
        return Status{};
    }

    template <typename Sender>
    void on_market(const feed::MarketUpdate& update, Sender& sender) noexcept {
        stats_.updates += 1;
        if (update.kind != feed::Kind::TopOfBook && update.kind != feed::Kind::Trade) {
            return;
        }
        const std::uint16_t sym = update.symbol.raw();
        if (!session_up_ || !update.tradable() || sym >= orders_.symbols() || busy_[sym]) {
            return;
        }
        if (!update.has_bid() || !update.has_ask() || update.bid_ticks >= update.ask_ticks) {
            return;
        }

        const std::uint64_t total = update.bid_qty + update.ask_qty;
        const std::int64_t position = orders_.position(update.symbol);
        const auto clip = static_cast<std::int64_t>(config_.clip_qty);

        Side side = Side::Buy;
        std::uint32_t price = 0;
        if (update.bid_qty * 100U >= total * config_.threshold_pct &&
            position + clip <= config_.max_position) {
            side = Side::Buy;
            price = update.ask_ticks;
        } else if (update.ask_qty * 100U >= total * config_.threshold_pct &&
                   position - clip >= -config_.max_position) {
            side = Side::Sell;
            price = update.bid_ticks;
        } else {
            return;
        }
        stats_.signals += 1;

        if (!orders_.can_insert(sender.next_id())) {
            stats_.refused += 1;
            return;
        }
        const std::uint64_t id = sender.send_new(update.symbol, side, config_.clip_qty, price,
                                                 wire::ouch::kTifImmediateOrCancel);
        if (id == 0) {
            stats_.refused += 1;
            return;
        }
        static_cast<void>(orders_.insert(id, update.symbol, side, config_.clip_qty, price));
        busy_[sym] = true;
        stats_.orders += 1;
    }

    template <typename Sender>
    void on_report(const gateway::ExecutionReport& report, Sender&) noexcept {
        using gateway::ReportKind;
        if (report.kind == ReportKind::SessionUp) {
            session_up_ = true;
            return;
        }
        if (report.kind == ReportKind::SessionDown) {
            session_up_ = false;
            return;
        }
        if (report.kind == ReportKind::Executed) {
            stats_.fills += 1;
        } else if (report.kind == ReportKind::Rejected) {
            stats_.rejects += 1;
        }

        const Applied applied = orders_.apply(report);
        if (!applied.known) {
            stats_.untracked_reports += 1;
            return;
        }
        if (applied.done) {
            busy_[applied.symbol.raw()] = false;
        }
    }

    [[nodiscard]] std::int64_t position(SymbolId symbol) const noexcept {
        return orders_.position(symbol);
    }
    [[nodiscard]] std::int64_t cash_ticks(SymbolId symbol) const noexcept {
        return orders_.cash_ticks(symbol);
    }
    [[nodiscard]] bool session_up() const noexcept { return session_up_; }
    [[nodiscard]] std::uint32_t open_orders() const noexcept { return orders_.open_orders(); }
    [[nodiscard]] const TakerStats& stats() const noexcept { return stats_; }

private:
    OrderTable orders_{};
    bool* busy_{nullptr};
    TakerConfig config_{};
    TakerStats stats_{};
    bool session_up_{false};
};

} // namespace hotpath::strategy

#endif // HOTPATH_STRATEGY_IMBALANCE_TAKER_HPP
