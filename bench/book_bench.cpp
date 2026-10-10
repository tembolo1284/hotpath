#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "hotpath/book/book_set.hpp"
#include "hotpath/core/clock.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/feed/feed_handler.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/feed/symbol_table.hpp"
#include "support/bench.hpp"
#include "support/flow.hpp"

using namespace hotpath;

namespace {

struct CountingSink {
    feed::MarketUpdate scratch{};
    std::uint64_t published{0};

    feed::MarketUpdate* claim() noexcept { return &scratch; }
    void publish() noexcept { published += 1; }
};

void churn(bench::Session& session, const char* name, std::uint32_t resting) {
    if (!session.selected(name)) {
        return;
    }
    bench::Workspace workspace{std::size_t{256} << 20};
    book::BookSet books;
    bench::require(books.init(workspace.arena(), {.max_symbols = bench::kFlowSymbols,
                                                  .max_orders = std::bit_ceil(resting * 2U),
                                                  .levels_per_side = 256}),
                   "book init");

    constexpr std::uint64_t kSpread = 0x9E37'79B9'7F4A'7C15ULL;
    const auto symbol_of = [](std::uint64_t sequence) {
        return SymbolId{static_cast<std::uint16_t>((sequence * kSpread >> 40) % bench::kFlowSymbols)};
    };
    const auto add = [&](std::uint64_t sequence) {
        const std::uint64_t key = sequence * kSpread;
        const Side side = ((key >> 33) & 1U) != 0 ? Side::Buy : Side::Sell;
        const std::int64_t offset = 1 + static_cast<std::int64_t>((key >> 44) % 40U);
        const Price price{side == Side::Buy ? 1'000'000 - offset * 100 : 1'000'000 + offset * 100};
        return books.add(symbol_of(sequence), OrderId{sequence}, side, Qty{100}, price);
    };
    for (std::uint64_t sequence = 1; sequence <= resting; ++sequence) {
        if (!add(sequence).ok()) {
            bench::die("book preload failed");
        }
    }

    std::uint64_t next = resting + 1;
    session.run(name, session.ops(2'000'000), [] {}, [&](std::uint64_t) {
        const std::uint64_t oldest = next - resting;
        bench::keep(books.remove(symbol_of(oldest), OrderId{oldest}).top_changed);
        bench::keep(add(next).top_changed);
        next += 1;
    });
    if (books.open_orders() != resting) {
        bench::die("book churn lost orders");
    }
}

struct FeedRig {
    bench::Workspace workspace{std::size_t{64} << 20};
    book::BookSet books{};
    feed::SymbolTable symbols{};
    CountingSink sink{};
    std::unique_ptr<feed::FeedHandler<CountingSink>> handler{};

    FeedRig() {
        bench::require(books.init(workspace.arena(), {.max_symbols = bench::kFlowSymbols,
                                                      .max_orders = 1U << 14,
                                                      .levels_per_side = 256}),
                       "book init");
        bench::require(symbols.init(workspace.arena(), bench::kFlowSymbols), "symbol table init");
        for (const auto& ticker : bench::kFlowTickers) {
            bench::require(symbols.watch(ticker.view()), "watch");
        }
        handler = std::make_unique<feed::FeedHandler<CountingSink>>(books, symbols, sink);
        handler->configure(1, false);
    }
};

void feed_replay(bench::Session& session) {
    const char* name = "feed packet";
    if (!session.selected(name)) {
        return;
    }
    bench::Flow flow;
    bench::generate_flow(flow, session.ops(1'000'000), 0x9E37'79B9'7F4A'7C15ULL);
    const bench::PacketLog& packets = flow.packets;

    std::unique_ptr<FeedRig> rig;
    session.run(name, packets.size(), [&] { rig = std::make_unique<FeedRig>(); },
                [&](std::uint64_t i) {
                    rig->handler->on_packet(packets[i], feed::Source::Live, tsc_now());
                });

    const feed::FeedStats& stats = rig->handler->stats();
    const feed::BuilderStats& built = rig->handler->builder_stats();
    std::printf("  packets=%zu messages=%llu (%llu.%02llu per packet) published=%llu gaps=%llu "
                "unknown_orders=%llu\n",
                packets.size(), static_cast<unsigned long long>(flow.messages),
                static_cast<unsigned long long>(flow.messages / packets.size()),
                static_cast<unsigned long long>(flow.messages * 100U / packets.size() % 100U),
                static_cast<unsigned long long>(rig->sink.published),
                static_cast<unsigned long long>(stats.gaps),
                static_cast<unsigned long long>(built.unknown_orders));
    if (stats.gaps != 0 || stats.malformed != 0 || built.unknown_orders != 0) {
        bench::die("feed replay was not clean");
    }
}

} // namespace

int main(int argc, char** argv) {
    bench::Session session{argc, argv, "book_bench: order book and feed handler"};
    session.note("churn: remove the oldest order and add a new one, book size held constant");
    churn(session, "churn 1k orders", 1'000);
    churn(session, "churn 100k orders", 100'000);
    churn(session, "churn 1M orders", 1'000'000);
    session.note("feed packet: MoldUDP64 packet in, books updated, top-of-book published");
    feed_replay(session);
    return 0;
}
