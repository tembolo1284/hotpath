#include <gtest/gtest.h>

#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "hotpath/gateway/order_gateway.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/gateway/risk.hpp"
#include "hotpath/gateway/soup_session.hpp"
#include "hotpath/gateway/token.hpp"
#include "hotpath/ipc/spin.hpp"
#include "hotpath/net/tcp.hpp"
#include "hotpath/wire/ouch.hpp"
#include "hotpath/wire/soup.hpp"
#include "support/fakes.hpp"

using namespace hotpath;
using gateway::ReportKind;
using gateway::SessionState;
using test::FakeClock;
using test::FakeTransport;

namespace {

static_assert(gateway::Transport<FakeTransport>);
static_assert(gateway::Transport<net::TcpStream>);

constexpr std::size_t kSoupHeader = sizeof(wire::soup::Header);

struct Collector {
    std::vector<std::vector<std::byte>> messages;
    std::vector<SessionState> states;

    void on_sequenced(std::span<const std::byte> payload) {
        messages.emplace_back(payload.begin(), payload.end());
    }
    void on_session(SessionState state) { states.push_back(state); }
};

gateway::SoupConfig soup_config() {
    gateway::SoupConfig config{};
    config.username = wire::Alpha<6>{"paul"};
    config.password = wire::Alpha<10>{"secret"};
    config.heartbeat_ticks = 1000;
    config.timeout_ticks = 15'000;
    return config;
}

TEST(Token, RoundTripsAcrossTheIdRange) {
    for (const std::uint64_t id : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{31},
                                   std::uint64_t{32}, std::uint64_t{1} << 40,
                                   ~std::uint64_t{0}}) {
        const wire::ouch::Token token = gateway::make_token('H', id);
        const gateway::TokenId parsed = gateway::parse_token(token, 'H');
        EXPECT_TRUE(parsed.ok);
        EXPECT_EQ(parsed.id, id);
        EXPECT_EQ(token.raw().size(), 14U);
    }
    EXPECT_FALSE(gateway::parse_token(wire::ouch::Token{"HZZZZZZZZZZZZZ"}, 'H').ok);
    EXPECT_FALSE(gateway::parse_token(wire::ouch::Token{"H"}, 'H').ok);
}

TEST(Risk, FailsClosedAndLimitsRate) {
    gateway::RiskGate closed;
    EXPECT_EQ(closed.check(1, 1, 0), gateway::RiskVerdict::Quantity);

    gateway::RiskGate gate{{.max_order_qty = 500, .max_order_notional = 1'000'000'000,
                            .max_orders_per_window = 2, .window_ticks = 100}};
    EXPECT_EQ(gate.check(0, 10, 0), gateway::RiskVerdict::Quantity);
    EXPECT_EQ(gate.check(501, 10, 0), gateway::RiskVerdict::Quantity);
    EXPECT_EQ(gate.check(500, 2'000'001, 0), gateway::RiskVerdict::Notional);
    EXPECT_EQ(gate.check(100, 4'250'000, 10), gateway::RiskVerdict::Pass);
    EXPECT_EQ(gate.check(100, 4'250'000, 20), gateway::RiskVerdict::Pass);
    EXPECT_EQ(gate.check(100, 4'250'000, 30), gateway::RiskVerdict::Rate);
    EXPECT_EQ(gate.check(100, 4'250'000, 110), gateway::RiskVerdict::Pass);
}

TEST(SoupSession, LogsInAndDeliversSequencedData) {
    FakeTransport wire_;
    gateway::SoupSession<FakeTransport> session{wire_};
    Collector collector;

    session.login(soup_config(), 100);
    EXPECT_EQ(session.state(), SessionState::LoggingIn);
    ASSERT_EQ(wire_.sent.size(), sizeof(wire::soup::LoginRequest));
    const auto login = wire_.sent_at<wire::soup::LoginRequest>(0);
    EXPECT_EQ(login.hdr.type, wire::soup::kLoginRequest);
    EXPECT_EQ(login.hdr.length.get(), 47U);
    EXPECT_EQ(login.username.view(), "paul");
    EXPECT_EQ(login.password.view(), "secret");
    EXPECT_EQ(login.requested_session.trimmed(), "");
    EXPECT_EQ(wire::parse_numeric(login.requested_sequence.raw()).value, 1U);

    wire_.accept_login("SESS01", 5);
    session.poll(110, collector);
    EXPECT_TRUE(session.active());
    EXPECT_EQ(session.next_sequence(), 5U);
    EXPECT_EQ(session.session_id().trimmed(), "SESS01");
    ASSERT_EQ(collector.states.size(), 1U);
    EXPECT_EQ(collector.states[0], SessionState::Active);

    auto exec = wire::ouch::make<wire::ouch::Executed>();
    exec.executed_shares.set(25);
    wire_.feed_packet(wire::soup::kSequencedData, exec);
    wire_.feed_empty(wire::soup::kServerHeartbeat);
    wire_.feed_packet(wire::soup::kSequencedData, exec);
    wire_.recv_limit = 7;
    for (std::uint64_t now = 120; now < 400 && collector.messages.size() < 2; ++now) {
        session.poll(now, collector);
    }
    ASSERT_EQ(collector.messages.size(), 2U);
    EXPECT_EQ(collector.messages[0].size(), sizeof(exec));
    EXPECT_EQ(session.next_sequence(), 7U);
    EXPECT_EQ(session.stats().heartbeats_in, 1U);
    EXPECT_EQ(session.stats().sequenced_in, 2U);
}

TEST(SoupSession, HeartbeatsAndTimesOut) {
    FakeTransport wire_;
    gateway::SoupSession<FakeTransport> session{wire_};
    Collector collector;
    session.login(soup_config(), 0);
    wire_.accept_login("S", 1);
    session.poll(10, collector);
    ASSERT_TRUE(session.active());
    wire_.sent.clear();

    session.poll(900, collector);
    EXPECT_TRUE(wire_.sent.empty());
    session.poll(1010, collector);
    ASSERT_EQ(wire_.sent.size(), kSoupHeader);
    EXPECT_EQ(wire_.sent_at<wire::soup::Header>(0).type, wire::soup::kClientHeartbeat);
    EXPECT_EQ(session.stats().heartbeats_out, 1U);

    session.poll(1500, collector);
    EXPECT_EQ(wire_.sent.size(), kSoupHeader);

    wire_.feed_empty(wire::soup::kServerHeartbeat);
    session.poll(14'000, collector);
    EXPECT_TRUE(session.active());
    session.poll(28'999, collector);
    EXPECT_TRUE(session.active());
    session.poll(29'000, collector);
    EXPECT_EQ(session.state(), SessionState::Failed);
    EXPECT_EQ(collector.states.back(), SessionState::Failed);
}

TEST(SoupSession, HandlesRejectEndOfSessionAndPeerClose) {
    {
        FakeTransport wire_;
        gateway::SoupSession<FakeTransport> session{wire_};
        Collector collector;
        session.login(soup_config(), 0);
        auto rejected = wire::soup::make<wire::soup::LoginRejected>(wire::soup::kLoginRejected);
        rejected.reason = wire::soup::kRejectNotAuthorized;
        wire_.feed(std::as_bytes(std::span<const wire::soup::LoginRejected, 1>{&rejected, 1}));
        session.poll(1, collector);
        EXPECT_EQ(session.state(), SessionState::Rejected);
        EXPECT_EQ(session.reject_reason(), 'A');
        auto order = wire::ouch::make<wire::ouch::CancelOrder>();
        EXPECT_FALSE(session.send_unsequenced(order, 2));
    }
    {
        FakeTransport wire_;
        gateway::SoupSession<FakeTransport> session{wire_};
        Collector collector;
        session.login(soup_config(), 0);
        wire_.accept_login("S", 1);
        wire_.feed_empty(wire::soup::kEndOfSession);
        session.poll(1, collector);
        EXPECT_EQ(session.state(), SessionState::Ended);
        ASSERT_EQ(collector.states.size(), 1U);
    }
    {
        FakeTransport wire_;
        gateway::SoupSession<FakeTransport> session{wire_};
        Collector collector;
        session.login(soup_config(), 0);
        wire_.accept_login("S", 1);
        session.poll(1, collector);
        wire_.closed = true;
        session.poll(2, collector);
        EXPECT_EQ(session.state(), SessionState::Failed);
    }
    {
        FakeTransport wire_;
        gateway::SoupSession<FakeTransport> session{wire_};
        Collector collector;
        session.login(soup_config(), 0);
        const std::byte zero_length[3] = {};
        wire_.feed(zero_length);
        session.poll(1, collector);
        EXPECT_EQ(session.state(), SessionState::Failed);
        EXPECT_EQ(session.stats().malformed, 1U);
    }
}

TEST(SoupSession, PartialWritesAreBufferedInOrder) {
    FakeTransport wire_;
    gateway::SoupSession<FakeTransport> session{wire_};
    Collector collector;
    session.login(soup_config(), 0);
    wire_.accept_login("S", 1);
    session.poll(1, collector);
    wire_.sent.clear();

    auto first = wire::ouch::make<wire::ouch::CancelOrder>();
    first.token = gateway::make_token('H', 1);
    auto second = wire::ouch::make<wire::ouch::CancelOrder>();
    second.token = gateway::make_token('H', 2);

    wire_.send_limit = 5;
    EXPECT_TRUE(session.send_unsequenced(first, 2));
    EXPECT_EQ(wire_.sent.size(), 5U);
    EXPECT_EQ(session.tx_pending(), kSoupHeader + sizeof(first) - 5);
    wire_.send_blocked = true;
    EXPECT_TRUE(session.send_unsequenced(second, 3));
    session.poll(4, collector);
    EXPECT_EQ(wire_.sent.size(), 5U);

    wire_.send_blocked = false;
    wire_.send_limit = ~std::size_t{0};
    session.poll(5, collector);
    EXPECT_EQ(session.tx_pending(), 0U);
    ASSERT_EQ(wire_.sent.size(), 2 * (kSoupHeader + sizeof(first)));
    EXPECT_EQ(wire_.sent_at<wire::ouch::CancelOrder>(kSoupHeader).token, first.token);
    EXPECT_EQ(wire_.sent_at<wire::ouch::CancelOrder>(2 * kSoupHeader + sizeof(first)).token,
              second.token);

    wire_.send_error = EPIPE;
    EXPECT_FALSE(session.send_unsequenced(first, 6));
    EXPECT_EQ(session.state(), SessionState::Failed);
}

class GatewayTest : public ::testing::Test {
protected:
    using Gateway = gateway::OrderGateway<FakeTransport, test::QueueSource<gateway::OrderRequest>,
                                          test::VectorSink<gateway::ExecutionReport>, FakeClock>;

    void SetUp() override {
        FakeClock::value = 1000;
        config_.soup = soup_config();
        config_.risk = {.max_order_qty = 1000, .max_order_notional = 10'000'000'000ULL,
                        .max_orders_per_window = 100, .window_ticks = 1'000'000};
        config_.firm = wire::Alpha<4>{"HOTP"};
    }

    void bring_up(Gateway& gw) {
        gw.connect();
        wire_.accept_login("SESS01", 1);
        static_cast<void>(gw.poll());
        ASSERT_TRUE(gw.active());
        ASSERT_EQ(reports_.items.size(), 1U);
        ASSERT_EQ(reports_.items[0].kind, ReportKind::SessionUp);
        reports_.items.clear();
        wire_.sent.clear();
    }

    static gateway::OrderRequest new_order(std::uint64_t id, Side side, std::uint32_t qty,
                                           std::uint32_t price) {
        gateway::OrderRequest r{};
        r.kind = gateway::RequestKind::New;
        r.client_id = id;
        r.symbol = SymbolId{1};
        r.side = side;
        r.qty = qty;
        r.price_ticks = price;
        r.time_in_force = wire::ouch::kTifImmediateOrCancel;
        return r;
    }

    FakeTransport wire_;
    test::QueueSource<gateway::OrderRequest> requests_;
    test::VectorSink<gateway::ExecutionReport> reports_;
    std::array<wire::Alpha<8>, 2> tickers_{wire::Alpha<8>{"MSFT"}, wire::Alpha<8>{"NVDA"}};
    gateway::GatewayConfig config_{};
};

TEST_F(GatewayTest, TranslatesRequestsToOuch) {
    Gateway gw{wire_, requests_, reports_, tickers_, config_};
    bring_up(gw);

    gateway::OrderRequest order = new_order(7, Side::Sell, 300, 4'251'000);
    order.origin_tsc = 400;
    requests_.items.push_back(order);

    gateway::OrderRequest cancel{};
    cancel.kind = gateway::RequestKind::Cancel;
    cancel.client_id = 7;
    cancel.qty = 100;
    requests_.items.push_back(cancel);

    gateway::OrderRequest replace{};
    replace.kind = gateway::RequestKind::Replace;
    replace.client_id = 8;
    replace.target_id = 7;
    replace.qty = 200;
    replace.price_ticks = 4'250'500;
    replace.time_in_force = wire::ouch::kTifMarketHours;
    requests_.items.push_back(replace);

    FakeClock::value = 1500;
    EXPECT_EQ(gw.poll(), 3U);
    EXPECT_TRUE(reports_.items.empty());
    EXPECT_EQ(gw.stats().sent, 3U);

    std::size_t offset = 0;
    const auto h1 = wire_.sent_at<wire::soup::Header>(offset);
    EXPECT_EQ(h1.type, wire::soup::kUnsequencedData);
    EXPECT_EQ(h1.length.get(), sizeof(wire::ouch::EnterOrder) + 1U);
    const auto enter = wire_.sent_at<wire::ouch::EnterOrder>(offset + kSoupHeader);
    EXPECT_EQ(enter.type, 'O');
    EXPECT_EQ(enter.token, gateway::make_token('H', 7));
    EXPECT_EQ(enter.side, 'S');
    EXPECT_EQ(enter.shares.get(), 300U);
    EXPECT_EQ(enter.stock.view(), "NVDA");
    EXPECT_EQ(enter.price.get(), 4'251'000U);
    EXPECT_EQ(enter.time_in_force.get(), 0U);
    EXPECT_EQ(enter.firm.view(), "HOTP");
    EXPECT_EQ(enter.display, 'Y');
    EXPECT_EQ(enter.capacity, 'P');
    EXPECT_EQ(enter.intermarket_sweep, 'N');
    EXPECT_EQ(enter.cross_type, 'N');
    offset += kSoupHeader + sizeof(enter);

    const auto cxl = wire_.sent_at<wire::ouch::CancelOrder>(offset + kSoupHeader);
    EXPECT_EQ(cxl.type, 'X');
    EXPECT_EQ(cxl.token, gateway::make_token('H', 7));
    EXPECT_EQ(cxl.shares.get(), 100U);
    offset += kSoupHeader + sizeof(cxl);

    const auto rep = wire_.sent_at<wire::ouch::ReplaceOrder>(offset + kSoupHeader);
    EXPECT_EQ(rep.type, 'U');
    EXPECT_EQ(rep.existing_token, gateway::make_token('H', 7));
    EXPECT_EQ(rep.replacement_token, gateway::make_token('H', 8));
    EXPECT_EQ(rep.shares.get(), 200U);
    EXPECT_EQ(rep.price.get(), 4'250'500U);
    EXPECT_EQ(rep.time_in_force.get(), wire::ouch::kTifMarketHours);
    EXPECT_EQ(wire_.sent.size(), offset + kSoupHeader + sizeof(rep));

    EXPECT_EQ(gw.stats().tick_to_trade.count, 1U);
    EXPECT_EQ(gw.stats().tick_to_trade.min, 1100U);
    EXPECT_EQ(gw.stats().tick_to_trade.max, 1100U);
    EXPECT_EQ(gw.latency().tick_to_trade.count(), 1U);
    EXPECT_EQ(gw.latency().feed_to_decision.count(), 0U);
}

TEST_F(GatewayTest, TurnsVenueMessagesIntoReports) {
    Gateway gw{wire_, requests_, reports_, tickers_, config_};
    bring_up(gw);

    auto accepted = wire::ouch::make<wire::ouch::Accepted>();
    accepted.token = gateway::make_token('H', 7);
    accepted.timestamp_ns.set(34'200'000'000'001ULL);
    accepted.shares.set(300);
    accepted.price.set(4'251'000);
    accepted.order_ref.set(555);
    accepted.order_state = 'L';
    wire_.feed_packet(wire::soup::kSequencedData, accepted);

    auto exec = wire::ouch::make<wire::ouch::Executed>();
    exec.token = gateway::make_token('H', 7);
    exec.executed_shares.set(120);
    exec.execution_price.set(4'251'000);
    exec.liquidity_flag = 'R';
    exec.match_number.set(9001);
    wire_.feed_packet(wire::soup::kSequencedData, exec);

    auto canceled = wire::ouch::make<wire::ouch::Canceled>();
    canceled.token = gateway::make_token('H', 7);
    canceled.decrement_shares.set(180);
    canceled.reason = 'I';
    wire_.feed_packet(wire::soup::kSequencedData, canceled);

    auto rejected = wire::ouch::make<wire::ouch::Rejected>();
    rejected.token = gateway::make_token('H', 9);
    rejected.reason = 'X';
    wire_.feed_packet(wire::soup::kSequencedData, rejected);

    auto replaced = wire::ouch::make<wire::ouch::Replaced>();
    replaced.replacement_token = gateway::make_token('H', 11);
    replaced.previous_token = gateway::make_token('H', 10);
    replaced.shares.set(40);
    replaced.price.set(4'250'000);
    wire_.feed_packet(wire::soup::kSequencedData, replaced);

    auto foreign = wire::ouch::make<wire::ouch::Executed>();
    foreign.token = wire::ouch::Token{"OTHERSESSION01"};
    wire_.feed_packet(wire::soup::kSequencedData, foreign);

    auto broken = wire::ouch::make<wire::ouch::BrokenTrade>();
    broken.token = gateway::make_token('H', 7);
    broken.match_number.set(9001);
    broken.reason = 'E';
    wire_.feed_packet(wire::soup::kSequencedData, broken);

    FakeClock::value = 2000;
    static_cast<void>(gw.poll());
    ASSERT_EQ(reports_.items.size(), 6U);

    const gateway::ExecutionReport& a = reports_.items[0];
    EXPECT_EQ(a.kind, ReportKind::Accepted);
    EXPECT_EQ(a.client_id, 7U);
    EXPECT_EQ(a.order_ref, 555U);
    EXPECT_EQ(a.qty, 300U);
    EXPECT_EQ(a.venue_ns, 34'200'000'000'001ULL);
    EXPECT_EQ(a.rx_tsc, 2000U);

    const gateway::ExecutionReport& e = reports_.items[1];
    EXPECT_EQ(e.kind, ReportKind::Executed);
    EXPECT_EQ(e.qty, 120U);
    EXPECT_EQ(e.price_ticks, 4'251'000U);
    EXPECT_EQ(e.liquidity, 'R');
    EXPECT_EQ(e.match_number, 9001U);

    EXPECT_EQ(reports_.items[2].kind, ReportKind::Canceled);
    EXPECT_EQ(reports_.items[2].qty, 180U);
    EXPECT_EQ(reports_.items[2].reason, 'I');

    EXPECT_EQ(reports_.items[3].kind, ReportKind::Rejected);
    EXPECT_EQ(reports_.items[3].client_id, 9U);
    EXPECT_EQ(reports_.items[3].source, gateway::RejectSource::Venue);
    EXPECT_EQ(reports_.items[3].reason, 'X');

    EXPECT_EQ(reports_.items[4].kind, ReportKind::Replaced);
    EXPECT_EQ(reports_.items[4].client_id, 11U);
    EXPECT_EQ(reports_.items[4].related_id, 10U);

    EXPECT_EQ(reports_.items[5].kind, ReportKind::TradeBroken);
    EXPECT_EQ(reports_.items[5].match_number, 9001U);

    EXPECT_EQ(gw.stats().foreign_tokens, 1U);
    EXPECT_EQ(gw.next_sequence(), 8U);
}

TEST_F(GatewayTest, RejectsLocallyWithoutTouchingTheWire) {
    Gateway gw{wire_, requests_, reports_, tickers_, config_};

    requests_.items.push_back(new_order(1, Side::Buy, 100, 4'250'000));
    static_cast<void>(gw.poll());
    ASSERT_EQ(reports_.items.size(), 1U);
    EXPECT_EQ(reports_.items[0].kind, ReportKind::Rejected);
    EXPECT_EQ(reports_.items[0].source, gateway::RejectSource::NotConnected);
    EXPECT_TRUE(wire_.sent.empty());
    reports_.items.clear();

    bring_up(gw);
    requests_.items.push_back(new_order(2, Side::Buy, 1001, 4'250'000));
    requests_.items.push_back(new_order(3, Side::Buy, 1000, 20'000'000));
    gateway::OrderRequest bad_symbol = new_order(4, Side::Buy, 10, 4'250'000);
    bad_symbol.symbol = SymbolId{2};
    requests_.items.push_back(bad_symbol);
    gateway::OrderRequest bad_price = new_order(5, Side::Buy, 10, wire::ouch::kMaxPrice + 1);
    requests_.items.push_back(bad_price);
    static_cast<void>(gw.poll());

    ASSERT_EQ(reports_.items.size(), 4U);
    EXPECT_EQ(reports_.items[0].source, gateway::RejectSource::Risk);
    EXPECT_EQ(reports_.items[0].reason, 'Q');
    EXPECT_EQ(reports_.items[1].source, gateway::RejectSource::Risk);
    EXPECT_EQ(reports_.items[1].reason, 'N');
    EXPECT_EQ(reports_.items[2].source, gateway::RejectSource::BadRequest);
    EXPECT_EQ(reports_.items[3].source, gateway::RejectSource::BadRequest);
    EXPECT_TRUE(wire_.sent.empty());
    EXPECT_EQ(gw.stats().risk_rejects, 2U);
    EXPECT_EQ(gw.stats().bad_requests, 2U);
}

TEST_F(GatewayTest, SessionLossRejectsAndCancelFailuresAreNotOrderRejects) {
    Gateway gw{wire_, requests_, reports_, tickers_, config_};
    gw.connect();
    static_cast<void>(gw.poll());
    EXPECT_TRUE(reports_.items.empty());
    wire_.accept_login("SESS01", 1);
    static_cast<void>(gw.poll());
    ASSERT_EQ(reports_.items.size(), 1U);
    EXPECT_EQ(reports_.items[0].kind, ReportKind::SessionUp);
    reports_.items.clear();

    wire_.closed = true;
    static_cast<void>(gw.poll());
    ASSERT_EQ(reports_.items.size(), 1U);
    EXPECT_EQ(reports_.items[0].kind, ReportKind::SessionDown);
    reports_.items.clear();

    gateway::OrderRequest cancel{};
    cancel.kind = gateway::RequestKind::Cancel;
    cancel.client_id = 7;
    requests_.items.push_back(cancel);
    requests_.items.push_back(new_order(8, Side::Buy, 10, 4'250'000));
    static_cast<void>(gw.poll());
    ASSERT_EQ(reports_.items.size(), 2U);
    EXPECT_EQ(reports_.items[0].kind, ReportKind::CancelRejected);
    EXPECT_EQ(reports_.items[0].client_id, 7U);
    EXPECT_EQ(reports_.items[1].kind, ReportKind::Rejected);
    EXPECT_EQ(gw.stats().not_connected, 2U);
}

TEST(GatewayTcp, LogsInAndTradesOverLoopback) {
    net::TcpListener listener;
    ASSERT_TRUE(listener.listen(net::make_endpoint("127.0.0.1", 0).endpoint, 1).ok());
    net::TcpStream client;
    ASSERT_TRUE(client.connect(listener.local_endpoint().endpoint, 1000).ok());
    net::TcpStream server;
    ASSERT_TRUE(ipc::spin_until(
        [&] {
            if (listener.accept(server).ok()) {
                return true;
            }
            static_cast<void>(::usleep(10));
            return false;
        },
        1'000'000));

    test::QueueSource<gateway::OrderRequest> requests;
    test::VectorSink<gateway::ExecutionReport> reports;
    const std::array<wire::Alpha<8>, 1> tickers{wire::Alpha<8>{"MSFT"}};
    gateway::GatewayConfig config{};
    config.soup = soup_config();
    config.soup.heartbeat_ticks = ~std::uint64_t{0} >> 1;
    config.soup.timeout_ticks = ~std::uint64_t{0} >> 1;
    config.risk = {.max_order_qty = 1000, .max_order_notional = 10'000'000'000ULL,
                   .max_orders_per_window = 100, .window_ticks = 1};
    gateway::OrderGateway<net::TcpStream, test::QueueSource<gateway::OrderRequest>,
                          test::VectorSink<gateway::ExecutionReport>>
        gw{client, requests, reports, tickers, config};
    gw.connect();

    std::vector<std::byte> inbox;
    std::array<std::byte, 512> chunk{};
    const auto server_read = [&](std::size_t want) {
        return ipc::spin_until(
            [&] {
                static_cast<void>(gw.poll());
                const net::IoResult io = server.recv(chunk);
                if (io.ok()) {
                    inbox.insert(inbox.end(), chunk.begin(),
                                 chunk.begin() + static_cast<std::ptrdiff_t>(io.bytes));
                }
                if (inbox.size() >= want) {
                    return true;
                }
                static_cast<void>(::usleep(10));
                return false;
            },
            1'000'000);
    };
    const auto pump_until = [&](std::size_t report_count) {
        return ipc::spin_until(
            [&] {
                static_cast<void>(gw.poll());
                if (reports.items.size() >= report_count) {
                    return true;
                }
                static_cast<void>(::usleep(10));
                return false;
            },
            1'000'000);
    };

    ASSERT_TRUE(server_read(sizeof(wire::soup::LoginRequest)));
    auto accepted_login = wire::soup::make<wire::soup::LoginAccepted>(wire::soup::kLoginAccepted);
    accepted_login.session = wire::Alpha<10>::right_justified("LOOP");
    accepted_login.sequence = wire::format_numeric<20>(1);
    ASSERT_TRUE(server.send(std::as_bytes(std::span{&accepted_login, 1})).ok());
    ASSERT_TRUE(pump_until(1));
    EXPECT_EQ(reports.items[0].kind, ReportKind::SessionUp);

    gateway::OrderRequest order{};
    order.kind = gateway::RequestKind::New;
    order.client_id = 42;
    order.symbol = SymbolId{0};
    order.side = Side::Buy;
    order.qty = 100;
    order.price_ticks = 4'250'000;
    order.origin_tsc = tsc_now();
    requests.items.push_back(order);

    inbox.clear();
    ASSERT_TRUE(server_read(kSoupHeader + sizeof(wire::ouch::EnterOrder)));
    wire::ouch::EnterOrder enter{};
    std::memcpy(&enter, inbox.data() + kSoupHeader, sizeof(enter));
    EXPECT_EQ(enter.token, gateway::make_token('H', 42));
    EXPECT_EQ(enter.stock.view(), "MSFT");
    EXPECT_EQ(enter.shares.get(), 100U);

    gateway::Frame<wire::ouch::Executed> fill{
        wire::soup::make_header(wire::soup::kSequencedData, sizeof(wire::ouch::Executed)),
        wire::ouch::make<wire::ouch::Executed>()};
    fill.msg.token = enter.token;
    fill.msg.executed_shares.set(100);
    fill.msg.execution_price.set(4'250'000);
    ASSERT_TRUE(server.send(std::as_bytes(std::span{&fill, 1})).ok());
    ASSERT_TRUE(pump_until(2));
    EXPECT_EQ(reports.items[1].kind, ReportKind::Executed);
    EXPECT_EQ(reports.items[1].client_id, 42U);
    EXPECT_EQ(reports.items[1].qty, 100U);
    EXPECT_EQ(gw.stats().tick_to_trade.count, 1U);
    EXPECT_GT(gw.stats().tick_to_trade.max, 0U);
}

} // namespace
