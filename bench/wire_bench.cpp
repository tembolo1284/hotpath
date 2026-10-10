#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hotpath/gateway/token.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/itch.hpp"
#include "hotpath/wire/mold.hpp"
#include "hotpath/wire/ouch.hpp"
#include "support/bench.hpp"

using namespace hotpath;

namespace {

struct Totals {
    std::uint64_t refs{0};
    std::uint64_t shares{0};
    std::uint64_t notional{0};

    void on(const wire::itch::AddOrder& m) noexcept {
        refs += m.order_ref.get();
        shares += m.shares.get();
        notional += std::uint64_t{m.shares.get()} * m.price.get();
    }
    void on(const wire::itch::OrderExecuted& m) noexcept {
        refs += m.order_ref.get();
        shares += m.executed_shares.get();
    }
    void on(const wire::itch::OrderCancel& m) noexcept {
        refs += m.order_ref.get();
        shares += m.cancelled_shares.get();
    }
    void on(const wire::itch::OrderDelete& m) noexcept { refs += m.order_ref.get(); }
};

template <wire::WireMessage M>
std::span<const std::byte> bytes_of(const M& msg) {
    return std::as_bytes(std::span<const M, 1>{&msg, 1});
}

} // namespace

int main(int argc, char** argv) {
    bench::Session session{argc, argv, "wire_bench: ITCH decode, MoldUDP64 framing, OUCH encode"};
    bench::Xorshift rng{0x5EED'0001ULL};

    auto add = wire::itch::make<wire::itch::AddOrder>();
    add.hdr.stock_locate.set(7);
    add.hdr.timestamp_ns.set(34'200'000'123'456ULL);
    add.side = 'B';
    add.shares.set(300);
    add.stock = wire::Alpha<8>{"MSFT"};
    add.price.set(4'251'000);

    Totals totals;
    session.run("itch add decode", session.ops(5'000'000), [] {}, [&](std::uint64_t i) {
        add.order_ref.set(i);
        bench::keep(wire::itch::dispatch(bytes_of(add), totals));
        bench::keep(totals.notional);
    });

    constexpr unsigned kPacketMessages = 24;
    std::array<std::byte, wire::mold::kMaxPacketBytes> buffer{};
    wire::mold::PacketWriter writer{buffer};
    writer.begin(wire::Alpha<10>{"BENCH00001"}, 1);
    for (unsigned k = 0; k < kPacketMessages; ++k) {
        const std::uint32_t pick = rng.below(100);
        bool ok = false;
        if (pick < 45) {
            add.order_ref.set(rng.next());
            ok = writer.append(add);
        } else if (pick < 60) {
            auto m = wire::itch::make<wire::itch::OrderExecuted>();
            m.order_ref.set(rng.next());
            m.executed_shares.set(100);
            ok = writer.append(m);
        } else if (pick < 75) {
            auto m = wire::itch::make<wire::itch::OrderCancel>();
            m.order_ref.set(rng.next());
            m.cancelled_shares.set(100);
            ok = writer.append(m);
        } else {
            auto m = wire::itch::make<wire::itch::OrderDelete>();
            m.order_ref.set(rng.next());
            ok = writer.append(m);
        }
        if (!ok) {
            bench::die("packet overflow");
        }
    }
    const std::span<const std::byte> packet = writer.finish();

    session.run("mold packet x24", session.ops(1'000'000), [] {}, [&](std::uint64_t) {
        const wire::mold::Header* hdr = wire::mold::parse_header(packet);
        bench::keep(hdr->sequence.get());
        const wire::Parse parsed = wire::mold::for_each_message(
            packet, [&](std::uint64_t, std::span<const std::byte> msg) {
                static_cast<void>(wire::itch::dispatch(msg, totals));
            });
        bench::keep(parsed);
        bench::keep(totals.refs);
    });

    session.run("ouch enter encode", session.ops(5'000'000), [] {}, [&](std::uint64_t i) {
        auto msg = wire::ouch::make<wire::ouch::EnterOrder>();
        msg.token = gateway::make_token('H', i);
        msg.side = 'B';
        msg.shares.set(100);
        msg.stock = add.stock;
        msg.price.set(4'251'000);
        msg.time_in_force.set(wire::ouch::kTifImmediateOrCancel);
        msg.display = 'Y';
        msg.capacity = 'P';
        msg.intermarket_sweep = 'N';
        msg.minimum_quantity.set(0);
        msg.cross_type = 'N';
        msg.customer_type = ' ';
        bench::keep(msg);
    });

    wire::ouch::Token token = gateway::make_token('H', 1);
    session.run("token parse", session.ops(5'000'000),
                [] {}, [&](std::uint64_t i) {
                    token = gateway::make_token('H', i * 0x9E37'79B9ULL);
                    bench::keep(gateway::parse_token(token, 'H').id);
                });
    return 0;
}
