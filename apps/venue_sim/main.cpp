#include <cstdio>

#include "common/app.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"
#include "hotpath/sim/exchange.hpp"

using namespace hotpath;

namespace {

constexpr const char* kApp = "venue_sim";

void usage() {
    std::puts(
        "venue_sim: matching engine publishing ITCH over MoldUDP64 and accepting OUCH over TCP\n"
        "  --feed=ip:port         where to send market data (default 127.0.0.1:31001)\n"
        "  --iface=ip:0           interface address for multicast sends\n"
        "  --retransmit=ip:port   retransmission listen address (default 127.0.0.1:31002)\n"
        "  --order-entry=ip:port  order entry listen address (default 127.0.0.1:31003)\n"
        "  --symbols=A,B,C        instruments to list\n"
        "  --rate=N               background order-flow actions per second (default 2000)\n"
        "  --seed=N               order-flow seed (default 1)\n"
        "  --cpu=N --mlock --no-hugepages --idle-us=N --seconds=N --stats-sec=N");
}

void print_stats(sim::Exchange& exchange) {
    const sim::VenueStats& v = exchange.venue().stats();
    const sim::ExchangeStats& e = exchange.stats();
    std::printf("%s: itch_msgs=%llu packets=%llu trades=%llu client_orders=%llu client_fills=%llu "
                "rejected=%llu resting=%u connections=%llu retransmits=%llu client=%s\n",
                kApp, static_cast<unsigned long long>(exchange.publisher().next_sequence() - 1),
                static_cast<unsigned long long>(exchange.publisher().packets_sent()),
                static_cast<unsigned long long>(v.trades),
                static_cast<unsigned long long>(v.orders_entered),
                static_cast<unsigned long long>(v.client_fills),
                static_cast<unsigned long long>(v.orders_rejected),
                exchange.engine().resting_orders(),
                static_cast<unsigned long long>(e.connections),
                static_cast<unsigned long long>(e.retransmit_replies),
                exchange.session().logged_in() ? "logged-in" : "none");
    static_cast<void>(std::fflush(stdout));
}

} // namespace

int main(int argc, char** argv) {
    const app::Args args{argc, argv, kApp};
    if (args.flag("help")) {
        usage();
        return 0;
    }
    args.expect({"feed", "iface", "retransmit", "order-entry", "symbols", "rate", "seed", "cpu",
                 "mlock", "no-hugepages", "idle-us", "seconds", "stats-sec"});
    app::Runtime rt = app::start(args);
    const app::Symbols symbols = app::parse_symbols(args);
    const std::uint64_t rate = args.number("rate", 2000);

    sim::ExchangeConfig config{};
    config.feed_destination = app::parse_endpoint(args, "feed", app::kDefaultFeed);
    config.feed_interface_be = app::parse_endpoint(args, "iface", "0.0.0.0:0").addr_be;
    config.retransmit = app::parse_endpoint(args, "retransmit", app::kDefaultRetransmit);
    config.order_entry = app::parse_endpoint(args, "order-entry", app::kDefaultOrderEntry);
    config.engine = {.symbols = symbols.count, .max_orders = 1U << 16, .levels_per_side = 256};
    config.max_tokens = 1U << 20;
    config.soup.log_capacity = 1U << 18;
    config.publisher.store_capacity = 1U << 21;
    config.liquidity.seed = args.number("seed", 1);
    config.liquidity_steps_per_poll = 0;

    mem::Region region;
    app::map_region(args, region, std::size_t{256} << 20);
    mem::Arena arena{region};

    static sim::Exchange exchange;
    const Status st = exchange.init(arena, config, symbols.span(), monotonic_ns());
    if (!st.ok()) {
        app::die(kApp, "cannot start the exchange", st);
    }
    arena.freeze();
    std::fprintf(stderr, "%s: open, %u symbols, %llu actions/s\n", kApp,
                 static_cast<unsigned>(symbols.count), static_cast<unsigned long long>(rate));

    constexpr std::uint64_t kMaxBurst = 64;
    const std::uint64_t started = monotonic_ns();
    std::uint64_t done = 0;
    while (app::running()) {
        const std::uint64_t now = monotonic_ns();
        if (rt.expired(now)) {
            break;
        }
        const std::uint64_t target = (now - started) / 1000U * rate / 1'000'000U;
        const std::uint64_t due = target > done ? target - done : 0;
        const std::uint64_t burst = due < kMaxBurst ? due : kMaxBurst;
        exchange.set_liquidity_steps(static_cast<std::uint32_t>(burst));
        exchange.poll(now);
        done += burst;
        rt.idle(static_cast<std::uint32_t>(burst));
        if (rt.stats_due(now)) {
            print_stats(exchange);
        }
    }

    print_stats(exchange);
    return 0;
}
