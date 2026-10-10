#include <gtest/gtest.h>

#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "hotpath/book/book_set.hpp"
#include "hotpath/core/clock.hpp"
#include "hotpath/feed/feed_handler.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/feed/symbol_table.hpp"
#include "hotpath/gateway/order_gateway.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/ipc/spin.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"
#include "hotpath/net/tcp.hpp"
#include "hotpath/perf/report.hpp"
#include "hotpath/sim/exchange.hpp"
#include "hotpath/strategy/imbalance_taker.hpp"
#include "hotpath/strategy/runner.hpp"
#include "support/fakes.hpp"

using namespace hotpath;

namespace {

TEST(SimLoop, StrategyTradesAgainstTheSimulatedVenueOverSockets) {
    constexpr std::uint16_t kSymbols = 2;
    mem::Region region;
    ASSERT_TRUE(region.map(std::size_t{64} << 20, sys::PageKind::Small).ok());
    region.prefault();
    mem::Arena arena{region};
    const std::array<wire::Alpha<8>, kSymbols> tickers{wire::Alpha<8>{"MSFT"},
                                                       wire::Alpha<8>{"NVDA"}};

    test::HeapRing<feed::MarketUpdate> market_ring{1U << 14};
    test::HeapRing<gateway::OrderRequest> request_ring{1U << 10};
    test::HeapRing<gateway::ExecutionReport> report_ring{1U << 12};
    ipc::SpscProducer<feed::MarketUpdate> market_tx{market_ring.ring()};
    ipc::SpscConsumer<feed::MarketUpdate> market_rx{market_ring.ring()};
    ipc::SpscProducer<gateway::OrderRequest> request_tx{request_ring.ring()};
    ipc::SpscConsumer<gateway::OrderRequest> request_rx{request_ring.ring()};
    ipc::SpscProducer<gateway::ExecutionReport> report_tx{report_ring.ring()};
    ipc::SpscConsumer<gateway::ExecutionReport> report_rx{report_ring.ring()};

    book::BookSet books;
    ASSERT_TRUE(books.init(arena, {.max_symbols = kSymbols, .max_orders = 1U << 12,
                                   .levels_per_side = 128}).ok());
    feed::SymbolTable symbols;
    ASSERT_TRUE(symbols.init(arena, kSymbols).ok());
    for (const auto& ticker : tickers) {
        ASSERT_TRUE(symbols.watch(ticker.view()).ok());
    }

    auto exchange_owner = std::make_unique<sim::Exchange>();
    sim::Exchange& exchange = *exchange_owner;
    auto feed_owner = std::make_unique<feed::FeedHandler<ipc::SpscProducer<feed::MarketUpdate>>>(
        books, symbols, market_tx);
    auto& feed_handler = *feed_owner;

    const net::Endpoint loopback_any = net::make_endpoint("127.0.0.1", 0).endpoint;
    net::UdpServer probe;
    ASSERT_TRUE(probe.open(loopback_any).ok());
    const net::Endpoint retransmit_endpoint = probe.local_endpoint().endpoint;
    probe = net::UdpServer{};

    feed::FeedConfig feed_config{};
    feed_config.line_a.listen = loopback_any;
    feed_config.line_a.rcvbuf_bytes = 1 << 21;
    feed_config.recovery = retransmit_endpoint;
    feed_config.has_recovery = true;
    feed_config.request_interval_ticks = 2'000'000;
    ASSERT_TRUE(feed_handler.open(feed_config).ok());

    sim::ExchangeConfig config{};
    config.order_entry = loopback_any;
    config.feed_destination = feed_handler.line_a_endpoint().endpoint;
    config.retransmit = retransmit_endpoint;
    config.engine = {.symbols = kSymbols, .max_orders = 1U << 12, .levels_per_side = 128};
    config.max_tokens = 1U << 14;
    config.soup.log_capacity = 1U << 14;
    config.publisher.store_capacity = 1U << 18;
    config.liquidity.max_live = 24;
    config.liquidity_steps_per_poll = 1;
    ASSERT_TRUE(exchange.init(arena, config, tickers, 1).ok());

    strategy::ImbalanceTaker taker;
    ASSERT_TRUE(taker.init(arena, kSymbols, {.threshold_pct = 70, .clip_qty = 100,
                                             .max_position = 400, .max_open_orders = 256}).ok());
    strategy::StrategyRunner<strategy::ImbalanceTaker, ipc::SpscConsumer<feed::MarketUpdate>,
                             ipc::SpscConsumer<gateway::ExecutionReport>,
                             ipc::SpscProducer<gateway::OrderRequest>>
        runner{taker, market_rx, report_rx, request_tx};

    net::TcpStream venue_link;
    ASSERT_TRUE(venue_link.connect(exchange.order_entry_endpoint().endpoint, 1000).ok());
    gateway::GatewayConfig gw_config{};
    gw_config.soup.username = wire::Alpha<6>{"paul"};
    gw_config.risk = {.max_order_qty = 1000, .max_order_notional = 10'000'000'000ULL,
                      .max_orders_per_window = 1'000'000, .window_ticks = 1'000'000'000};
    auto gw_owner = std::make_unique<
        gateway::OrderGateway<net::TcpStream, ipc::SpscConsumer<gateway::OrderRequest>,
                              ipc::SpscProducer<gateway::ExecutionReport>>>(
        venue_link, request_rx, report_tx, tickers, gw_config);
    auto& gw = *gw_owner;
    gw.connect();

    std::uint64_t now = 1;
    const auto turn = [&] {
        now += 1000;
        exchange.poll(now);
        for (int i = 0; i < 4; ++i) {
            static_cast<void>(feed_handler.poll());
        }
        static_cast<void>(runner.poll());
        static_cast<void>(gw.poll());
    };

    ASSERT_TRUE(ipc::spin_until(
        [&] {
            turn();
            return taker.stats().fills >= 25;
        },
        400'000))
        << "fills=" << taker.stats().fills << " orders=" << taker.stats().orders
        << " signals=" << taker.stats().signals << " session=" << taker.session_up();

    exchange.set_liquidity_steps(0);
    ASSERT_TRUE(ipc::spin_until(
        [&] {
            turn();
            static_cast<void>(::usleep(50));
            return feed_handler.expected_sequence() == exchange.publisher().next_sequence() &&
                   !feed_handler.recovering() && taker.open_orders() == 0 &&
                   exchange.session().client_cursor() == exchange.session().next_sequence() &&
                   report_rx.peek() == nullptr && market_rx.peek() == nullptr &&
                   request_rx.peek() == nullptr;
        },
        200'000));
    for (int i = 0; i < 50; ++i) {
        turn();
    }

    EXPECT_EQ(feed_handler.stats().lost_messages, 0U);
    EXPECT_EQ(feed_handler.builder_stats().unknown_orders, 0U);
    EXPECT_EQ(feed_handler.builder_stats().rejected, 0U);
    EXPECT_EQ(feed_handler.builder_stats().dropped, 0U);
    EXPECT_EQ(feed_handler.stats().malformed, 0U);
    EXPECT_EQ(gw.stats().reports_dropped, 0U);
    EXPECT_EQ(gw.stats().risk_rejects, 0U);
    EXPECT_EQ(gw.stats().sent, taker.stats().orders);
    EXPECT_GE(gw.stats().tick_to_trade.count, 25U);
    EXPECT_EQ(exchange.venue().stats().orders_entered, taker.stats().orders);
    EXPECT_EQ(exchange.venue().stats().orders_rejected, 0U);
    EXPECT_EQ(taker.stats().untracked_reports, 0U);

    for (std::uint16_t s = 0; s < kSymbols; ++s) {
        const book::Top top = books.book(SymbolId{s})->top();
        const sim::PriceLevel bid = exchange.engine().best(s, Side::Buy);
        const sim::PriceLevel ask = exchange.engine().best(s, Side::Sell);
        EXPECT_EQ(top.bid.ticks, bid.ticks) << "symbol " << s;
        EXPECT_EQ(top.bid.qty, bid.qty) << "symbol " << s;
        EXPECT_EQ(top.bid.orders, bid.orders) << "symbol " << s;
        EXPECT_EQ(top.ask.ticks, ask.ticks) << "symbol " << s;
        EXPECT_EQ(top.ask.qty, ask.qty) << "symbol " << s;
        EXPECT_EQ(books.book(SymbolId{s})->bids().depth(), exchange.engine().depth(s, Side::Buy));
        EXPECT_EQ(books.book(SymbolId{s})->asks().depth(), exchange.engine().depth(s, Side::Sell));
        EXPECT_LE(taker.position(SymbolId{s}), 400);
        EXPECT_GE(taker.position(SymbolId{s}), -400);
    }
    EXPECT_EQ(books.open_orders(), exchange.engine().resting_orders());

    std::printf("sim loop: itch msgs=%llu orders=%llu fills=%llu gaps=%llu requests=%llu\n",
                static_cast<unsigned long long>(exchange.publisher().next_sequence() - 1),
                static_cast<unsigned long long>(taker.stats().orders),
                static_cast<unsigned long long>(taker.stats().fills),
                static_cast<unsigned long long>(feed_handler.stats().gaps),
                static_cast<unsigned long long>(feed_handler.stats().requests_sent));
    EXPECT_EQ(gw.latency().tick_to_trade.count(), gw.stats().tick_to_trade.count);
    EXPECT_EQ(gw.latency().feed_to_decision.count(), gw.latency().decision_to_wire.count());
    EXPECT_EQ(gw.latency().decision_to_gateway.count(), gw.latency().gateway_to_wire.count());
    if (has_invariant_tsc()) {
        const TscScale scale = calibrate_tsc(10'000'000ULL);
        perf::print(stdout, "tick-to-trade", perf::summarize(gw.latency().tick_to_trade, scale));
        perf::print(stdout, "feed-to-decision",
                    perf::summarize(gw.latency().feed_to_decision, scale));
        perf::print(stdout, "decision-to-wire",
                    perf::summarize(gw.latency().decision_to_wire, scale));
    }
}

} // namespace
