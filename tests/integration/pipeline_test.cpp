#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hotpath/book/book_set.hpp"
#include "hotpath/feed/feed_handler.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/feed/symbol_table.hpp"
#include "hotpath/gateway/order_gateway.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/gateway/token.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"
#include "hotpath/strategy/imbalance_taker.hpp"
#include "hotpath/strategy/runner.hpp"
#include "hotpath/wire/itch.hpp"
#include "hotpath/wire/mold.hpp"
#include "hotpath/wire/ouch.hpp"
#include "hotpath/wire/soup.hpp"
#include "support/fakes.hpp"

using namespace hotpath;
using test::FakeClock;
using test::FakeTransport;

namespace {

constexpr std::uint16_t kLocate = 42;

wire::itch::AddOrder add_order(std::uint64_t ref, char side, std::uint32_t shares,
                               std::uint32_t price) {
    auto m = wire::itch::make<wire::itch::AddOrder>();
    m.hdr.stock_locate.set(kLocate);
    m.order_ref.set(ref);
    m.side = side;
    m.shares.set(shares);
    m.price.set(price);
    return m;
}

TEST(Pipeline, TickInOrderOutThroughAllThreeStages) {
    mem::Region region;
    ASSERT_TRUE(region.map(std::size_t{16} << 20, sys::PageKind::Small).ok());
    region.prefault();
    mem::Arena arena{region};

    test::HeapRing<feed::MarketUpdate> market_ring{256};
    test::HeapRing<gateway::OrderRequest> request_ring{256};
    test::HeapRing<gateway::ExecutionReport> report_ring{256};
    ipc::SpscProducer<feed::MarketUpdate> market_tx{market_ring.ring()};
    ipc::SpscConsumer<feed::MarketUpdate> market_rx{market_ring.ring()};
    ipc::SpscProducer<gateway::OrderRequest> request_tx{request_ring.ring()};
    ipc::SpscConsumer<gateway::OrderRequest> request_rx{request_ring.ring()};
    ipc::SpscProducer<gateway::ExecutionReport> report_tx{report_ring.ring()};
    ipc::SpscConsumer<gateway::ExecutionReport> report_rx{report_ring.ring()};

    book::BookSet books;
    ASSERT_TRUE(books.init(arena, {.max_symbols = 1, .max_orders = 1024,
                                   .levels_per_side = 32}).ok());
    feed::SymbolTable symbols;
    ASSERT_TRUE(symbols.init(arena, 1).ok());
    ASSERT_TRUE(symbols.watch("MSFT").ok());
    feed::FeedHandler<ipc::SpscProducer<feed::MarketUpdate>> feed_handler{books, symbols,
                                                                         market_tx};

    strategy::ImbalanceTaker taker;
    ASSERT_TRUE(taker.init(arena, 1, {.threshold_pct = 80, .clip_qty = 100, .max_position = 300,
                                      .max_open_orders = 64}).ok());
    strategy::StrategyRunner<strategy::ImbalanceTaker, ipc::SpscConsumer<feed::MarketUpdate>,
                             ipc::SpscConsumer<gateway::ExecutionReport>,
                             ipc::SpscProducer<gateway::OrderRequest>, FakeClock>
        runner{taker, market_rx, report_rx, request_tx};

    FakeTransport venue;
    const std::array<wire::Alpha<8>, 1> tickers{symbols.ticker(SymbolId{0})};
    gateway::GatewayConfig config{};
    config.risk = {.max_order_qty = 1000, .max_order_notional = 10'000'000'000ULL,
                   .max_orders_per_window = 100, .window_ticks = 1'000'000};
    gateway::OrderGateway<FakeTransport, ipc::SpscConsumer<gateway::OrderRequest>,
                          ipc::SpscProducer<gateway::ExecutionReport>, FakeClock>
        gw{venue, request_rx, report_tx, tickers, config};

    FakeClock::value = 100;
    gw.connect();
    venue.accept_login("SIM", 1);
    static_cast<void>(gw.poll());
    static_cast<void>(runner.poll());
    ASSERT_TRUE(taker.session_up());
    venue.sent.clear();

    std::array<std::byte, wire::mold::kMaxPacketBytes> buffer{};
    wire::mold::PacketWriter writer{buffer};
    writer.begin(wire::Alpha<10>{"SIM"}, 1);
    auto dir = wire::itch::make<wire::itch::StockDirectory>();
    dir.hdr.stock_locate.set(kLocate);
    dir.stock = wire::Alpha<8>{"MSFT"};
    ASSERT_TRUE(writer.append(dir));
    ASSERT_TRUE(writer.append(add_order(1, 'S', 100, 4'251'000)));
    ASSERT_TRUE(writer.append(add_order(2, 'B', 900, 4'250'000)));

    FakeClock::value = 1000;
    feed_handler.on_packet(writer.finish(), feed::Source::Live, 1000);
    FakeClock::value = 1040;
    EXPECT_EQ(runner.poll(), 2U);
    FakeClock::value = 1100;
    EXPECT_EQ(gw.poll(), 1U);

    ASSERT_EQ(venue.sent.size(), sizeof(wire::soup::Header) + sizeof(wire::ouch::EnterOrder));
    const auto enter = venue.sent_at<wire::ouch::EnterOrder>(sizeof(wire::soup::Header));
    EXPECT_EQ(enter.side, 'B');
    EXPECT_EQ(enter.shares.get(), 100U);
    EXPECT_EQ(enter.stock.view(), "MSFT");
    EXPECT_EQ(enter.price.get(), 4'251'000U);
    EXPECT_EQ(enter.time_in_force.get(), wire::ouch::kTifImmediateOrCancel);
    EXPECT_EQ(gw.stats().tick_to_trade.count, 1U);
    EXPECT_EQ(gw.stats().tick_to_trade.max, 100U);
    EXPECT_EQ(gw.latency().tick_to_trade.count(), 1U);
    EXPECT_EQ(gw.latency().tick_to_trade.max(), 100U);
    EXPECT_EQ(gw.latency().feed_to_decision.max(), 40U);
    EXPECT_EQ(gw.latency().decision_to_wire.max(), 60U);
    EXPECT_EQ(gw.latency().decision_to_gateway.max(), 60U);
    EXPECT_EQ(gw.latency().gateway_to_wire.count(), 1U);

    auto fill = wire::ouch::make<wire::ouch::Executed>();
    fill.token = enter.token;
    fill.executed_shares.set(100);
    fill.execution_price.set(4'251'000);
    venue.feed_packet(wire::soup::kSequencedData, fill);
    static_cast<void>(gw.poll());
    static_cast<void>(runner.poll());
    EXPECT_EQ(taker.position(SymbolId{0}), 100);
    EXPECT_EQ(taker.open_orders(), 0U);
    EXPECT_EQ(taker.stats().fills, 1U);
}

} // namespace
