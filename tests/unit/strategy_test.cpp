#include <gtest/gtest.h>

#include <cstdint>

#include "hotpath/feed/market_update.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"
#include "hotpath/strategy/imbalance_taker.hpp"
#include "hotpath/strategy/order_sender.hpp"
#include "hotpath/strategy/order_table.hpp"
#include "hotpath/strategy/runner.hpp"
#include "hotpath/wire/ouch.hpp"
#include "support/fakes.hpp"

using namespace hotpath;
using gateway::ReportKind;
using test::FakeClock;

namespace {

using RequestSink = test::VectorSink<gateway::OrderRequest>;
using Sender = strategy::OrderSender<RequestSink, FakeClock>;

static_assert(strategy::Strategy<strategy::ImbalanceTaker, Sender>);
static_assert(strategy::MarketSource<ipc::SpscConsumer<feed::MarketUpdate>>);
static_assert(gateway::RequestSink<ipc::SpscProducer<gateway::OrderRequest>>);
static_assert(gateway::RequestSource<ipc::SpscConsumer<gateway::OrderRequest>>);
static_assert(gateway::ReportSink<ipc::SpscProducer<gateway::ExecutionReport>>);
static_assert(gateway::ReportSource<ipc::SpscConsumer<gateway::ExecutionReport>>);

feed::MarketUpdate top(std::uint16_t symbol, std::uint32_t bid, std::uint64_t bid_qty,
                       std::uint32_t ask, std::uint64_t ask_qty) {
    feed::MarketUpdate u{};
    u.kind = feed::Kind::TopOfBook;
    u.symbol = SymbolId{symbol};
    u.bid_ticks = bid;
    u.bid_qty = bid_qty;
    u.ask_ticks = ask;
    u.ask_qty = ask_qty;
    u.rx_tsc = 5000;
    return u;
}

gateway::ExecutionReport report(ReportKind kind, std::uint64_t id, std::uint32_t qty = 0,
                                std::uint32_t price = 0) {
    gateway::ExecutionReport r{};
    r.kind = kind;
    r.client_id = id;
    r.qty = qty;
    r.price_ticks = price;
    return r;
}

class StrategyTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(region_.map(std::size_t{4} << 20, sys::PageKind::Small).ok());
        region_.prefault();
        arena_ = mem::Arena{region_};
        FakeClock::value = 6000;
    }

    mem::Region region_;
    mem::Arena arena_;
};

TEST_F(StrategyTest, OrderTableTracksFillsAndPosition) {
    strategy::OrderTable table;
    EXPECT_EQ(table.init(arena_, 6, 2).err(), EINVAL);
    ASSERT_TRUE(table.init(arena_, 8, 2).ok());

    ASSERT_NE(table.insert(1, SymbolId{0}, Side::Buy, 100, 4'250'000), nullptr);
    EXPECT_FALSE(table.can_insert(9));
    EXPECT_EQ(table.insert(9, SymbolId{0}, Side::Buy, 1, 1), nullptr);
    EXPECT_EQ(table.insert(2, SymbolId{2}, Side::Buy, 1, 1), nullptr);
    EXPECT_EQ(table.find(9), nullptr);

    strategy::Applied a = table.apply(report(ReportKind::Accepted, 1, 100, 4'250'000));
    EXPECT_TRUE(a.known);
    EXPECT_FALSE(a.done);
    a = table.apply(report(ReportKind::Executed, 1, 60, 4'250'000));
    EXPECT_FALSE(a.done);
    EXPECT_EQ(table.position(SymbolId{0}), 60);
    EXPECT_EQ(table.cash_ticks(SymbolId{0}), -60LL * 4'250'000);
    a = table.apply(report(ReportKind::Canceled, 1, 40));
    EXPECT_TRUE(a.done);
    EXPECT_EQ(table.open_orders(), 0U);
    EXPECT_FALSE(table.apply(report(ReportKind::Executed, 1, 10, 1)).known);

    ASSERT_NE(table.insert(9, SymbolId{0}, Side::Sell, 60, 4'251'000), nullptr);
    a = table.apply(report(ReportKind::Executed, 9, 60, 4'251'000));
    EXPECT_TRUE(a.done);
    EXPECT_EQ(table.position(SymbolId{0}), 0);
    EXPECT_EQ(table.cash_ticks(SymbolId{0}), 60LL * 1000);

    ASSERT_NE(table.insert(3, SymbolId{1}, Side::Buy, 50, 100), nullptr);
    EXPECT_TRUE(table.apply(report(ReportKind::Rejected, 3)).done);
    ASSERT_NE(table.insert(4, SymbolId{1}, Side::Buy, 50, 100), nullptr);
    EXPECT_FALSE(table.apply(report(ReportKind::CancelRejected, 4)).done);

    gateway::ExecutionReport moved = report(ReportKind::Replaced, 5, 30, 110);
    moved.related_id = 4;
    a = table.apply(moved);
    EXPECT_TRUE(a.known);
    EXPECT_EQ(table.find(4), nullptr);
    ASSERT_NE(table.find(5), nullptr);
    EXPECT_EQ(table.find(5)->leaves(), 30U);
    EXPECT_EQ(table.find(5)->price_ticks, 110U);
}

TEST_F(StrategyTest, SenderStampsAndNumbersRequests) {
    RequestSink sink;
    Sender sender{sink, 100};
    sender.set_origin(5000);

    EXPECT_EQ(sender.send_new(SymbolId{1}, Side::Sell, 10, 4'250'000, 0), 100U);
    EXPECT_TRUE(sender.cancel(100, 5));
    EXPECT_EQ(sender.replace(100, 20, 4'251'000, 99'998), 101U);
    ASSERT_EQ(sink.items.size(), 3U);

    EXPECT_EQ(sink.items[0].kind, gateway::RequestKind::New);
    EXPECT_EQ(sink.items[0].origin_tsc, 5000U);
    EXPECT_EQ(sink.items[0].decision_tsc, 6000U);
    EXPECT_EQ(sink.items[0].side, Side::Sell);
    EXPECT_EQ(sink.items[1].kind, gateway::RequestKind::Cancel);
    EXPECT_EQ(sink.items[1].client_id, 100U);
    EXPECT_EQ(sink.items[1].qty, 5U);
    EXPECT_EQ(sink.items[2].client_id, 101U);
    EXPECT_EQ(sink.items[2].target_id, 100U);

    sink.full = true;
    EXPECT_EQ(sender.send_new(SymbolId{1}, Side::Buy, 10, 1, 0), 0U);
    EXPECT_FALSE(sender.cancel(100));
    EXPECT_EQ(sender.next_id(), 102U);
    EXPECT_EQ(sender.refused(), 2U);
}

TEST_F(StrategyTest, TakerTradesOnImbalanceOneOrderAtATime) {
    strategy::ImbalanceTaker taker;
    EXPECT_EQ(taker.init(arena_, 2, {.threshold_pct = 50}).err(), EINVAL);
    ASSERT_TRUE(taker.init(arena_, 2, {.threshold_pct = 80, .clip_qty = 100, .max_position = 200,
                                       .max_open_orders = 64}).ok());
    RequestSink sink;
    Sender sender{sink};

    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    EXPECT_TRUE(sink.items.empty());

    taker.on_report(report(ReportKind::SessionUp, 0), sender);
    taker.on_market(top(0, 4'250'000, 500, 4'251'000, 500), sender);
    EXPECT_TRUE(sink.items.empty());

    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    ASSERT_EQ(sink.items.size(), 1U);
    EXPECT_EQ(sink.items[0].side, Side::Buy);
    EXPECT_EQ(sink.items[0].price_ticks, 4'251'000U);
    EXPECT_EQ(sink.items[0].qty, 100U);
    EXPECT_EQ(sink.items[0].time_in_force, wire::ouch::kTifImmediateOrCancel);

    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    EXPECT_EQ(sink.items.size(), 1U);
    taker.on_market(top(1, 1'000'000, 10, 1'001'000, 990), sender);
    ASSERT_EQ(sink.items.size(), 2U);
    EXPECT_EQ(sink.items[1].side, Side::Sell);
    EXPECT_EQ(sink.items[1].price_ticks, 1'000'000U);

    taker.on_report(report(ReportKind::Accepted, 1, 100, 4'251'000), sender);
    taker.on_report(report(ReportKind::Executed, 1, 60, 4'251'000), sender);
    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    EXPECT_EQ(sink.items.size(), 2U);
    taker.on_report(report(ReportKind::Canceled, 1, 40), sender);
    EXPECT_EQ(taker.position(SymbolId{0}), 60);

    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    ASSERT_EQ(sink.items.size(), 3U);
    taker.on_report(report(ReportKind::Executed, 3, 100, 4'251'000), sender);
    EXPECT_EQ(taker.position(SymbolId{0}), 160);

    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    EXPECT_EQ(sink.items.size(), 3U);
    EXPECT_EQ(taker.stats().orders, 3U);
    EXPECT_EQ(taker.stats().fills, 2U);
}

TEST_F(StrategyTest, TakerIgnoresUntradableUpdates) {
    strategy::ImbalanceTaker taker;
    ASSERT_TRUE(taker.init(arena_, 1, {}).ok());
    RequestSink sink;
    Sender sender{sink};
    taker.on_report(report(ReportKind::SessionUp, 0), sender);

    feed::MarketUpdate stale = top(0, 4'250'000, 900, 4'251'000, 100);
    stale.flags = feed::kFlagStale;
    taker.on_market(stale, sender);
    feed::MarketUpdate halted = stale;
    halted.flags = feed::kFlagHalted;
    taker.on_market(halted, sender);
    taker.on_market(top(0, 4'250'000, 900, 0, 0), sender);
    taker.on_market(top(0, 4'251'000, 900, 4'251'000, 100), sender);
    taker.on_market(top(5, 4'250'000, 900, 4'251'000, 100), sender);
    feed::MarketUpdate status = top(0, 4'250'000, 900, 4'251'000, 100);
    status.kind = feed::Kind::FeedStatus;
    taker.on_market(status, sender);
    EXPECT_TRUE(sink.items.empty());

    taker.on_report(report(ReportKind::SessionDown, 0), sender);
    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    EXPECT_TRUE(sink.items.empty());

    taker.on_report(report(ReportKind::SessionUp, 0), sender);
    sink.full = true;
    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    EXPECT_EQ(taker.stats().refused, 1U);
    sink.full = false;
    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    ASSERT_EQ(sink.items.size(), 1U);

    gateway::ExecutionReport rejected = report(ReportKind::Rejected, 1);
    rejected.source = gateway::RejectSource::Risk;
    taker.on_report(rejected, sender);
    EXPECT_EQ(taker.open_orders(), 0U);
    taker.on_market(top(0, 4'250'000, 900, 4'251'000, 100), sender);
    EXPECT_EQ(sink.items.size(), 2U);
}

TEST_F(StrategyTest, RunnerDrainsReportsBeforeMarketData) {
    test::HeapRing<feed::MarketUpdate> market_ring{64};
    test::HeapRing<gateway::ExecutionReport> report_ring{64};
    test::HeapRing<gateway::OrderRequest> request_ring{64};
    ipc::SpscProducer<feed::MarketUpdate> market_tx{market_ring.ring()};
    ipc::SpscConsumer<feed::MarketUpdate> market_rx{market_ring.ring()};
    ipc::SpscProducer<gateway::ExecutionReport> report_tx{report_ring.ring()};
    ipc::SpscConsumer<gateway::ExecutionReport> report_rx{report_ring.ring()};
    ipc::SpscProducer<gateway::OrderRequest> request_tx{request_ring.ring()};
    ipc::SpscConsumer<gateway::OrderRequest> request_rx{request_ring.ring()};

    strategy::ImbalanceTaker taker;
    ASSERT_TRUE(taker.init(arena_, 2, {}).ok());
    strategy::StrategyRunner<strategy::ImbalanceTaker, ipc::SpscConsumer<feed::MarketUpdate>,
                             ipc::SpscConsumer<gateway::ExecutionReport>,
                             ipc::SpscProducer<gateway::OrderRequest>, FakeClock>
        runner{taker, market_rx, report_rx, request_tx, 500};

    ASSERT_TRUE(market_tx.try_push(top(0, 4'250'000, 900, 4'251'000, 100)));
    ASSERT_TRUE(report_tx.try_push(report(ReportKind::SessionUp, 0)));
    EXPECT_EQ(runner.poll(), 2U);

    const gateway::OrderRequest* request = request_rx.peek();
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->client_id, 500U);
    EXPECT_EQ(request->origin_tsc, 5000U);
    EXPECT_EQ(request->decision_tsc, 6000U);
    EXPECT_EQ(request->symbol, SymbolId{0});
    request_rx.consume();
    EXPECT_EQ(request_rx.peek(), nullptr);
    EXPECT_EQ(runner.poll(), 0U);
    EXPECT_EQ(runner.sender().sent(), 1U);
}

} // namespace
