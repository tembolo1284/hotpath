#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/itch.hpp"
#include "hotpath/wire/mold.hpp"
#include "hotpath/wire/ouch.hpp"
#include "hotpath/wire/soup.hpp"

using namespace hotpath;
using wire::Parse;

namespace {

template <wire::WireMessage M>
std::span<const std::byte> bytes_of(const M& msg) {
    return std::as_bytes(std::span<const M, 1>{&msg, 1});
}

wire::itch::AddOrder sample_add(std::uint64_t ref) {
    auto add = wire::itch::make<wire::itch::AddOrder>();
    add.hdr.stock_locate.set(42);
    add.hdr.timestamp_ns.set(34'200'000'000'123ULL);
    add.order_ref.set(ref);
    add.side = 'B';
    add.shares.set(300);
    add.stock = wire::Alpha<8>{"MSFT"};
    add.price.set(4'251'500);
    return add;
}

struct BookCounter {
    std::uint64_t adds{0};
    std::uint64_t deletes{0};
    std::uint64_t last_ref{0};
    std::uint32_t last_price{0};

    void on(const wire::itch::AddOrder& m) noexcept {
        adds += 1;
        last_ref = m.order_ref.get();
        last_price = m.price.get();
    }
    void on(const wire::itch::OrderDelete& m) noexcept {
        deletes += 1;
        last_ref = m.order_ref.get();
    }
};

TEST(Fields, BigEndianByteOrderOnTheWire) {
    const wire::U32 value{0x0A0B0C0DU};
    std::uint8_t raw[4];
    std::memcpy(raw, &value, sizeof(raw));
    EXPECT_EQ(raw[0], 0x0A);
    EXPECT_EQ(raw[3], 0x0D);

    const std::uint8_t wire48[6] = {0x00, 0x00, 0x1F, 0x1A, 0xDB, 0x00};
    wire::U48 ts;
    std::memcpy(&ts, wire48, sizeof(ts));
    EXPECT_EQ(ts.get(), 0x1F1ADB00ULL);
}

TEST(Itch, DispatchDecodesAddOrderInPlace) {
    const auto add = sample_add(987'654'321ULL);
    BookCounter book;
    ASSERT_EQ(wire::itch::dispatch(bytes_of(add), book), Parse::Ok);
    EXPECT_EQ(book.adds, 1U);
    EXPECT_EQ(book.last_ref, 987'654'321ULL);
    EXPECT_EQ(book.last_price, 4'251'500U);
    EXPECT_EQ(add.stock.view(), "MSFT");
    EXPECT_EQ(add.hdr.timestamp_ns.get(), 34'200'000'000'123ULL);
}

TEST(Itch, DispatchSkipsUnhandledAndFlagsBadInput) {
    BookCounter book;
    const auto exec = wire::itch::make<wire::itch::OrderExecuted>();
    EXPECT_EQ(wire::itch::dispatch(bytes_of(exec), book), Parse::Ok);
    EXPECT_EQ(book.adds + book.deletes, 0U);

    const auto add = sample_add(1);
    EXPECT_EQ(wire::itch::dispatch(bytes_of(add).first(20), book), Parse::Truncated);

    const std::byte unknown[4] = {std::byte{'~'}, {}, {}, {}};
    EXPECT_EQ(wire::itch::dispatch(unknown, book), Parse::Unknown);
    EXPECT_EQ(wire::itch::dispatch({}, book), Parse::Truncated);
}

TEST(Mold, WriterAndReaderRoundTrip) {
    std::array<std::byte, wire::mold::kMaxPacketBytes> buffer{};
    wire::mold::PacketWriter writer{buffer};
    writer.begin(wire::Alpha<10>{"HOTPATH01"}, 1000);

    ASSERT_TRUE(writer.append(sample_add(1)));
    ASSERT_TRUE(writer.append(sample_add(2)));
    auto del = wire::itch::make<wire::itch::OrderDelete>();
    del.order_ref.set(1);
    ASSERT_TRUE(writer.append(del));
    const std::span<const std::byte> packet = writer.finish();

    const wire::mold::Header* hdr = wire::mold::parse_header(packet);
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->session.view(), "HOTPATH01");
    EXPECT_EQ(hdr->sequence.get(), 1000U);
    EXPECT_EQ(hdr->count.get(), 3U);

    BookCounter book;
    std::uint64_t next_seq = 1000;
    const Parse result = wire::mold::for_each_message(
        packet, [&](std::uint64_t seq, std::span<const std::byte> msg) {
            EXPECT_EQ(seq, next_seq);
            next_seq += 1;
            EXPECT_EQ(wire::itch::dispatch(msg, book), Parse::Ok);
        });
    EXPECT_EQ(result, Parse::Ok);
    EXPECT_EQ(next_seq, 1003U);
    EXPECT_EQ(book.adds, 2U);
    EXPECT_EQ(book.deletes, 1U);

    std::uint64_t seen = 0;
    const auto count = [&](std::uint64_t, std::span<const std::byte>) { seen += 1; };
    EXPECT_EQ(wire::mold::for_each_message(packet.first(packet.size() - 1), count),
              Parse::Truncated);
    EXPECT_EQ(wire::mold::for_each_message(packet.first(10), count), Parse::Truncated);
}

TEST(Mold, WriterRefusesOverflowAndHandlesHeartbeat) {
    std::array<std::byte, 64> small{};
    wire::mold::PacketWriter writer{small};
    writer.begin(wire::Alpha<10>{"S"}, 7);
    EXPECT_TRUE(writer.append(sample_add(1)));
    EXPECT_FALSE(writer.append(sample_add(2)));
    EXPECT_EQ(writer.count(), 1U);

    writer.begin(wire::Alpha<10>{"S"}, 8);
    const auto heartbeat = writer.finish();
    EXPECT_EQ(heartbeat.size(), sizeof(wire::mold::Header));
    std::uint64_t seen = 0;
    EXPECT_EQ(wire::mold::for_each_message(
                  heartbeat, [&](std::uint64_t, std::span<const std::byte>) { seen += 1; }),
              Parse::Ok);
    EXPECT_EQ(seen, 0U);
}

TEST(Soup, FramesPacketsFromAPartialStream) {
    auto enter = wire::ouch::make<wire::ouch::EnterOrder>();
    enter.token = wire::ouch::Token{"T0000000000001"};
    enter.shares.set(100);

    std::array<std::byte, 128> stream{};
    std::size_t used = wire::soup::write_packet(stream, wire::soup::kUnsequencedData,
                                                bytes_of(enter));
    ASSERT_EQ(used, 3U + sizeof(enter));
    used += wire::soup::write_packet(std::span{stream}.subspan(used),
                                     wire::soup::kClientHeartbeat, {});
    ASSERT_EQ(used, 3U + sizeof(enter) + 3U);

    const std::span<const std::byte> all{stream.data(), used};
    EXPECT_EQ(wire::soup::next_packet(all.first(2)).result, Parse::Truncated);
    EXPECT_EQ(wire::soup::next_packet(all.first(30)).result, Parse::Truncated);

    const wire::soup::Packet first = wire::soup::next_packet(all);
    ASSERT_EQ(first.result, Parse::Ok);
    EXPECT_EQ(first.type, wire::soup::kUnsequencedData);
    EXPECT_EQ(first.payload.size(), sizeof(enter));
    EXPECT_EQ(first.consumed, 3U + sizeof(enter));

    const wire::soup::Packet second = wire::soup::next_packet(all.subspan(first.consumed));
    ASSERT_EQ(second.result, Parse::Ok);
    EXPECT_EQ(second.type, wire::soup::kClientHeartbeat);
    EXPECT_TRUE(second.payload.empty());
    EXPECT_EQ(second.consumed, 3U);

    const std::byte zero_len[3] = {};
    EXPECT_EQ(wire::soup::next_packet(zero_len).result, Parse::Malformed);
}

TEST(Soup, LoginMessagesCarryNumericSequence) {
    auto login = wire::soup::make<wire::soup::LoginRequest>(wire::soup::kLoginRequest);
    login.username = wire::Alpha<6>{"paul"};
    login.requested_sequence = wire::format_numeric<20>(1);
    EXPECT_EQ(login.hdr.length.get(), sizeof(login) - 2U);

    const wire::soup::Packet packet = wire::soup::next_packet(bytes_of(login));
    ASSERT_EQ(packet.result, Parse::Ok);
    EXPECT_EQ(packet.type, wire::soup::kLoginRequest);
    EXPECT_EQ(packet.consumed, sizeof(login));

    const wire::Numeric seq = wire::parse_numeric(login.requested_sequence.raw());
    EXPECT_TRUE(seq.ok);
    EXPECT_EQ(seq.value, 1U);
}

struct FillCounter {
    std::uint32_t filled{0};
    std::uint32_t accepted{0};
    void on(const wire::ouch::Executed& m) noexcept { filled += m.executed_shares.get(); }
    void on(const wire::ouch::Accepted&) noexcept { accepted += 1; }
};

struct OrderCounter {
    std::uint32_t entered{0};
    std::uint32_t cancelled{0};
    void on(const wire::ouch::EnterOrder& m) noexcept { entered += m.shares.get(); }
    void on(const wire::ouch::CancelOrder&) noexcept { cancelled += 1; }
};

TEST(Ouch, DirectionsDispatchSeparately) {
    auto enter = wire::ouch::make<wire::ouch::EnterOrder>();
    enter.shares.set(500);
    enter.price.set(1'234'500);
    enter.time_in_force.set(wire::ouch::kTifImmediateOrCancel);
    auto cancel = wire::ouch::make<wire::ouch::CancelOrder>();

    OrderCounter venue;
    EXPECT_EQ(wire::ouch::dispatch_inbound(bytes_of(enter), venue), Parse::Ok);
    EXPECT_EQ(wire::ouch::dispatch_inbound(bytes_of(cancel), venue), Parse::Ok);
    EXPECT_EQ(venue.entered, 500U);
    EXPECT_EQ(venue.cancelled, 1U);

    auto exec = wire::ouch::make<wire::ouch::Executed>();
    exec.executed_shares.set(200);
    auto replaced = wire::ouch::make<wire::ouch::Replaced>();
    auto accepted = wire::ouch::make<wire::ouch::Accepted>();

    FillCounter client;
    EXPECT_EQ(wire::ouch::dispatch_outbound(bytes_of(exec), client), Parse::Ok);
    EXPECT_EQ(wire::ouch::dispatch_outbound(bytes_of(accepted), client), Parse::Ok);
    EXPECT_EQ(wire::ouch::dispatch_outbound(bytes_of(replaced), client), Parse::Ok);
    EXPECT_EQ(client.filled, 200U);
    EXPECT_EQ(client.accepted, 1U);

    EXPECT_EQ(wire::ouch::dispatch_outbound(bytes_of(exec).first(8), client), Parse::Truncated);
    EXPECT_EQ(wire::ouch::dispatch_inbound(bytes_of(exec), venue), Parse::Unknown);
}

} // namespace
