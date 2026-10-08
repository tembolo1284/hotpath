#ifndef HOTPATH_GATEWAY_ORDER_GATEWAY_HPP
#define HOTPATH_GATEWAY_ORDER_GATEWAY_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "hotpath/core/clock.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/gateway/risk.hpp"
#include "hotpath/gateway/soup_session.hpp"
#include "hotpath/gateway/token.hpp"
#include "hotpath/perf/histogram.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/ouch.hpp"

namespace hotpath::gateway {

struct GatewayConfig {
    SoupConfig soup{};
    RiskLimits risk{};
    wire::Alpha<4> firm{};
    char token_prefix{'H'};
    char display{'Y'};
    char capacity{'P'};
};

struct LatencySummary {
    std::uint64_t count{0};
    std::uint64_t sum{0};
    std::uint64_t min{~std::uint64_t{0}};
    std::uint64_t max{0};

    constexpr void record(std::uint64_t ticks) noexcept {
        count += 1;
        sum += ticks;
        min = ticks < min ? ticks : min;
        max = ticks > max ? ticks : max;
    }
};

struct GatewayStats {
    std::uint64_t requests{0};
    std::uint64_t sent{0};
    std::uint64_t risk_rejects{0};
    std::uint64_t not_connected{0};
    std::uint64_t backpressure{0};
    std::uint64_t bad_requests{0};
    std::uint64_t reports_published{0};
    std::uint64_t reports_dropped{0};
    std::uint64_t foreign_tokens{0};
    std::uint64_t unhandled{0};
    LatencySummary tick_to_trade{};
};

struct GatewayLatency {
    perf::Histogram tick_to_trade{};
    perf::Histogram feed_to_decision{};
    perf::Histogram decision_to_wire{};
};

template <Transport T, RequestSource Requests, ReportSink Reports, typename Clock = TscClock>
class OrderGateway {
public:
    static constexpr unsigned kMaxRequestsPerPoll = 16;

    OrderGateway(T& transport, Requests& requests, Reports& reports,
                 std::span<const wire::Alpha<8>> tickers, const GatewayConfig& config) noexcept
        : session_{transport},
          requests_{&requests},
          reports_{&reports},
          tickers_{tickers},
          config_{config},
          risk_{config.risk} {}

    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;

    void connect() noexcept { session_.login(config_.soup, Clock::now()); }
    void disconnect() noexcept { session_.logout(Clock::now()); }

    std::uint32_t poll() noexcept {
        now_ = Clock::now();
        session_.poll(now_, *this);

        std::uint32_t handled = 0;
        for (unsigned i = 0; i < kMaxRequestsPerPoll; ++i) {
            const OrderRequest* request = requests_->peek();
            if (request == nullptr) {
                break;
            }
            handle(*request);
            requests_->consume();
            handled += 1U;
        }
        return handled;
    }

    void on_session(SessionState state) noexcept {
        if (state == SessionState::Idle || state == SessionState::LoggingIn) {
            return;
        }
        ExecutionReport* out = begin_report(
            state == SessionState::Active ? ReportKind::SessionUp : ReportKind::SessionDown, 0, 0);
        if (out != nullptr) {
            out->reason = session_.reject_reason();
            reports_->publish();
        }
    }

    void on_sequenced(std::span<const std::byte> payload) noexcept {
        if (wire::ouch::dispatch_outbound(payload, *this) != wire::Parse::Ok) {
            stats_.unhandled += 1;
        }
    }

    void on(const wire::ouch::Accepted& m) noexcept {
        ExecutionReport* out = begin_report(ReportKind::Accepted, m.token, m.timestamp_ns.get());
        if (out != nullptr) {
            out->order_ref = m.order_ref.get();
            out->qty = m.shares.get();
            out->price_ticks = m.price.get();
            out->reason = m.order_state;
            reports_->publish();
        }
    }

    void on(const wire::ouch::Replaced& m) noexcept {
        const TokenId previous = parse_token(m.previous_token, config_.token_prefix);
        ExecutionReport* out =
            begin_report(ReportKind::Replaced, m.replacement_token, m.timestamp_ns.get());
        if (out != nullptr) {
            out->related_id = previous.ok ? previous.id : 0;
            out->order_ref = m.order_ref.get();
            out->qty = m.shares.get();
            out->price_ticks = m.price.get();
            out->reason = m.order_state;
            reports_->publish();
        }
    }

    void on(const wire::ouch::Executed& m) noexcept {
        executed(m.token, m.timestamp_ns.get(), m.executed_shares.get(), m.execution_price.get(),
                 m.liquidity_flag, m.match_number.get());
    }

    void on(const wire::ouch::ExecutedWithReferencePrice& m) noexcept {
        executed(m.token, m.timestamp_ns.get(), m.executed_shares.get(), m.execution_price.get(),
                 m.liquidity_flag, m.match_number.get());
    }

    void on(const wire::ouch::Canceled& m) noexcept {
        canceled(m.token, m.timestamp_ns.get(), m.decrement_shares.get(), m.reason);
    }

    void on(const wire::ouch::AiqCanceled& m) noexcept {
        canceled(m.token, m.timestamp_ns.get(), m.decrement_shares.get(), m.reason);
    }

    void on(const wire::ouch::Rejected& m) noexcept {
        ExecutionReport* out = begin_report(ReportKind::Rejected, m.token, m.timestamp_ns.get());
        if (out != nullptr) {
            out->source = RejectSource::Venue;
            out->reason = m.reason;
            reports_->publish();
        }
    }

    void on(const wire::ouch::CancelPending& m) noexcept {
        simple(ReportKind::CancelPending, m.token, m.timestamp_ns.get());
    }

    void on(const wire::ouch::CancelReject& m) noexcept {
        simple(ReportKind::CancelRejected, m.token, m.timestamp_ns.get());
    }

    void on(const wire::ouch::BrokenTrade& m) noexcept {
        ExecutionReport* out = begin_report(ReportKind::TradeBroken, m.token, m.timestamp_ns.get());
        if (out != nullptr) {
            out->match_number = m.match_number.get();
            out->reason = m.reason;
            reports_->publish();
        }
    }

    [[nodiscard]] SessionState state() const noexcept { return session_.state(); }
    [[nodiscard]] bool active() const noexcept { return session_.active(); }
    [[nodiscard]] const GatewayStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const GatewayLatency& latency() const noexcept { return latency_; }
    [[nodiscard]] const SoupStats& session_stats() const noexcept { return session_.stats(); }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return session_.next_sequence(); }

private:
    void handle(const OrderRequest& request) noexcept {
        stats_.requests += 1;
        if (!session_.active()) {
            stats_.not_connected += 1;
            reject(request, RejectSource::NotConnected, 0);
            return;
        }

        switch (request.kind) {
            case RequestKind::New:
                enter(request);
                return;
            case RequestKind::Cancel: {
                auto msg = wire::ouch::make<wire::ouch::CancelOrder>();
                msg.token = make_token(config_.token_prefix, request.client_id);
                msg.shares.set(request.qty);
                transmit(request, msg);
                return;
            }
            case RequestKind::Replace:
                replace(request);
                return;
        }
        stats_.bad_requests += 1;
        reject(request, RejectSource::BadRequest, 0);
    }

    [[nodiscard]] bool admit(const OrderRequest& request) noexcept {
        if (request.price_ticks > wire::ouch::kMaxPrice) {
            stats_.bad_requests += 1;
            reject(request, RejectSource::BadRequest, 'P');
            return false;
        }
        const RiskVerdict verdict = risk_.check(request.qty, request.price_ticks, now_);
        if (verdict != RiskVerdict::Pass) {
            stats_.risk_rejects += 1;
            reject(request, RejectSource::Risk, static_cast<char>(verdict));
            return false;
        }
        return true;
    }

    void enter(const OrderRequest& request) noexcept {
        if (request.symbol.raw() >= tickers_.size()) {
            stats_.bad_requests += 1;
            reject(request, RejectSource::BadRequest, 'S');
            return;
        }
        if (!admit(request)) {
            return;
        }
        auto msg = wire::ouch::make<wire::ouch::EnterOrder>();
        msg.token = make_token(config_.token_prefix, request.client_id);
        msg.side = request.side == Side::Buy ? 'B' : 'S';
        msg.shares.set(request.qty);
        msg.stock = tickers_[request.symbol.raw()];
        msg.price.set(request.price_ticks);
        msg.time_in_force.set(request.time_in_force);
        msg.firm = config_.firm;
        msg.display = config_.display;
        msg.capacity = config_.capacity;
        msg.intermarket_sweep = 'N';
        msg.minimum_quantity.set(0);
        msg.cross_type = 'N';
        msg.customer_type = ' ';
        transmit(request, msg);
    }

    void replace(const OrderRequest& request) noexcept {
        if (!admit(request)) {
            return;
        }
        auto msg = wire::ouch::make<wire::ouch::ReplaceOrder>();
        msg.existing_token = make_token(config_.token_prefix, request.target_id);
        msg.replacement_token = make_token(config_.token_prefix, request.client_id);
        msg.shares.set(request.qty);
        msg.price.set(request.price_ticks);
        msg.time_in_force.set(request.time_in_force);
        msg.display = config_.display;
        msg.intermarket_sweep = 'N';
        msg.minimum_quantity.set(0);
        transmit(request, msg);
    }

    template <wire::WireMessage M>
    void transmit(const OrderRequest& request, const M& msg) noexcept {
        if (!session_.send_unsequenced(msg, now_)) {
            stats_.backpressure += 1;
            reject(request, RejectSource::Backpressure, 0);
            return;
        }
        stats_.sent += 1;
        if (request.origin_tsc != 0) {
            const std::uint64_t sent = Clock::now();
            const std::uint64_t total = sent >= request.origin_tsc ? sent - request.origin_tsc : 0;
            stats_.tick_to_trade.record(total);
            latency_.tick_to_trade.record(total);
            if (request.decision_tsc >= request.origin_tsc && sent >= request.decision_tsc) {
                latency_.feed_to_decision.record(request.decision_tsc - request.origin_tsc);
                latency_.decision_to_wire.record(sent - request.decision_tsc);
            }
        }
    }

    void reject(const OrderRequest& request, RejectSource source, char reason) noexcept {
        const ReportKind kind = request.kind == RequestKind::Cancel ? ReportKind::CancelRejected
                                                                    : ReportKind::Rejected;
        ExecutionReport* out = begin_report(kind, request.client_id, 0);
        if (out != nullptr) {
            out->related_id = request.target_id;
            out->source = source;
            out->reason = reason;
            reports_->publish();
        }
    }

    void executed(const wire::ouch::Token& token, std::uint64_t venue_ns, std::uint32_t shares,
                  std::uint32_t price, char liquidity, std::uint64_t match) noexcept {
        ExecutionReport* out = begin_report(ReportKind::Executed, token, venue_ns);
        if (out != nullptr) {
            out->qty = shares;
            out->price_ticks = price;
            out->liquidity = liquidity;
            out->match_number = match;
            reports_->publish();
        }
    }

    void canceled(const wire::ouch::Token& token, std::uint64_t venue_ns, std::uint32_t shares,
                  char reason) noexcept {
        ExecutionReport* out = begin_report(ReportKind::Canceled, token, venue_ns);
        if (out != nullptr) {
            out->qty = shares;
            out->reason = reason;
            reports_->publish();
        }
    }

    void simple(ReportKind kind, const wire::ouch::Token& token, std::uint64_t venue_ns) noexcept {
        if (begin_report(kind, token, venue_ns) != nullptr) {
            reports_->publish();
        }
    }

    [[nodiscard]] ExecutionReport* begin_report(ReportKind kind, const wire::ouch::Token& token,
                                                std::uint64_t venue_ns) noexcept {
        const TokenId parsed = parse_token(token, config_.token_prefix);
        if (!parsed.ok) {
            stats_.foreign_tokens += 1;
            return nullptr;
        }
        return begin_report(kind, parsed.id, venue_ns);
    }

    [[nodiscard]] ExecutionReport* begin_report(ReportKind kind, std::uint64_t client_id,
                                                std::uint64_t venue_ns) noexcept {
        ExecutionReport* out = reports_->claim();
        if (out == nullptr) {
            stats_.reports_dropped += 1;
            return nullptr;
        }
        *out = ExecutionReport{};
        out->client_id = client_id;
        out->venue_ns = venue_ns;
        out->rx_tsc = now_;
        out->kind = kind;
        stats_.reports_published += 1;
        return out;
    }

    SoupSession<T> session_;
    Requests* requests_;
    Reports* reports_;
    std::span<const wire::Alpha<8>> tickers_;
    GatewayConfig config_;
    RiskGate risk_;
    GatewayStats stats_{};
    GatewayLatency latency_{};
    std::uint64_t now_{0};
};

} // namespace hotpath::gateway

#endif // HOTPATH_GATEWAY_ORDER_GATEWAY_HPP
