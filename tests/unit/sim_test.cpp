#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "hotpath/gateway/token.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"
#include "hotpath/sim/itch_publisher.hpp"
#include "hotpath/sim/liquidity.hpp"
#include "hotpath/sim/matching_engine.hpp"
#include "hotpath/sim/soup_server.hpp"
#include "hotpath/sim/token_map.hpp"
#include "hotpath/sim/venue.hpp"
#include "hotpath/wire/itch.hpp"
#include "hotpath/wire/mold.hpp"
#include "hotpath/wire/ouch.hpp"
#include "hotpath/wire/soup.hpp"
#include "support/fakes.hpp"

using namespace hotpath;
using sim::OrderHandle;
using test::FakeTransport;

namespace {

struct Recorder {
    struct TradeEvent {
        std::uint64_t resting_ref;
        std::uint64_t aggressor_ref;
        std::uint32_t qty;
        std::uint32_t ticks;
        std::uint64_t match;
        bool done;
    };
    std::vector<TradeEvent> trades;
    std::vector<std::uint64_t> rested;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reduced;
    std::uint32_t removed{0};

    void on_trade(const sim::RestingOrder& resting, std::uint64_t aggressor_ref, std::uint64_t,
                  Side, std::uint32_t qty, std::uint32_t ticks, std::uint64_t match, bool done) {
        trades.push_back({resting.ref, aggressor_ref, qty, ticks, match, done});
    }
    void on_rest(const sim::RestingOrder& order) { rested.push_back(order.ref); }
    void on_reduce(const sim::RestingOrder& order, std::uint32_t qty, bool gone) {
        reduced.emplace_back(order.ref, qty);
        removed += gone ? 1U : 0U;
    }
};

struct CaptureFeed {
    std::vector<std::vector<std::byte>> messages;

    template <wire::WireMessage M>
    void publish(const M& msg) {
        const auto bytes = std::as_bytes(std::span<const M, 1>{&msg, 1});
        messages.emplace_back(bytes.begin(), bytes.end());
    }

    [[nodiscard]] char type(std::size_t i) const { return static_cast<char>(messages[i][0]); }

    template <wire::WireMessage M>
    [[nodiscard]] M as(std::size_t i) const {
        M out{};
        std::memcpy(&out, messages[i].data(), sizeof(M));
        return out;
    }

    [[nodiscard]] std::size_t count(char wanted) const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            n += type(i) == wanted ? 1U : 0U;
        }
        return n;
    }
};

struct CaptureSession : CaptureFeed {
    template <wire::WireMessage M>
    void send_sequenced(const M& msg) {
        publish(msg);
    }
};

struct PacketCapture {
    std::vector<std::vector<std::byte>> packets;
    net::IoResult send(std::span<const std::byte> data) {
        packets.emplace_back(data.begin(), data.end());
        return net::IoResult{data.size(), Status{}, false};
    }
};

class SimTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(region_.map(std::size_t{32} << 20, sys::PageKind::Small).ok());
        region_.prefault();
        arena_ = mem::Arena{region_};
    }

    mem::Region region_;
    mem::Arena arena_;
};

TEST_F(SimTest, EngineMatchesInPriceTimePriority) {
    sim::MatchingEngine engine;
    ASSERT_TRUE(engine.init(arena_, {.symbols = 2, .max_orders = 64, .levels_per_side = 4}).ok());
    Recorder rec;

    const auto a = engine.submit(rec, 0, Side::Sell, 100, 10'100, false, 0);
    const auto b = engine.submit(rec, 0, Side::Sell, 100, 10'000, false, 0);
    const auto c = engine.submit(rec, 0, Side::Sell, 50, 10'000, false, 0);
    ASSERT_TRUE(a.accepted && b.resting.valid() && c.resting.valid());
    EXPECT_EQ(engine.best(0, Side::Sell).ticks, 10'000U);
    EXPECT_EQ(engine.best(0, Side::Sell).qty, 150U);
    EXPECT_EQ(engine.depth(0, Side::Sell), 2U);
    EXPECT_EQ(rec.rested.size(), 3U);

    const auto buy = engine.submit(rec, 0, Side::Buy, 180, 10'100, false, 7);
    EXPECT_EQ(buy.filled, 180U);
    EXPECT_FALSE(buy.resting.valid());
    ASSERT_EQ(rec.trades.size(), 3U);
    EXPECT_EQ(rec.trades[0].resting_ref, b.ref);
    EXPECT_EQ(rec.trades[0].qty, 100U);
    EXPECT_EQ(rec.trades[0].ticks, 10'000U);
    EXPECT_TRUE(rec.trades[0].done);
    EXPECT_EQ(rec.trades[1].resting_ref, c.ref);
    EXPECT_EQ(rec.trades[1].qty, 50U);
    EXPECT_EQ(rec.trades[2].resting_ref, a.ref);
    EXPECT_EQ(rec.trades[2].qty, 30U);
    EXPECT_EQ(rec.trades[2].ticks, 10'100U);
    EXPECT_FALSE(rec.trades[2].done);
    EXPECT_EQ(rec.trades[2].match, 3U);
    EXPECT_EQ(engine.best(0, Side::Sell).qty, 70U);
    EXPECT_EQ(engine.find(b.resting), nullptr);
    ASSERT_NE(engine.find(a.resting), nullptr);
    EXPECT_EQ(engine.find(a.resting)->qty, 70U);

    const auto passive = engine.submit(rec, 0, Side::Buy, 40, 10'050, false, 0);
    EXPECT_EQ(passive.filled, 0U);
    EXPECT_TRUE(passive.resting.valid());
    EXPECT_EQ(engine.best(0, Side::Buy).ticks, 10'050U);

    const auto ioc = engine.submit(rec, 0, Side::Sell, 100, 10'050, true, 0);
    EXPECT_EQ(ioc.filled, 40U);
    EXPECT_EQ(ioc.canceled, 60U);
    EXPECT_FALSE(ioc.resting.valid());
    EXPECT_EQ(engine.depth(0, Side::Buy), 0U);
    EXPECT_EQ(engine.depth(1, Side::Sell), 0U);
    EXPECT_EQ(engine.resting_orders(), 1U);
}

TEST_F(SimTest, EngineReducesRemovesAndBoundsCapacity) {
    sim::MatchingEngine engine;
    ASSERT_TRUE(engine.init(arena_, {.symbols = 1, .max_orders = 4, .levels_per_side = 2}).ok());
    Recorder rec;

    const auto a = engine.submit(rec, 0, Side::Buy, 100, 9'900, false, 0);
    const auto b = engine.submit(rec, 0, Side::Buy, 100, 9'900, false, 0);
    EXPECT_EQ(engine.reduce(rec, a.resting, 150), 0U);
    EXPECT_EQ(engine.reduce(rec, a.resting, 30), 70U);
    EXPECT_EQ(engine.best(0, Side::Buy).qty, 130U);
    EXPECT_EQ(engine.reduce(rec, a.resting, 0), 30U);
    EXPECT_EQ(rec.removed, 1U);
    EXPECT_EQ(engine.reduce(rec, a.resting, 0), 0U);
    EXPECT_EQ(engine.best(0, Side::Buy).orders, 1U);

    const auto reused = engine.submit(rec, 0, Side::Buy, 10, 9'800, false, 0);
    EXPECT_EQ(reused.resting.index, a.resting.index);
    EXPECT_EQ(engine.find(a.resting), nullptr);

    const auto no_level = engine.submit(rec, 0, Side::Buy, 10, 9'700, false, 0);
    EXPECT_TRUE(no_level.accepted);
    EXPECT_EQ(no_level.canceled, 10U);
    EXPECT_FALSE(no_level.resting.valid());

    static_cast<void>(engine.submit(rec, 0, Side::Buy, 10, 9'900, false, 0));
    static_cast<void>(engine.submit(rec, 0, Side::Buy, 10, 9'900, false, 0));
    const auto no_pool = engine.submit(rec, 0, Side::Buy, 10, 9'900, false, 0);
    EXPECT_EQ(no_pool.canceled, 10U);

    EXPECT_FALSE(engine.submit(rec, 1, Side::Buy, 10, 9'900, false, 0).accepted);
    EXPECT_FALSE(engine.submit(rec, 0, Side::Buy, 0, 9'900, false, 0).accepted);
    EXPECT_FALSE(engine.submit(rec, 0, Side::Buy, 10, 0, false, 0).accepted);
    EXPECT_TRUE(engine.find(b.resting) != nullptr);
}

TEST_F(SimTest, TokenMapIsInsertOnce) {
    sim::TokenMap tokens;
    ASSERT_TRUE(tokens.init(arena_, 64).ok());
    for (std::uint64_t id = 1; id <= 48; ++id) {
        sim::TokenEntry* entry = tokens.insert(gateway::make_token('H', id));
        ASSERT_NE(entry, nullptr);
        entry->symbol = static_cast<std::uint16_t>(id);
        EXPECT_EQ(tokens.by_user(tokens.user_of(*entry)), entry);
    }
    EXPECT_EQ(tokens.insert(gateway::make_token('H', 49)), nullptr);
    EXPECT_EQ(tokens.insert(gateway::make_token('H', 7)), nullptr);
    for (std::uint64_t id = 1; id <= 48; ++id) {
        sim::TokenEntry* entry = tokens.find(gateway::make_token('H', id));
        ASSERT_NE(entry, nullptr);
        EXPECT_EQ(entry->symbol, id);
    }
    EXPECT_EQ(tokens.find(gateway::make_token('H', 99)), nullptr);
    EXPECT_EQ(tokens.by_user(0), nullptr);
    EXPECT_EQ(tokens.by_user(65), nullptr);
}

TEST_F(SimTest, PublisherPacketizesSequencesAndRetransmits) {
    PacketCapture capture;
    sim::ItchPublisher<PacketCapture> publisher{capture};
    ASSERT_TRUE(publisher.init(arena_, {.store_capacity = 64}).ok());

    auto add = wire::itch::make<wire::itch::AddOrder>();
    for (std::uint64_t ref = 1; ref <= 50; ++ref) {
        add.order_ref.set(ref);
        publisher.publish(add);
    }
    publisher.flush();
    publisher.flush();
    ASSERT_EQ(capture.packets.size(), 2U);
    EXPECT_EQ(publisher.next_sequence(), 51U);

    std::uint64_t expected = 1;
    for (const auto& packet : capture.packets) {
        EXPECT_LE(packet.size(), wire::mold::kMaxPacketBytes);
        EXPECT_EQ(wire::mold::for_each_message(
                      packet,
                      [&](std::uint64_t seq, std::span<const std::byte> msg) {
                          EXPECT_EQ(seq, expected);
                          wire::itch::AddOrder got{};
                          ASSERT_EQ(msg.size(), sizeof(got));
                          std::memcpy(&got, msg.data(), sizeof(got));
                          EXPECT_EQ(got.order_ref.get(), expected);
                          expected += 1;
                      }),
                  wire::Parse::Ok);
    }
    EXPECT_EQ(expected, 51U);

    publisher.heartbeat();
    ASSERT_EQ(capture.packets.size(), 3U);
    const wire::mold::Header* hb = wire::mold::parse_header(capture.packets[2]);
    ASSERT_NE(hb, nullptr);
    EXPECT_EQ(hb->count.get(), 0U);
    EXPECT_EQ(hb->sequence.get(), 51U);

    std::array<std::byte, wire::mold::kMaxPacketBytes> out{};
    const auto reply = publisher.retransmit(48, 10, out);
    const wire::mold::Header* hdr = wire::mold::parse_header(reply);
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->sequence.get(), 48U);
    EXPECT_EQ(hdr->count.get(), 3U);
    EXPECT_TRUE(publisher.retransmit(51, 1, out).empty());
    EXPECT_TRUE(publisher.retransmit(0, 1, out).empty());

    for (std::uint64_t ref = 51; ref <= 100; ++ref) {
        publisher.publish(add);
    }
    EXPECT_TRUE(publisher.retransmit(30, 1, out).empty());
    EXPECT_FALSE(publisher.retransmit(40, 1, out).empty());
}

struct Inbox {
    std::vector<std::vector<std::byte>> messages;
    void on_unsequenced(std::span<const std::byte> payload) {
        messages.emplace_back(payload.begin(), payload.end());
    }
};

void client_login(FakeTransport& wire, std::uint64_t sequence, const char* session = "") {
    auto login = wire::soup::make<wire::soup::LoginRequest>(wire::soup::kLoginRequest);
    login.username = wire::Alpha<6>{"paul"};
    login.requested_session = wire::Alpha<10>::right_justified(session);
    login.requested_sequence = wire::format_numeric<20>(sequence);
    wire.feed(std::as_bytes(std::span<const wire::soup::LoginRequest, 1>{&login, 1}));
}

std::vector<wire::soup::Packet> split(const std::vector<std::byte>& stream) {
    std::vector<wire::soup::Packet> out;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const wire::soup::Packet packet = wire::soup::next_packet(
            std::span<const std::byte>{stream.data() + offset, stream.size() - offset});
        if (packet.result != wire::Parse::Ok) {
            break;
        }
        out.push_back(packet);
        offset += packet.consumed;
    }
    return out;
}

TEST_F(SimTest, SoupServerLogsInSequencesAndReplays) {
    sim::SoupServer<FakeTransport> server;
    ASSERT_TRUE(server.init(arena_, {.heartbeat_ticks = 1000, .log_capacity = 8}).ok());
    Inbox inbox;

    auto exec = wire::ouch::make<wire::ouch::Executed>();
    exec.executed_shares.set(1);
    server.send_sequenced(exec);
    server.send_sequenced(exec);

    FakeTransport first;
    server.attach(&first, 0);
    auto order = wire::ouch::make<wire::ouch::CancelOrder>();
    first.feed_packet(wire::soup::kUnsequencedData, order);
    server.poll(1, inbox);
    EXPECT_TRUE(inbox.messages.empty());

    client_login(first, 1);
    first.feed_packet(wire::soup::kUnsequencedData, order);
    first.send_limit = 11;
    for (std::uint64_t now = 2; now < 40; ++now) {
        server.poll(now, inbox);
    }
    EXPECT_TRUE(server.logged_in());
    EXPECT_EQ(inbox.messages.size(), 1U);
    auto packets = split(first.sent);
    ASSERT_EQ(packets.size(), 3U);
    EXPECT_EQ(packets[0].type, wire::soup::kLoginAccepted);
    wire::soup::LoginAccepted accepted{};
    std::memcpy(&accepted, first.sent.data(), sizeof(accepted));
    EXPECT_EQ(accepted.session.trimmed(), "HOTPATH001");
    EXPECT_EQ(wire::parse_numeric(accepted.sequence.raw()).value, 1U);
    EXPECT_EQ(packets[1].type, wire::soup::kSequencedData);
    EXPECT_EQ(packets[2].payload.size(), sizeof(exec));

    first.send_limit = ~std::size_t{0};
    server.poll(2000, inbox);
    packets = split(first.sent);
    ASSERT_EQ(packets.size(), 4U);
    EXPECT_EQ(packets[3].type, wire::soup::kServerHeartbeat);

    server.send_sequenced(exec);
    FakeTransport second;
    server.attach(&second, 3000);
    client_login(second, 3, "HOTPATH001");
    server.poll(3001, inbox);
    packets = split(second.sent);
    ASSERT_EQ(packets.size(), 2U);
    EXPECT_EQ(packets[1].type, wire::soup::kSequencedData);
    EXPECT_EQ(server.client_cursor(), 4U);

    FakeTransport wrong;
    server.attach(&wrong, 4000);
    client_login(wrong, 1, "OTHER");
    server.poll(4001, inbox);
    packets = split(wrong.sent);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].type, wire::soup::kLoginRejected);
    EXPECT_FALSE(server.logged_in());

    for (int i = 0; i < 10; ++i) {
        server.send_sequenced(exec);
    }
    FakeTransport stale;
    server.attach(&stale, 5000);
    client_login(stale, 2);
    server.poll(5001, inbox);
    EXPECT_EQ(split(stale.sent)[0].type, wire::soup::kLoginRejected);

    FakeTransport latest;
    server.attach(&latest, 6000);
    client_login(latest, 0);
    server.poll(6001, inbox);
    wire::soup::LoginAccepted tail{};
    std::memcpy(&tail, latest.sent.data(), sizeof(tail));
    EXPECT_EQ(wire::parse_numeric(tail.sequence.raw()).value, server.next_sequence());

    latest.closed = true;
    server.poll(6002, inbox);
    EXPECT_FALSE(server.connected());
}

class VenueTest : public SimTest {
protected:
    using VenueType = sim::Venue<CaptureFeed, CaptureSession>;

    void SetUp() override {
        SimTest::SetUp();
        ASSERT_TRUE(engine_.init(arena_, {.symbols = 2, .max_orders = 256,
                                          .levels_per_side = 16}).ok());
        ASSERT_TRUE(tokens_.init(arena_, 256).ok());
    }

    static wire::ouch::EnterOrder enter(std::uint64_t id, char side, std::uint32_t shares,
                                        std::uint32_t price, std::uint32_t tif,
                                        const char* stock = "MSFT") {
        auto m = wire::ouch::make<wire::ouch::EnterOrder>();
        m.token = gateway::make_token('H', id);
        m.side = side;
        m.shares.set(shares);
        m.stock = wire::Alpha<8>{stock};
        m.price.set(price);
        m.time_in_force.set(tif);
        return m;
    }

    sim::MatchingEngine engine_;
    sim::TokenMap tokens_;
    CaptureFeed feed_;
    CaptureSession session_;
    std::array<wire::Alpha<8>, 2> tickers_{wire::Alpha<8>{"MSFT"}, wire::Alpha<8>{"NVDA"}};
};

TEST_F(VenueTest, OpensMarketWithDirectoryAndTradingState) {
    VenueType venue{engine_, tokens_, feed_, session_, tickers_};
    venue.set_time(34'200'000'000'000ULL);
    venue.open_market();
    ASSERT_EQ(feed_.messages.size(), 7U);
    EXPECT_EQ(feed_.count('S'), 3U);
    const auto dir = feed_.as<wire::itch::StockDirectory>(5);
    EXPECT_EQ(dir.stock.view(), "NVDA");
    EXPECT_EQ(dir.hdr.stock_locate.get(), 2U);
    EXPECT_EQ(dir.hdr.timestamp_ns.get(), 34'200'000'000'000ULL);
    EXPECT_EQ(feed_.as<wire::itch::StockTradingAction>(6).trading_state, 'T');
    ASSERT_EQ(session_.messages.size(), 1U);
    EXPECT_EQ(session_.type(0), 'S');
}

TEST_F(VenueTest, ClientTakerGetsAcceptedThenFillThenIocCancel) {
    VenueType venue{engine_, tokens_, feed_, session_, tickers_};
    const OrderHandle ask = venue.house_submit(0, Side::Sell, 60, 4'251'000, false);
    ASSERT_TRUE(ask.valid());
    ASSERT_EQ(feed_.messages.size(), 1U);
    EXPECT_EQ(feed_.type(0), 'A');
    EXPECT_EQ(feed_.as<wire::itch::AddOrder>(0).stock.view(), "MSFT");
    EXPECT_TRUE(session_.messages.empty());

    venue.on(enter(1, 'B', 100, 4'251'000, wire::ouch::kTifImmediateOrCancel));
    ASSERT_EQ(session_.messages.size(), 3U);
    const auto accepted = session_.as<wire::ouch::Accepted>(0);
    EXPECT_EQ(accepted.type, 'A');
    EXPECT_EQ(accepted.token, gateway::make_token('H', 1));
    EXPECT_EQ(accepted.shares.get(), 100U);
    EXPECT_EQ(accepted.order_state, 'L');
    const auto exec = session_.as<wire::ouch::Executed>(1);
    EXPECT_EQ(exec.type, 'E');
    EXPECT_EQ(exec.executed_shares.get(), 60U);
    EXPECT_EQ(exec.execution_price.get(), 4'251'000U);
    EXPECT_EQ(exec.liquidity_flag, 'R');
    const auto canceled = session_.as<wire::ouch::Canceled>(2);
    EXPECT_EQ(canceled.type, 'C');
    EXPECT_EQ(canceled.decrement_shares.get(), 40U);
    EXPECT_EQ(canceled.reason, 'I');

    ASSERT_EQ(feed_.messages.size(), 2U);
    const auto print = feed_.as<wire::itch::OrderExecuted>(1);
    EXPECT_EQ(print.hdr.type, 'E');
    EXPECT_EQ(print.order_ref.get(), ask.ref);
    EXPECT_EQ(print.executed_shares.get(), 60U);
    EXPECT_EQ(print.match_number.get(), exec.match_number.get());
    EXPECT_EQ(engine_.depth(0, Side::Sell), 0U);
}

TEST_F(VenueTest, RestingClientOrderIsHitCanceledAndReplaced) {
    VenueType venue{engine_, tokens_, feed_, session_, tickers_};
    venue.on(enter(1, 'B', 100, 4'250'000, wire::ouch::kTifMarketHours));
    ASSERT_EQ(session_.messages.size(), 1U);
    ASSERT_EQ(feed_.messages.size(), 1U);
    const auto add = feed_.as<wire::itch::AddOrder>(0);
    EXPECT_EQ(add.side, 'B');
    EXPECT_EQ(add.order_ref.get(), session_.as<wire::ouch::Accepted>(0).order_ref.get());

    static_cast<void>(venue.house_submit(0, Side::Sell, 30, 4'250'000, true));
    ASSERT_EQ(session_.messages.size(), 2U);
    const auto fill = session_.as<wire::ouch::Executed>(1);
    EXPECT_EQ(fill.executed_shares.get(), 30U);
    EXPECT_EQ(fill.liquidity_flag, 'A');

    auto cancel = wire::ouch::make<wire::ouch::CancelOrder>();
    cancel.token = gateway::make_token('H', 1);
    cancel.shares.set(50);
    venue.on(cancel);
    ASSERT_EQ(session_.messages.size(), 3U);
    EXPECT_EQ(session_.as<wire::ouch::Canceled>(2).decrement_shares.get(), 20U);
    EXPECT_EQ(session_.as<wire::ouch::Canceled>(2).reason, 'U');
    EXPECT_EQ(feed_.type(feed_.messages.size() - 1), 'X');
    EXPECT_EQ(engine_.best(0, Side::Buy).qty, 50U);

    auto replace = wire::ouch::make<wire::ouch::ReplaceOrder>();
    replace.existing_token = gateway::make_token('H', 1);
    replace.replacement_token = gateway::make_token('H', 2);
    replace.shares.set(80);
    replace.price.set(4'249'000);
    replace.time_in_force.set(wire::ouch::kTifMarketHours);
    venue.on(replace);
    ASSERT_EQ(session_.messages.size(), 4U);
    const auto replaced = session_.as<wire::ouch::Replaced>(3);
    EXPECT_EQ(replaced.type, 'U');
    EXPECT_EQ(replaced.replacement_token, gateway::make_token('H', 2));
    EXPECT_EQ(replaced.previous_token, gateway::make_token('H', 1));
    EXPECT_EQ(replaced.side, 'B');
    EXPECT_EQ(replaced.stock.view(), "MSFT");
    EXPECT_EQ(feed_.type(feed_.messages.size() - 2), 'D');
    EXPECT_EQ(feed_.type(feed_.messages.size() - 1), 'A');
    EXPECT_EQ(engine_.best(0, Side::Buy).ticks, 4'249'000U);
    EXPECT_EQ(engine_.best(0, Side::Buy).qty, 80U);

    venue.on(cancel);
    EXPECT_EQ(session_.messages.size(), 4U);

    cancel.token = gateway::make_token('H', 2);
    cancel.shares.set(0);
    venue.on(cancel);
    ASSERT_EQ(session_.messages.size(), 5U);
    EXPECT_EQ(session_.as<wire::ouch::Canceled>(4).decrement_shares.get(), 80U);
    EXPECT_EQ(feed_.type(feed_.messages.size() - 1), 'D');
    EXPECT_EQ(engine_.resting_orders(), 0U);

    static_cast<void>(venue.house_submit(0, Side::Sell, 30, 4'240'000, true));
    EXPECT_EQ(session_.messages.size(), 5U);
}

TEST_F(VenueTest, RejectsBadOrdersAndIgnoresDuplicateTokens) {
    VenueType venue{engine_, tokens_, feed_, session_, tickers_};
    venue.on(enter(1, 'B', 100, 4'250'000, 99'998, "ZZZZ"));
    venue.on(enter(2, 'B', 0, 4'250'000, 99'998));
    venue.on(enter(3, 'B', 1'000'000, 4'250'000, 99'998));
    venue.on(enter(4, 'B', 100, wire::ouch::kMarketPrice, 99'998));
    venue.on(enter(5, 'Q', 100, 4'250'000, 99'998));
    ASSERT_EQ(session_.messages.size(), 5U);
    const char reasons[] = {'S', 'Z', 'Z', 'X', 'Y'};
    for (std::size_t i = 0; i < 5; ++i) {
        const auto rejected = session_.as<wire::ouch::Rejected>(i);
        EXPECT_EQ(rejected.type, 'J');
        EXPECT_EQ(rejected.reason, reasons[i]);
    }
    EXPECT_TRUE(feed_.messages.empty());

    venue.on(enter(1, 'B', 100, 4'250'000, 99'998));
    EXPECT_EQ(session_.messages.size(), 5U);
    EXPECT_EQ(venue.stats().duplicate_tokens, 1U);

    venue.on(enter(6, 'T', 100, 4'250'000, 99'998, "NVDA"));
    ASSERT_EQ(session_.messages.size(), 6U);
    EXPECT_EQ(feed_.as<wire::itch::AddOrder>(0).side, 'S');
    EXPECT_EQ(feed_.as<wire::itch::AddOrder>(0).hdr.stock_locate.get(), 2U);
}

TEST_F(VenueTest, LiquidityIsDeterministicAndBounded) {
    const auto run = [&](std::uint64_t seed) {
        mem::Region region;
        EXPECT_TRUE(region.map(std::size_t{8} << 20, sys::PageKind::Small).ok());
        region.prefault();
        mem::Arena arena{region};
        sim::MatchingEngine engine;
        sim::TokenMap tokens;
        EXPECT_TRUE(engine.init(arena, {.symbols = 2, .max_orders = 1024,
                                        .levels_per_side = 64}).ok());
        EXPECT_TRUE(tokens.init(arena, 64).ok());
        CaptureFeed feed;
        CaptureSession session;
        VenueType venue{engine, tokens, feed, session, tickers_};
        sim::Liquidity liquidity;
        EXPECT_TRUE(liquidity.init(arena, 2, {.seed = seed, .max_live = 16}).ok());
        for (int i = 0; i < 20'000; ++i) {
            liquidity.step(venue);
        }
        EXPECT_LE(engine.resting_orders(), 32U);
        EXPECT_GT(feed.count('E'), 100U);
        EXPECT_GT(feed.count('A'), 1000U);
        EXPECT_GT(feed.count('D'), 100U);
        EXPECT_TRUE(session.messages.empty());
        std::uint64_t digest = feed.messages.size();
        for (const auto& msg : feed.messages) {
            for (const std::byte b : msg) {
                digest = digest * 1099511628211ULL + static_cast<std::uint64_t>(b);
            }
        }
        return digest;
    };
    const std::uint64_t first = run(11);
    EXPECT_EQ(first, run(11));
    EXPECT_NE(first, run(12));
}

} // namespace
