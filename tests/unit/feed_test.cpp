#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "hotpath/book/book_set.hpp"
#include "hotpath/feed/book_builder.hpp"
#include "hotpath/feed/feed_handler.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/feed/sequencer.hpp"
#include "hotpath/feed/symbol_table.hpp"
#include "hotpath/ipc/spin.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"
#include "hotpath/net/udp.hpp"
#include "hotpath/wire/itch.hpp"
#include "hotpath/wire/mold.hpp"

using namespace hotpath;
using feed::Kind;
using feed::Source;
using feed::Verdict;

namespace {

constexpr std::uint16_t kMsftLocate = 42;
constexpr std::uint16_t kAaplLocate = 7;

struct VectorSink {
    std::vector<feed::MarketUpdate> updates;
    feed::MarketUpdate scratch{};
    bool full{false};

    feed::MarketUpdate* claim() { return full ? nullptr : &scratch; }
    void publish() { updates.push_back(scratch); }
};

static_assert(feed::UpdateSink<VectorSink>);
static_assert(feed::UpdateSink<ipc::SpscProducer<feed::MarketUpdate>>);

using Packet = std::vector<std::byte>;

class PacketBuilder {
public:
    explicit PacketBuilder(std::uint64_t sequence) : writer_{buffer_} {
        writer_.begin(wire::Alpha<10>{"HOTPATH01"}, sequence);
    }

    template <wire::WireMessage M>
    PacketBuilder& add(const M& msg) {
        EXPECT_TRUE(writer_.append(msg));
        return *this;
    }

    Packet build() {
        const auto bytes = writer_.finish();
        return Packet{bytes.begin(), bytes.end()};
    }

private:
    std::array<std::byte, wire::mold::kMaxPacketBytes> buffer_{};
    wire::mold::PacketWriter writer_;
};

wire::itch::StockDirectory directory(std::uint16_t locate, const char* ticker) {
    auto m = wire::itch::make<wire::itch::StockDirectory>();
    m.hdr.stock_locate.set(locate);
    m.stock = wire::Alpha<8>{ticker};
    return m;
}

wire::itch::AddOrder add_order(std::uint16_t locate, std::uint64_t ref, char side,
                               std::uint32_t shares, std::uint32_t price) {
    auto m = wire::itch::make<wire::itch::AddOrder>();
    m.hdr.stock_locate.set(locate);
    m.hdr.timestamp_ns.set(34'200'000'000'000ULL + ref);
    m.order_ref.set(ref);
    m.side = side;
    m.shares.set(shares);
    m.price.set(price);
    return m;
}

wire::itch::OrderExecuted executed(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
    auto m = wire::itch::make<wire::itch::OrderExecuted>();
    m.hdr.stock_locate.set(locate);
    m.order_ref.set(ref);
    m.executed_shares.set(shares);
    return m;
}

wire::itch::OrderCancel cancel(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares) {
    auto m = wire::itch::make<wire::itch::OrderCancel>();
    m.hdr.stock_locate.set(locate);
    m.order_ref.set(ref);
    m.cancelled_shares.set(shares);
    return m;
}

wire::itch::OrderDelete remove_order(std::uint16_t locate, std::uint64_t ref) {
    auto m = wire::itch::make<wire::itch::OrderDelete>();
    m.hdr.stock_locate.set(locate);
    m.order_ref.set(ref);
    return m;
}

wire::itch::OrderReplace replace(std::uint16_t locate, std::uint64_t old_ref, std::uint64_t new_ref,
                                 std::uint32_t shares, std::uint32_t price) {
    auto m = wire::itch::make<wire::itch::OrderReplace>();
    m.hdr.stock_locate.set(locate);
    m.original_order_ref.set(old_ref);
    m.new_order_ref.set(new_ref);
    m.shares.set(shares);
    m.price.set(price);
    return m;
}

wire::itch::StockTradingAction trading_action(std::uint16_t locate, char state) {
    auto m = wire::itch::make<wire::itch::StockTradingAction>();
    m.hdr.stock_locate.set(locate);
    m.trading_state = state;
    return m;
}

Packet heartbeat(std::uint64_t next_sequence) { return PacketBuilder{next_sequence}.build(); }

TEST(Sequencer, ClassifiesPackets) {
    const wire::Alpha<10> session{"S1"};
    feed::Sequencer seq{1};
    EXPECT_EQ(seq.inspect(session, 1, 3).verdict, Verdict::Process);
    seq.commit(4);
    EXPECT_EQ(seq.inspect(session, 1, 3).verdict, Verdict::Duplicate);
    EXPECT_EQ(seq.inspect(session, 3, 2).verdict, Verdict::Process);
    EXPECT_EQ(seq.inspect(session, 4, 0).verdict, Verdict::Heartbeat);
    EXPECT_EQ(seq.inspect(session, 4, wire::mold::kEndOfSession).verdict, Verdict::EndOfSession);

    const feed::Decision gap = seq.inspect(session, 9, 2);
    EXPECT_EQ(gap.verdict, Verdict::Gap);
    EXPECT_EQ(gap.missing, 5U);
    EXPECT_EQ(seq.inspect(session, 6, 0).verdict, Verdict::Gap);
    EXPECT_EQ(seq.inspect(wire::Alpha<10>{"S2"}, 4, 1).verdict, Verdict::WrongSession);

    feed::Sequencer late{0};
    EXPECT_EQ(late.inspect(session, 500, 2).verdict, Verdict::Process);
    EXPECT_EQ(late.expected(), 500U);
}

class FeedTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(region_.map(std::size_t{16} << 20, sys::PageKind::Small).ok());
        region_.prefault();
        arena_ = mem::Arena{region_};
        ASSERT_TRUE(books_.init(arena_, {.max_symbols = 4, .max_orders = 1024,
                                         .levels_per_side = 32}).ok());
        ASSERT_TRUE(symbols_.init(arena_, 4).ok());
        ASSERT_TRUE(symbols_.watch("MSFT").ok());
        ASSERT_TRUE(symbols_.watch("NVDA").ok());
    }

    static Packet opening_packet() {
        return PacketBuilder{1}
            .add(directory(kAaplLocate, "AAPL"))
            .add(directory(kMsftLocate, "MSFT"))
            .add(add_order(kMsftLocate, 100, 'B', 300, 4'250'000))
            .add(add_order(kMsftLocate, 101, 'S', 200, 4'251'000))
            .add(add_order(kAaplLocate, 900, 'B', 50, 2'000'000))
            .build();
    }

    mem::Region region_;
    mem::Arena arena_;
    book::BookSet books_;
    feed::SymbolTable symbols_;
    VectorSink sink_;
};

TEST_F(FeedTest, SymbolTableResolvesWatchedTickers) {
    EXPECT_EQ(symbols_.watch("MSFT").err(), EEXIST);
    EXPECT_EQ(symbols_.watch("TOOLONGXX").err(), EINVAL);
    EXPECT_EQ(symbols_.lookup(kMsftLocate), feed::SymbolTable::kNone);

    EXPECT_EQ(symbols_.resolve(kAaplLocate, wire::Alpha<8>{"AAPL"}), feed::SymbolTable::kNone);
    EXPECT_EQ(symbols_.resolve(kMsftLocate, wire::Alpha<8>{"MSFT"}), 0U);
    EXPECT_EQ(symbols_.lookup(kMsftLocate), 0U);
    EXPECT_EQ(symbols_.lookup(kAaplLocate), feed::SymbolTable::kNone);
    EXPECT_EQ(symbols_.ticker(SymbolId{1}).view(), "NVDA");
    EXPECT_EQ(symbols_.resolved(), 1U);

    EXPECT_EQ(symbols_.resolve(50, wire::Alpha<8>{"MSFT"}), 0U);
    EXPECT_EQ(symbols_.lookup(kMsftLocate), feed::SymbolTable::kNone);
    EXPECT_EQ(symbols_.lookup(50), 0U);
    EXPECT_EQ(symbols_.resolved(), 1U);
}

TEST_F(FeedTest, BuildsBookAndPublishesTopChanges) {
    feed::FeedHandler<VectorSink> handler{books_, symbols_, sink_};
    handler.on_packet(opening_packet(), Source::Live, 1111);

    ASSERT_EQ(sink_.updates.size(), 2U);
    const feed::MarketUpdate& bid = sink_.updates[0];
    EXPECT_EQ(bid.kind, Kind::TopOfBook);
    EXPECT_EQ(bid.symbol, SymbolId{0});
    EXPECT_EQ(bid.sequence, 3U);
    EXPECT_EQ(bid.rx_tsc, 1111U);
    EXPECT_EQ(bid.exchange_ns, 34'200'000'000'100ULL);
    EXPECT_EQ(bid.bid_ticks, 4'250'000U);
    EXPECT_EQ(bid.bid_qty, 300U);
    EXPECT_FALSE(bid.has_ask());
    EXPECT_TRUE(bid.tradable());

    const feed::MarketUpdate& both = sink_.updates[1];
    EXPECT_EQ(both.sequence, 4U);
    EXPECT_EQ(both.ask_ticks, 4'251'000U);
    EXPECT_EQ(both.ask_qty, 200U);
    EXPECT_EQ(books_.open_orders(), 2U);
    EXPECT_EQ(handler.expected_sequence(), 6U);

    handler.on_packet(PacketBuilder{6}
                          .add(add_order(kMsftLocate, 102, 'B', 100, 4'249'000))
                          .add(executed(kMsftLocate, 101, 50))
                          .add(cancel(kMsftLocate, 100, 100))
                          .add(replace(kMsftLocate, 101, 103, 80, 4'250'500))
                          .add(remove_order(kMsftLocate, 100))
                          .add(executed(kMsftLocate, 999, 10))
                          .build(),
                      Source::Live, 2222);

    ASSERT_EQ(sink_.updates.size(), 6U);
    const feed::MarketUpdate& trade = sink_.updates[2];
    EXPECT_EQ(trade.kind, Kind::Trade);
    EXPECT_EQ(trade.sequence, 7U);
    EXPECT_EQ(trade.last_ticks, 4'251'000U);
    EXPECT_EQ(trade.last_qty, 50U);
    EXPECT_EQ(trade.ask_qty, 150U);

    EXPECT_EQ(sink_.updates[3].bid_qty, 200U);
    EXPECT_EQ(sink_.updates[4].ask_ticks, 4'250'500U);
    EXPECT_EQ(sink_.updates[4].ask_qty, 80U);
    const feed::MarketUpdate& last = sink_.updates[5];
    EXPECT_EQ(last.sequence, 10U);
    EXPECT_EQ(last.bid_ticks, 4'249'000U);
    EXPECT_EQ(last.bid_qty, 100U);

    EXPECT_EQ(handler.builder_stats().unknown_orders, 1U);
    EXPECT_EQ(handler.builder_stats().messages, 11U);
    EXPECT_EQ(handler.expected_sequence(), 12U);
    EXPECT_EQ(books_.open_orders(), 2U);
}

TEST_F(FeedTest, SecondLineDuplicatesAreIgnored) {
    feed::FeedHandler<VectorSink> handler{books_, symbols_, sink_};
    const Packet first = opening_packet();
    handler.on_packet(first, Source::Live, 1);
    handler.on_packet(first, Source::Live, 2);
    EXPECT_EQ(handler.stats().duplicates, 1U);
    EXPECT_EQ(sink_.updates.size(), 2U);
    EXPECT_EQ(books_.open_orders(), 2U);

    const Packet overlap = PacketBuilder{5}
                               .add(add_order(kAaplLocate, 900, 'B', 50, 2'000'000))
                               .add(add_order(kMsftLocate, 104, 'B', 10, 4'250'000))
                               .build();
    handler.on_packet(overlap, Source::Live, 3);
    ASSERT_EQ(sink_.updates.size(), 3U);
    EXPECT_EQ(sink_.updates[2].bid_qty, 310U);
    EXPECT_EQ(handler.expected_sequence(), 7U);

    handler.on_packet(heartbeat(7), Source::Live, 4);
    EXPECT_EQ(handler.stats().heartbeats, 1U);
    EXPECT_EQ(handler.stats().gaps, 0U);
}

TEST_F(FeedTest, TradingHaltIsFlaggedOnUpdates) {
    feed::FeedHandler<VectorSink> handler{books_, symbols_, sink_};
    handler.on_packet(opening_packet(), Source::Live, 1);
    handler.on_packet(PacketBuilder{6}
                          .add(trading_action(kMsftLocate, 'H'))
                          .add(add_order(kMsftLocate, 105, 'B', 10, 4'250'100))
                          .add(trading_action(kMsftLocate, 'T'))
                          .build(),
                      Source::Live, 2);

    ASSERT_EQ(sink_.updates.size(), 5U);
    EXPECT_EQ(sink_.updates[2].kind, Kind::TradingStatus);
    EXPECT_NE(sink_.updates[2].flags & feed::kFlagHalted, 0);
    EXPECT_FALSE(sink_.updates[3].tradable());
    EXPECT_EQ(sink_.updates[4].kind, Kind::TradingStatus);
    EXPECT_TRUE(sink_.updates[4].tradable());
}

TEST_F(FeedTest, GapWithoutRecoveryIsSkippedAndFlaggedLossy) {
    feed::FeedHandler<VectorSink> handler{books_, symbols_, sink_};
    handler.on_packet(opening_packet(), Source::Live, 1);
    handler.on_packet(PacketBuilder{9}.add(add_order(kMsftLocate, 106, 'B', 5, 4'250'200)).build(),
                      Source::Live, 2);

    EXPECT_EQ(handler.stats().gaps, 1U);
    EXPECT_EQ(handler.stats().lost_messages, 3U);
    EXPECT_FALSE(handler.recovering());
    ASSERT_EQ(sink_.updates.size(), 4U);
    EXPECT_EQ(sink_.updates[2].kind, Kind::FeedStatus);
    EXPECT_EQ(sink_.updates[2].flags, feed::kFlagLossy);
    EXPECT_EQ(sink_.updates[3].bid_ticks, 4'250'200U);
    EXPECT_FALSE(sink_.updates[3].tradable());
    EXPECT_EQ(handler.expected_sequence(), 10U);
}

TEST_F(FeedTest, GapIsRecoveredFromRetransmission) {
    feed::FeedHandler<VectorSink> handler{books_, symbols_, sink_};
    handler.configure(1, true);
    const Packet second =
        PacketBuilder{6}.add(add_order(kMsftLocate, 107, 'B', 5, 4'250'300)).build();
    const Packet third =
        PacketBuilder{7}.add(add_order(kMsftLocate, 108, 'S', 5, 4'250'900)).build();

    handler.on_packet(opening_packet(), Source::Live, 1);
    handler.on_packet(third, Source::Live, 2);
    EXPECT_TRUE(handler.recovering());
    EXPECT_EQ(handler.stats().gaps, 1U);
    EXPECT_EQ(handler.expected_sequence(), 6U);
    ASSERT_EQ(sink_.updates.size(), 3U);
    EXPECT_EQ(sink_.updates[2].kind, Kind::FeedStatus);
    EXPECT_EQ(sink_.updates[2].flags, feed::kFlagStale);

    handler.on_packet(third, Source::Recovery, 3);
    EXPECT_EQ(handler.expected_sequence(), 6U);

    handler.on_packet(second, Source::Recovery, 4);
    EXPECT_TRUE(handler.recovering());
    ASSERT_EQ(sink_.updates.size(), 4U);
    EXPECT_NE(sink_.updates[3].flags & feed::kFlagStale, 0);

    handler.on_packet(third, Source::Recovery, 5);
    EXPECT_FALSE(handler.recovering());
    ASSERT_EQ(sink_.updates.size(), 6U);
    EXPECT_EQ(sink_.updates[5].kind, Kind::FeedStatus);
    EXPECT_EQ(sink_.updates[5].flags, 0);

    const book::Top top = books_.book(SymbolId{0})->top();
    EXPECT_EQ(top.bid.ticks, 4'250'300U);
    EXPECT_EQ(top.ask.ticks, 4'250'900U);
    EXPECT_EQ(handler.stats().lost_messages, 0U);
    EXPECT_EQ(handler.stats().gaps, 1U);
}

TEST_F(FeedTest, FullSinkDropsAreCounted) {
    feed::FeedHandler<VectorSink> handler{books_, symbols_, sink_};
    sink_.full = true;
    handler.on_packet(opening_packet(), Source::Live, 1);
    EXPECT_TRUE(sink_.updates.empty());
    EXPECT_EQ(handler.builder_stats().dropped, 2U);
    EXPECT_EQ(books_.open_orders(), 2U);
}

TEST_F(FeedTest, MalformedPacketsDoNotAdvancePastGoodMessages) {
    feed::FeedHandler<VectorSink> handler{books_, symbols_, sink_};
    const Packet whole = opening_packet();
    const Packet cut{whole.begin(), whole.end() - 10};
    handler.on_packet(cut, Source::Live, 1);
    EXPECT_EQ(handler.stats().malformed, 1U);
    EXPECT_EQ(handler.expected_sequence(), 5U);

    handler.on_packet(std::span<const std::byte>{whole.data(), 8}, Source::Live, 2);
    EXPECT_EQ(handler.stats().malformed, 2U);

    handler.on_packet(whole, Source::Live, 3);
    EXPECT_EQ(handler.expected_sequence(), 6U);
    EXPECT_EQ(books_.open_orders(), 2U);
}

TEST_F(FeedTest, PublishesIntoSharedMemoryRing) {
    using Ring = ipc::SpscRing<feed::MarketUpdate>;
    const std::size_t bytes = Ring::bytes_for(64);
    void* mem = std::aligned_alloc(kCacheLine, bytes);
    ASSERT_NE(mem, nullptr);
    const auto made = Ring::format(mem, bytes, 64);
    ASSERT_TRUE(made.status.ok());
    ipc::SpscProducer<feed::MarketUpdate> tx{*made.ring};
    ipc::SpscConsumer<feed::MarketUpdate> rx{*made.ring};

    feed::FeedHandler<ipc::SpscProducer<feed::MarketUpdate>> handler{books_, symbols_, tx};
    handler.on_packet(opening_packet(), Source::Live, 77);

    const feed::MarketUpdate* first = rx.peek();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->bid_ticks, 4'250'000U);
    EXPECT_EQ(first->rx_tsc, 77U);
    rx.consume();
    const feed::MarketUpdate* second = rx.peek();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->ask_ticks, 4'251'000U);
    rx.consume();
    EXPECT_EQ(rx.peek(), nullptr);
    std::free(mem);
}

TEST_F(FeedTest, RecoversOverLoopbackSockets) {
    const int server = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_GE(server, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(server, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)), 0);
    socklen_t len = sizeof(server_addr);
    ASSERT_EQ(::getsockname(server, reinterpret_cast<sockaddr*>(&server_addr), &len), 0);

    feed::FeedHandler<VectorSink> handler{books_, symbols_, sink_};
    feed::FeedConfig config{};
    config.line_a.listen = net::make_endpoint("127.0.0.1", 0).endpoint;
    config.recovery = net::Endpoint::from_sockaddr(server_addr);
    config.has_recovery = true;
    config.request_interval_ticks = 1;
    ASSERT_TRUE(handler.open(config).ok());

    net::UdpSender line;
    ASSERT_TRUE(line.open({.destination = handler.line_a_endpoint().endpoint}).ok());

    const Packet first = opening_packet();
    const Packet second =
        PacketBuilder{6}.add(add_order(kMsftLocate, 107, 'B', 5, 4'250'300)).build();
    const Packet third =
        PacketBuilder{7}.add(add_order(kMsftLocate, 108, 'S', 5, 4'250'900)).build();
    ASSERT_TRUE(line.send(first).ok());
    ASSERT_TRUE(line.send(third).ok());

    wire::mold::Request request{};
    wire::mold::Request first_request{};
    sockaddr_in client{};
    bool served = false;
    const bool caught_up = ipc::spin_until(
        [&] {
            static_cast<void>(handler.poll());
            socklen_t client_len = sizeof(client);
            const ssize_t got = ::recvfrom(server, &request, sizeof(request), 0,
                                           reinterpret_cast<sockaddr*>(&client), &client_len);
            if (got == static_cast<ssize_t>(sizeof(request))) {
                if (!served) {
                    first_request = request;
                }
                served = true;
                for (const Packet* reply : {&second, &third}) {
                    static_cast<void>(::sendto(server, reply->data(), reply->size(), 0,
                                               reinterpret_cast<sockaddr*>(&client), client_len));
                }
            }
            if (handler.expected_sequence() == 8 && !handler.recovering()) {
                return true;
            }
            static_cast<void>(::usleep(20));
            return false;
        },
        200'000);
    static_cast<void>(::close(server));

    ASSERT_TRUE(caught_up);
    EXPECT_TRUE(served);
    EXPECT_EQ(first_request.session.view(), "HOTPATH01");
    EXPECT_EQ(first_request.sequence.get(), 6U);
    EXPECT_EQ(first_request.requested_count.get(), 2U);
    EXPECT_GE(handler.stats().requests_sent, 1U);
    EXPECT_EQ(handler.stats().gaps, 1U);

    const book::Top top = books_.book(SymbolId{0})->top();
    EXPECT_EQ(top.bid.ticks, 4'250'300U);
    EXPECT_EQ(top.ask.ticks, 4'250'900U);
}

} // namespace
