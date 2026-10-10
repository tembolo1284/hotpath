#include <cstdio>

#include "common/app.hpp"
#include "hotpath/book/book_set.hpp"
#include "hotpath/feed/feed_handler.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/feed/symbol_table.hpp"
#include "hotpath/ipc/channel.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"

using namespace hotpath;

namespace {

constexpr const char* kApp = "feed_handler";

void usage() {
    std::puts(
        "feed_handler: MoldUDP64/ITCH in, book building, top-of-book out on shared memory\n"
        "  --feed=ip:port         line A listen address (default 127.0.0.1:31001)\n"
        "  --feed-b=ip:port       optional line B\n"
        "  --iface=ip:0           interface address for multicast joins\n"
        "  --retransmit=ip:port   retransmission server (default 127.0.0.1:31002)\n"
        "  --no-recovery          skip gaps instead of re-requesting them\n"
        "  --start-seq=N          first sequence wanted (default 1, 0 = join live)\n"
        "  --symbols=A,B,C        watch list, same order in every process\n"
        "  --max-orders=N         order table capacity, power of two (default 1048576)\n"
        "  --levels=N             price levels kept per side (default 1024)\n"
        "  --cpu=N --mlock --no-hugepages --idle-us=N --seconds=N --stats-sec=N");
}

template <typename H>
void print_stats(const H& handler, const book::BookSet& books) {
    const feed::FeedStats& s = handler.stats();
    const feed::BuilderStats& b = handler.builder_stats();
    std::printf("%s: packets=%llu msgs=%llu published=%llu dropped=%llu gaps=%llu lost=%llu "
                "requests=%llu dups=%llu unknown_orders=%llu open_orders=%u next_seq=%llu%s\n",
                kApp, static_cast<unsigned long long>(s.packets),
                static_cast<unsigned long long>(b.messages),
                static_cast<unsigned long long>(b.published),
                static_cast<unsigned long long>(b.dropped),
                static_cast<unsigned long long>(s.gaps),
                static_cast<unsigned long long>(s.lost_messages),
                static_cast<unsigned long long>(s.requests_sent),
                static_cast<unsigned long long>(s.duplicates),
                static_cast<unsigned long long>(b.unknown_orders), books.open_orders(),
                static_cast<unsigned long long>(handler.expected_sequence()),
                handler.recovering() ? " RECOVERING" : "");
    static_cast<void>(std::fflush(stdout));
}

} // namespace

int main(int argc, char** argv) {
    const app::Args args{argc, argv, kApp};
    if (args.flag("help")) {
        usage();
        return 0;
    }
    args.expect({"feed", "feed-b", "iface", "retransmit", "no-recovery", "start-seq", "symbols",
                 "max-orders", "levels", "cpu", "mlock", "no-hugepages", "idle-us", "seconds",
                 "stats-sec"});
    app::Runtime rt = app::start(args);
    const app::Symbols symbols = app::parse_symbols(args);

    const book::BookConfig book_config{
        .max_symbols = symbols.count,
        .max_orders = static_cast<std::uint32_t>(args.number("max-orders", 1U << 20)),
        .levels_per_side = static_cast<std::uint32_t>(args.number("levels", 1024))};

    mem::Region region;
    app::map_region(args, region, book::BookSet::bytes_for(book_config) + (std::size_t{4} << 20));
    mem::Arena arena{region};

    book::BookSet books;
    Status st = books.init(arena, book_config);
    if (!st.ok()) {
        app::die(kApp, "book set", st);
    }
    feed::SymbolTable table;
    st = table.init(arena, symbols.count);
    for (std::uint16_t i = 0; st.ok() && i < symbols.count; ++i) {
        st = table.watch(symbols.tickers[i].view());
    }
    if (!st.ok()) {
        app::die(kApp, "symbol table", st);
    }
    arena.freeze();

    ipc::Channel<feed::MarketUpdate> market;
    app::create_channel(args, market, app::kMarketChannel, app::kMarketCapacity);
    ipc::SpscProducer<feed::MarketUpdate> market_tx = market.producer();

    static feed::FeedHandler<ipc::SpscProducer<feed::MarketUpdate>> handler{books, table, market_tx};

    feed::FeedConfig config{};
    config.line_a.listen = app::parse_endpoint(args, "feed", app::kDefaultFeed);
    config.line_a.rcvbuf_bytes = 1 << 22;
    config.line_a.interface_be = app::parse_endpoint(args, "iface", "0.0.0.0:0").addr_be;
    if (args.has("feed-b")) {
        config.has_line_b = true;
        config.line_b = config.line_a;
        config.line_b.listen = app::parse_endpoint(args, "feed-b", {});
    }
    config.has_recovery = !args.flag("no-recovery");
    config.recovery = app::parse_endpoint(args, "retransmit", app::kDefaultRetransmit);
    config.start_sequence = args.number("start-seq", 1);
    config.request_interval_ticks = rt.ticks_per_second / 500U;
    st = handler.open(config);
    if (!st.ok()) {
        app::die(kApp, "cannot open feed sockets", st);
    }
    std::fprintf(stderr, "%s: listening, %u symbols, publishing to %s\n", kApp,
                 static_cast<unsigned>(symbols.count), app::kMarketChannel);

    std::uint64_t spins = 0;
    while (app::running()) {
        rt.idle(handler.poll());
        if ((++spins & 0xFFFU) == 0) {
            const std::uint64_t now = monotonic_ns();
            if (rt.expired(now)) {
                break;
            }
            if (rt.stats_due(now)) {
                print_stats(handler, books);
            }
        }
    }

    print_stats(handler, books);
    static_cast<void>(ipc::Channel<feed::MarketUpdate>::unlink(app::kMarketChannel));
    return 0;
}
