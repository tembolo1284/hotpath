#ifndef HOTPATH_STRATEGY_RUNNER_HPP
#define HOTPATH_STRATEGY_RUNNER_HPP

#include <concepts>
#include <cstdint>

#include "hotpath/core/clock.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/strategy/order_sender.hpp"

namespace hotpath::strategy {

template <typename S>
concept MarketSource = requires(S& source) {
    { source.peek() } -> std::same_as<const feed::MarketUpdate*>;
    source.consume();
};

template <typename S, typename Sender>
concept Strategy = requires(S& strategy, const feed::MarketUpdate& update,
                            const gateway::ExecutionReport& report, Sender& sender) {
    strategy.on_market(update, sender);
    strategy.on_report(report, sender);
};

template <typename S, MarketSource Market, gateway::ReportSource Reports,
          gateway::RequestSink Requests, typename Clock = TscClock>
    requires Strategy<S, OrderSender<Requests, Clock>>
class StrategyRunner {
public:
    using Sender = OrderSender<Requests, Clock>;

    static constexpr unsigned kMaxReportsPerPoll = 32;
    static constexpr unsigned kMaxUpdatesPerPoll = 32;

    StrategyRunner(S& strategy, Market& market, Reports& reports, Requests& requests,
                   std::uint64_t first_order_id = 1) noexcept
        : strategy_{&strategy},
          market_{&market},
          reports_{&reports},
          sender_{requests, first_order_id} {}

    std::uint32_t poll() noexcept {
        std::uint32_t handled = 0;

        sender_.set_origin(0);
        for (unsigned i = 0; i < kMaxReportsPerPoll; ++i) {
            const gateway::ExecutionReport* report = reports_->peek();
            if (report == nullptr) {
                break;
            }
            strategy_->on_report(*report, sender_);
            reports_->consume();
            handled += 1U;
        }

        for (unsigned i = 0; i < kMaxUpdatesPerPoll; ++i) {
            const feed::MarketUpdate* update = market_->peek();
            if (update == nullptr) {
                break;
            }
            sender_.set_origin(update->rx_tsc);
            strategy_->on_market(*update, sender_);
            market_->consume();
            handled += 1U;
        }
        return handled;
    }

    [[nodiscard]] const Sender& sender() const noexcept { return sender_; }

private:
    S* strategy_;
    Market* market_;
    Reports* reports_;
    Sender sender_;
};

} // namespace hotpath::strategy

#endif // HOTPATH_STRATEGY_RUNNER_HPP
