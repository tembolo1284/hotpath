#include <unistd.h>

#include <cstdio>

#include "common/app.hpp"
#include "hotpath/gateway/order_gateway.hpp"
#include "hotpath/gateway/order_messages.hpp"
#include "hotpath/ipc/channel.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/net/tcp.hpp"
#include "hotpath/perf/report.hpp"

using namespace hotpath;

namespace {

constexpr const char* kApp = "order_gateway";

using Gateway = gateway::OrderGateway<net::TcpStream, ipc::SpscConsumer<gateway::OrderRequest>,
                                      ipc::SpscProducer<gateway::ExecutionReport>>;

void usage() {
    std::puts(
        "order_gateway: order requests in from shared memory, OUCH over SoupBinTCP out\n"
        "  --venue=ip:port        order entry address (default 127.0.0.1:31003)\n"
        "  --user=NAME            SoupBinTCP username, up to 6 characters (default HOTPTH)\n"
        "  --password=TEXT        SoupBinTCP password, up to 10 characters\n"
        "  --firm=MPID            firm identifier, up to 4 characters\n"
        "  --symbols=A,B,C        watch list, same order in every process\n"
        "  --max-qty=N            largest order in shares (default 1000)\n"
        "  --max-notional=N       largest order value in dollars (default 1000000)\n"
        "  --max-rate=N           orders allowed per second (default 1000)\n"
        "  --cpu=N --mlock --idle-us=N --seconds=N --stats-sec=N");
}

void print_stats(const Gateway& gw, const TscScale& scale, bool with_latency) {
    const gateway::GatewayStats& s = gw.stats();
    std::printf("%s: requests=%llu sent=%llu risk_rejects=%llu not_connected=%llu "
                "backpressure=%llu reports=%llu reports_dropped=%llu session=%s\n",
                kApp, static_cast<unsigned long long>(s.requests),
                static_cast<unsigned long long>(s.sent),
                static_cast<unsigned long long>(s.risk_rejects),
                static_cast<unsigned long long>(s.not_connected),
                static_cast<unsigned long long>(s.backpressure),
                static_cast<unsigned long long>(s.reports_published),
                static_cast<unsigned long long>(s.reports_dropped), gw.active() ? "up" : "down");
    if (with_latency && gw.latency().tick_to_trade.count() > 0) {
        perf::print(stdout, "tick-to-trade", perf::summarize(gw.latency().tick_to_trade, scale));
        perf::print(stdout, "feed-to-decision",
                    perf::summarize(gw.latency().feed_to_decision, scale));
        perf::print(stdout, "decision-to-wire",
                    perf::summarize(gw.latency().decision_to_wire, scale));
        perf::print(stdout, "  ring-hop", perf::summarize(gw.latency().decision_to_gateway, scale));
        perf::print(stdout, "  gateway-send", perf::summarize(gw.latency().gateway_to_wire, scale));
    }
    static_cast<void>(std::fflush(stdout));
}

bool connect_venue(net::TcpStream& link, const net::Endpoint& venue, const app::Runtime& rt) {
    constexpr int kAttempts = 300;
    for (int attempt = 0; attempt < kAttempts && app::running(); ++attempt) {
        if (rt.expired(monotonic_ns())) {
            return false;
        }
        link.close();
        if (link.connect(venue, 1000).ok()) {
            return true;
        }
        if (attempt == 10) {
            std::fprintf(stderr, "%s: waiting for the venue ...\n", kApp);
        }
        static_cast<void>(::usleep(100'000));
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    const app::Args args{argc, argv, kApp};
    if (args.flag("help")) {
        usage();
        return 0;
    }
    args.expect({"venue", "user", "password", "firm", "symbols", "max-qty", "max-notional",
                 "max-rate", "cpu", "mlock", "idle-us", "seconds", "stats-sec"});
    app::Runtime rt = app::start(args);
    const app::Symbols symbols = app::parse_symbols(args);
    const net::Endpoint venue = app::parse_endpoint(args, "venue", app::kDefaultOrderEntry);

    ipc::Channel<gateway::ExecutionReport> reports;
    app::create_channel(args, reports, app::kReportChannel, app::kReportCapacity);
    ipc::Channel<gateway::OrderRequest> requests;
    app::open_channel(args, requests, app::kRequestChannel);
    ipc::SpscProducer<gateway::ExecutionReport> report_tx = reports.producer();
    ipc::SpscConsumer<gateway::OrderRequest> request_rx = requests.consumer();

    gateway::GatewayConfig config{};
    config.soup.username = wire::Alpha<6>{args.get("user", "HOTPTH")};
    config.soup.password = wire::Alpha<10>{args.get("password", "")};
    config.soup.heartbeat_ticks = rt.ticks_per_second;
    config.soup.timeout_ticks = rt.ticks_per_second * 15U;
    config.firm = wire::Alpha<4>{args.get("firm", "")};
    config.risk = {.max_order_qty = static_cast<std::uint32_t>(args.number("max-qty", 1000)),
                   .max_order_notional = args.number("max-notional", 1'000'000) * 10'000U,
                   .max_orders_per_window =
                       static_cast<std::uint32_t>(args.number("max-rate", 1000)),
                   .window_ticks = rt.ticks_per_second};

    net::TcpStream link;
    if (!connect_venue(link, venue, rt)) {
        app::die(kApp, "cannot reach the venue");
    }
    static Gateway gw{link, request_rx, report_tx, symbols.span(), config};
    gw.connect();
    std::fprintf(stderr, "%s: connected, logging in\n", kApp);

    std::uint64_t spins = 0;
    while (app::running()) {
        rt.idle(gw.poll());
        const gateway::SessionState state = gw.state();
        if (state == gateway::SessionState::Rejected || state == gateway::SessionState::Ended) {
            std::fprintf(stderr, "%s: session %s by the venue\n", kApp,
                         state == gateway::SessionState::Rejected ? "rejected" : "ended");
            break;
        }
        if (state == gateway::SessionState::Failed) {
            std::fprintf(stderr, "%s: link lost, reconnecting\n", kApp);
            if (!connect_venue(link, venue, rt)) {
                break;
            }
            gw.connect();
        }
        if ((++spins & 0xFFFU) == 0) {
            const std::uint64_t now = monotonic_ns();
            if (rt.expired(now)) {
                break;
            }
            if (rt.stats_due(now)) {
                print_stats(gw, rt.scale, false);
            }
        }
    }

    gw.disconnect();
    print_stats(gw, rt.scale, true);
    static_cast<void>(ipc::Channel<gateway::ExecutionReport>::unlink(app::kReportChannel));
    return 0;
}
