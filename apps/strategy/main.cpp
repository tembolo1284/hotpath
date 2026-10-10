#include <time.h>

#include <cstdio>

#include "common/app.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/ipc/channel.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/region.hpp"
#include "hotpath/strategy/imbalance_taker.hpp"
#include "hotpath/strategy/runner.hpp"

using namespace hotpath;

namespace {

constexpr const char* kApp = "strategy";

void usage() {
    std::puts(
        "strategy: reads top-of-book from shared memory, sends order requests to the gateway\n"
        "  --symbols=A,B,C        watch list, same order in every process\n"
        "  --threshold=N          percent of top-of-book size on one side that triggers (default 80)\n"
        "  --clip=N               shares per order (default 100)\n"
        "  --max-position=N       absolute position cap per symbol (default 500)\n"
        "  --cpu=N --mlock --no-hugepages --idle-us=N --seconds=N --stats-sec=N");
}

void print_stats(const strategy::ImbalanceTaker& taker, const app::Symbols& symbols) {
    const strategy::TakerStats& s = taker.stats();
    std::printf("%s: updates=%llu signals=%llu orders=%llu fills=%llu rejects=%llu refused=%llu "
                "open=%u session=%s positions:",
                kApp, static_cast<unsigned long long>(s.updates),
                static_cast<unsigned long long>(s.signals),
                static_cast<unsigned long long>(s.orders),
                static_cast<unsigned long long>(s.fills),
                static_cast<unsigned long long>(s.rejects),
                static_cast<unsigned long long>(s.refused), taker.open_orders(),
                taker.session_up() ? "up" : "down");
    for (std::uint16_t i = 0; i < symbols.count; ++i) {
        const auto ticker = symbols.tickers[i].view();
        std::printf(" %.*s=%lld", static_cast<int>(ticker.size()), ticker.data(),
                    static_cast<long long>(taker.position(SymbolId{i})));
    }
    std::printf("\n");
    static_cast<void>(std::fflush(stdout));
}

} // namespace

int main(int argc, char** argv) {
    const app::Args args{argc, argv, kApp};
    if (args.flag("help")) {
        usage();
        return 0;
    }
    args.expect({"symbols", "threshold", "clip", "max-position", "cpu", "mlock", "no-hugepages",
                 "idle-us", "seconds", "stats-sec"});
    app::Runtime rt = app::start(args);
    const app::Symbols symbols = app::parse_symbols(args);

    mem::Region region;
    app::map_region(args, region, std::size_t{4} << 20);
    mem::Arena arena{region};

    const strategy::TakerConfig config{
        .threshold_pct = static_cast<std::uint32_t>(args.number("threshold", 80)),
        .clip_qty = static_cast<std::uint32_t>(args.number("clip", 100)),
        .max_position = static_cast<std::int64_t>(args.number("max-position", 500)),
        .max_open_orders = 1024};
    strategy::ImbalanceTaker taker;
    const Status st = taker.init(arena, symbols.count, config);
    if (!st.ok()) {
        app::die(kApp, "strategy configuration", st);
    }
    arena.freeze();

    ipc::Channel<gateway::OrderRequest> requests;
    app::create_channel(args, requests, app::kRequestChannel, app::kRequestCapacity);
    ipc::Channel<feed::MarketUpdate> market;
    app::open_channel(args, market, app::kMarketChannel);
    ipc::Channel<gateway::ExecutionReport> reports;
    app::open_channel(args, reports, app::kReportChannel);

    ipc::SpscProducer<gateway::OrderRequest> request_tx = requests.producer();
    ipc::SpscConsumer<feed::MarketUpdate> market_rx = market.consumer();
    ipc::SpscConsumer<gateway::ExecutionReport> report_rx = reports.consumer();

    timespec wall{};
    static_cast<void>(::clock_gettime(CLOCK_REALTIME, &wall));
    const std::uint64_t first_id = (static_cast<std::uint64_t>(wall.tv_sec) << 24) + 1U;

    strategy::StrategyRunner<strategy::ImbalanceTaker, ipc::SpscConsumer<feed::MarketUpdate>,
                             ipc::SpscConsumer<gateway::ExecutionReport>,
                             ipc::SpscProducer<gateway::OrderRequest>>
        runner{taker, market_rx, report_rx, request_tx, first_id};
    std::fprintf(stderr, "%s: running, %u symbols\n", kApp, static_cast<unsigned>(symbols.count));

    std::uint64_t spins = 0;
    while (app::running()) {
        rt.idle(runner.poll());
        if ((++spins & 0xFFFU) == 0) {
            const std::uint64_t now = monotonic_ns();
            if (rt.expired(now)) {
                break;
            }
            if (rt.stats_due(now)) {
                print_stats(taker, symbols);
            }
        }
    }

    print_stats(taker, symbols);
    static_cast<void>(ipc::Channel<gateway::OrderRequest>::unlink(app::kRequestChannel));
    return 0;
}
