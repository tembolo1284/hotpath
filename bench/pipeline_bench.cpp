#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>

#include "hotpath/book/book_set.hpp"
#include "hotpath/core/clock.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/feed/feed_handler.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/feed/symbol_table.hpp"
#include "hotpath/gateway/order_gateway.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/net/socket.hpp"
#include "hotpath/perf/histogram.hpp"
#include "hotpath/strategy/imbalance_taker.hpp"
#include "hotpath/strategy/runner.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/ouch.hpp"
#include "hotpath/wire/soup.hpp"
#include "support/bench.hpp"
#include "support/flow.hpp"

using namespace hotpath;

namespace {

class EchoVenue {
public:
    net::IoResult send(std::span<const std::byte> data) noexcept {
        std::size_t offset = 0;
        while (data.size() - offset >= sizeof(wire::soup::Header)) {
            wire::soup::Header hdr{};
            std::memcpy(&hdr, data.data() + offset, sizeof(hdr));
            const std::size_t payload = hdr.length.get() - 1U;
            const std::byte* body = data.data() + offset + sizeof(hdr);
            if (hdr.type == wire::soup::kLoginRequest) {
                auto accepted =
                    wire::soup::make<wire::soup::LoginAccepted>(wire::soup::kLoginAccepted);
                accepted.session = wire::Alpha<10>::right_justified("BENCH");
                accepted.sequence = wire::format_numeric<20>(1);
                queue(&accepted, sizeof(accepted));
            } else if (hdr.type == wire::soup::kUnsequencedData &&
                       payload == sizeof(wire::ouch::EnterOrder)) {
                wire::ouch::EnterOrder order{};
                std::memcpy(&order, body, sizeof(order));
                respond(order);
            }
            offset += sizeof(hdr) + payload;
        }
        bytes_out_ += data.size();
        return net::IoResult{data.size(), Status{}, false};
    }

    net::IoResult recv(std::span<std::byte> buffer) noexcept {
        const std::size_t pending = tail_ - head_;
        if (pending == 0) {
            head_ = 0;
            tail_ = 0;
            return net::IoResult{0, Status{EAGAIN}, false};
        }
        const std::size_t n = buffer.size() < pending ? buffer.size() : pending;
        std::memcpy(buffer.data(), inbound_.data() + head_, n);
        head_ += n;
        return net::IoResult{n, Status{}, false};
    }

    [[nodiscard]] std::uint64_t orders() const noexcept { return orders_; }
    [[nodiscard]] std::uint64_t bytes_out() const noexcept { return bytes_out_; }

private:
    void respond(const wire::ouch::EnterOrder& order) noexcept {
        orders_ += 1;
        auto accepted = wire::ouch::make<wire::ouch::Accepted>();
        accepted.timestamp_ns.set(orders_);
        accepted.token = order.token;
        accepted.side = order.side;
        accepted.shares = order.shares;
        accepted.stock = order.stock;
        accepted.price = order.price;
        accepted.time_in_force = order.time_in_force;
        accepted.order_ref.set(orders_);
        accepted.order_state = 'L';
        sequenced(accepted);

        auto canceled = wire::ouch::make<wire::ouch::Canceled>();
        canceled.timestamp_ns.set(orders_);
        canceled.token = order.token;
        canceled.decrement_shares = order.shares;
        canceled.reason = 'I';
        sequenced(canceled);
    }

    template <wire::WireMessage M>
    void sequenced(const M& msg) noexcept {
        const wire::soup::Header hdr = wire::soup::make_header(wire::soup::kSequencedData, sizeof(M));
        queue(&hdr, sizeof(hdr));
        queue(&msg, sizeof(M));
    }

    void queue(const void* bytes, std::size_t len) noexcept {
        if (len > inbound_.size() - tail_) {
            bench::die("echo venue buffer overflow");
        }
        std::memcpy(inbound_.data() + tail_, bytes, len);
        tail_ += len;
    }

    std::array<std::byte, 16'384> inbound_{};
    std::size_t head_{0};
    std::size_t tail_{0};
    std::uint64_t orders_{0};
    std::uint64_t bytes_out_{0};
};

template <typename T>
class Ring {
public:
    explicit Ring(mem::Arena& arena, std::uint64_t capacity) {
        const std::size_t bytes = ipc::SpscRing<T>::bytes_for(capacity);
        void* mem = arena.allocate(bytes, kCacheLine);
        const auto formatted = ipc::SpscRing<T>::format(mem, bytes, capacity);
        bench::require(formatted.status, "ring format");
        ring_ = formatted.ring;
    }
    [[nodiscard]] ipc::SpscRing<T>& get() noexcept { return *ring_; }

private:
    ipc::SpscRing<T>* ring_{nullptr};
};

struct Rig {
    using MarketTx = ipc::SpscProducer<feed::MarketUpdate>;
    using MarketRx = ipc::SpscConsumer<feed::MarketUpdate>;
    using RequestTx = ipc::SpscProducer<gateway::OrderRequest>;
    using RequestRx = ipc::SpscConsumer<gateway::OrderRequest>;
    using ReportTx = ipc::SpscProducer<gateway::ExecutionReport>;
    using ReportRx = ipc::SpscConsumer<gateway::ExecutionReport>;
    using Feed = feed::FeedHandler<MarketTx>;
    using Runner = strategy::StrategyRunner<strategy::ImbalanceTaker, MarketRx, ReportRx, RequestTx>;
    using Gateway = gateway::OrderGateway<EchoVenue, RequestRx, ReportTx>;

    bench::Workspace workspace{std::size_t{64} << 20};
    Ring<feed::MarketUpdate> market{workspace.arena(), 1U << 12};
    Ring<gateway::OrderRequest> requests{workspace.arena(), 1U << 10};
    Ring<gateway::ExecutionReport> reports{workspace.arena(), 1U << 10};
    MarketTx market_tx{market.get()};
    MarketRx market_rx{market.get()};
    RequestTx request_tx{requests.get()};
    RequestRx request_rx{requests.get()};
    ReportTx report_tx{reports.get()};
    ReportRx report_rx{reports.get()};
    book::BookSet books{};
    feed::SymbolTable symbols{};
    strategy::ImbalanceTaker taker{};
    EchoVenue venue{};
    std::unique_ptr<Feed> feed_handler{};
    std::unique_ptr<Runner> runner{};
    std::unique_ptr<Gateway> gateway{};

    explicit Rig(std::uint64_t ticks_per_second) {
        mem::Arena& arena = workspace.arena();
        bench::require(books.init(arena, {.max_symbols = bench::kFlowSymbols,
                                          .max_orders = 1U << 14, .levels_per_side = 256}),
                       "book init");
        bench::require(symbols.init(arena, bench::kFlowSymbols), "symbol table init");
        for (const auto& ticker : bench::kFlowTickers) {
            bench::require(symbols.watch(ticker.view()), "watch");
        }
        bench::require(taker.init(arena, bench::kFlowSymbols,
                                  {.threshold_pct = 70, .clip_qty = 100, .max_position = 400,
                                   .max_open_orders = 256}),
                       "strategy init");

        gateway::GatewayConfig config{};
        config.soup.username = wire::Alpha<6>{"BENCH"};
        config.soup.heartbeat_ticks = ticks_per_second * 3600U;
        config.soup.timeout_ticks = ticks_per_second * 3600U;
        config.risk = {.max_order_qty = 1000, .max_order_notional = 10'000'000'000ULL,
                       .max_orders_per_window = 1'000'000'000, .window_ticks = ticks_per_second};

        feed_handler = std::make_unique<Feed>(books, symbols, market_tx);
        feed_handler->configure(1, false);
        runner = std::make_unique<Runner>(taker, market_rx, report_rx, request_tx);
        gateway = std::make_unique<Gateway>(venue, request_rx, report_tx, bench::kFlowTickers,
                                            config);
        gateway->connect();
        static_cast<void>(gateway->poll());
        static_cast<void>(runner->poll());
        if (!gateway->active() || !taker.session_up()) {
            bench::die("gateway did not log in");
        }
    }

    void replay(const bench::PacketLog& packets, perf::Histogram& per_packet,
                std::uint64_t overhead) noexcept {
        for (std::size_t i = 0; i < packets.size(); ++i) {
            const std::uint64_t t0 = tsc_begin();
            feed_handler->on_packet(packets[i], feed::Source::Live, t0);
            static_cast<void>(runner->poll());
            static_cast<void>(gateway->poll());
            const std::uint64_t t1 = tsc_end();
            per_packet.record(t1 - t0 > overhead ? t1 - t0 - overhead : 0);

            static_cast<void>(gateway->poll());
            static_cast<void>(runner->poll());
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    bench::Session session{argc, argv,
                           "pipeline_bench: tick to trade in one thread, no kernel"};
    session.note("feed handler -> ring -> strategy -> ring -> gateway -> in-memory venue");

    bench::Flow flow;
    bench::generate_flow(flow, session.ops(1'000'000), 0x9E37'79B9'7F4A'7C15ULL);
    const std::uint64_t overhead = perf::measure_clock_overhead();

    perf::Histogram discard;
    {
        Rig warm{session.ticks_per_second()};
        warm.replay(flow.packets, discard, overhead);
    }
    perf::Histogram per_packet;
    Rig rig{session.ticks_per_second()};
    rig.replay(flow.packets, per_packet, overhead);

    const gateway::GatewayLatency& latency = rig.gateway->latency();
    session.report("packet, all stages", per_packet);
    session.report("tick-to-trade", latency.tick_to_trade);
    session.report("  feed-to-decision", latency.feed_to_decision);
    session.report("  ring-hop", latency.decision_to_gateway);
    session.report("  gateway-send", latency.gateway_to_wire);

    const feed::FeedStats& feed_stats = rig.feed_handler->stats();
    const gateway::GatewayStats& gw = rig.gateway->stats();
    std::printf("  packets=%zu messages=%llu orders=%llu sent=%llu reports=%llu gaps=%llu "
                "risk_rejects=%llu reports_dropped=%llu\n",
                flow.packets.size(), static_cast<unsigned long long>(flow.messages),
                static_cast<unsigned long long>(rig.taker.stats().orders),
                static_cast<unsigned long long>(gw.sent),
                static_cast<unsigned long long>(gw.reports_published),
                static_cast<unsigned long long>(feed_stats.gaps),
                static_cast<unsigned long long>(gw.risk_rejects),
                static_cast<unsigned long long>(gw.reports_dropped));
    if (feed_stats.gaps != 0 || gw.sent != rig.venue.orders() || gw.sent == 0 ||
        gw.reports_dropped != 0 || rig.feed_handler->builder_stats().dropped != 0) {
        bench::die("pipeline replay was not clean");
    }
    return 0;
}
