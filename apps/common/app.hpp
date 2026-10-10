#ifndef HOTPATH_APPS_COMMON_APP_HPP
#define HOTPATH_APPS_COMMON_APP_HPP

#include <time.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>

#include "hotpath/core/clock.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/ipc/channel.hpp"
#include "hotpath/mem/region.hpp"
#include "hotpath/net/endpoint.hpp"
#include "hotpath/sys/affinity.hpp"
#include "hotpath/sys/isolation.hpp"
#include "hotpath/sys/memory.hpp"
#include "hotpath/wire/fields.hpp"

namespace hotpath::app {

inline constexpr const char* kMarketChannel = "/hotpath-market";
inline constexpr const char* kRequestChannel = "/hotpath-requests";
inline constexpr const char* kReportChannel = "/hotpath-reports";

inline constexpr std::string_view kDefaultFeed = "127.0.0.1:31001";
inline constexpr std::string_view kDefaultRetransmit = "127.0.0.1:31002";
inline constexpr std::string_view kDefaultOrderEntry = "127.0.0.1:31003";
inline constexpr std::string_view kDefaultSymbols = "MSFT,NVDA,AAPL,AMZN";

inline constexpr std::uint64_t kMarketCapacity = 1U << 16;
inline constexpr std::uint64_t kRequestCapacity = 1U << 12;
inline constexpr std::uint64_t kReportCapacity = 1U << 14;
inline constexpr std::size_t kMaxSymbols = 64;

[[noreturn]] inline void die(const char* app, const char* what, Status status = Status{}) noexcept {
    if (status.ok()) {
        static_cast<void>(std::fprintf(stderr, "%s: %s\n", app, what));
    } else {
        static_cast<void>(std::fprintf(stderr, "%s: %s: %s (errno %d)\n", app, what,
                                       std::strerror(status.err()), status.err()));
    }
    std::exit(1);
}

class Args {
public:
    Args(int argc, char** argv, const char* app) noexcept : argc_{argc}, argv_{argv}, app_{app} {}

    [[nodiscard]] const char* app() const noexcept { return app_; }

    [[nodiscard]] bool flag(std::string_view key) const noexcept {
        for (int i = 1; i < argc_; ++i) {
            const std::string_view arg{argv_[i]};
            if (arg.size() == key.size() + 2 && arg.substr(0, 2) == "--" && arg.substr(2) == key) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string_view get(std::string_view key,
                                       std::string_view fallback = {}) const noexcept {
        for (int i = 1; i < argc_; ++i) {
            const std::string_view arg{argv_[i]};
            if (arg.size() > key.size() + 2 && arg.substr(0, 2) == "--" &&
                arg.substr(2, key.size()) == key && arg[key.size() + 2] == '=') {
                return arg.substr(key.size() + 3);
            }
        }
        return fallback;
    }

    [[nodiscard]] bool has(std::string_view key) const noexcept {
        return flag(key) || get(key, "\x01") != "\x01";
    }

    [[nodiscard]] std::uint64_t number(std::string_view key, std::uint64_t fallback) const noexcept {
        const std::string_view text = get(key);
        if (text.empty()) {
            return fallback;
        }
        std::uint64_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            static_cast<void>(std::fprintf(stderr, "%s: --%.*s expects a number\n", app_,
                                           static_cast<int>(key.size()), key.data()));
            std::exit(2);
        }
        return value;
    }

    void expect(std::initializer_list<std::string_view> known) const noexcept {
        for (int i = 1; i < argc_; ++i) {
            const std::string_view arg{argv_[i]};
            const std::size_t eq = arg.find('=');
            const std::string_view name =
                arg.size() > 2 && arg.substr(0, 2) == "--" ? arg.substr(2, eq == arg.npos ? eq : eq - 2)
                                                           : std::string_view{};
            bool ok = false;
            for (const std::string_view candidate : known) {
                ok = ok || candidate == name;
            }
            if (!ok) {
                static_cast<void>(std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n",
                                               app_, argv_[i]));
                std::exit(2);
            }
        }
    }

private:
    int argc_;
    char** argv_;
    const char* app_;
};

[[nodiscard]] inline net::Endpoint parse_endpoint(const Args& args, std::string_view key,
                                                  std::string_view fallback) noexcept {
    const std::string_view text = args.get(key, fallback);
    const std::size_t colon = text.rfind(':');
    char host[32]{};
    std::uint16_t port = 0;
    bool ok = colon != text.npos && colon > 0 && colon < sizeof(host);
    if (ok) {
        std::memcpy(host, text.data(), colon);
        const std::string_view digits = text.substr(colon + 1);
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), port);
        ok = result.ec == std::errc{} && result.ptr == digits.data() + digits.size();
    }
    const net::EndpointResult parsed = ok ? net::make_endpoint(host, port) : net::EndpointResult{{}, Status{EINVAL}};
    if (!parsed.status.ok()) {
        static_cast<void>(std::fprintf(stderr, "%s: --%.*s expects ip:port, got '%.*s'\n",
                                       args.app(), static_cast<int>(key.size()), key.data(),
                                       static_cast<int>(text.size()), text.data()));
        std::exit(2);
    }
    return parsed.endpoint;
}

struct Symbols {
    std::array<wire::Alpha<8>, kMaxSymbols> tickers{};
    std::uint16_t count{0};

    [[nodiscard]] std::span<const wire::Alpha<8>> span() const noexcept {
        return std::span<const wire::Alpha<8>>{tickers.data(), count};
    }
};

[[nodiscard]] inline Symbols parse_symbols(const Args& args) noexcept {
    Symbols out{};
    std::string_view rest = args.get("symbols", kDefaultSymbols);
    for (std::size_t i = 0; i < kMaxSymbols && !rest.empty(); ++i) {
        const std::size_t comma = rest.find(',');
        const std::string_view ticker = rest.substr(0, comma);
        if (ticker.empty() || ticker.size() > 8) {
            die(args.app(), "--symbols expects comma-separated tickers of 1 to 8 characters");
        }
        out.tickers[out.count] = wire::Alpha<8>{ticker};
        out.count = static_cast<std::uint16_t>(out.count + 1U);
        rest = comma == rest.npos ? std::string_view{} : rest.substr(comma + 1);
    }
    if (out.count == 0 || !rest.empty()) {
        die(args.app(), "--symbols needs between 1 and 64 tickers");
    }
    return out;
}

namespace detail {
inline volatile std::sig_atomic_t g_stop = 0;
inline void on_signal(int) noexcept { g_stop = 1; }
} // namespace detail

inline void install_signal_handlers() noexcept {
    struct sigaction action {};
    action.sa_handler = detail::on_signal;
    static_cast<void>(::sigaction(SIGINT, &action, nullptr));
    static_cast<void>(::sigaction(SIGTERM, &action, nullptr));
}

[[nodiscard]] inline bool running() noexcept { return detail::g_stop == 0; }

struct Runtime {
    TscScale scale{};
    std::uint64_t ticks_per_second{0};
    std::uint64_t start_ns{0};
    std::uint64_t deadline_ns{0};
    std::uint64_t stats_interval_ns{0};
    std::uint64_t next_stats_ns{0};
    std::uint32_t idle_us{0};

    [[nodiscard]] bool expired(std::uint64_t now_ns) const noexcept {
        return deadline_ns != 0 && now_ns >= deadline_ns;
    }

    [[nodiscard]] bool stats_due(std::uint64_t now_ns) noexcept {
        if (stats_interval_ns == 0 || now_ns < next_stats_ns) {
            return false;
        }
        next_stats_ns = now_ns + stats_interval_ns;
        return true;
    }

    void idle(std::uint32_t handled) const noexcept {
        if (handled == 0 && idle_us != 0) {
            static_cast<void>(::usleep(idle_us));
        }
    }
};

[[nodiscard]] inline TscScale calibrate(const char* app) noexcept {
    if (has_invariant_tsc()) {
        return calibrate_tsc(50'000'000ULL);
    }
    static_cast<void>(std::fprintf(
        stderr, "%s: warning: no invariant TSC reported; latency figures may be unreliable\n", app));
    timespec nap{0, 50'000'000L};
    const std::uint64_t ns0 = monotonic_ns();
    const std::uint64_t c0 = tsc_now();
    static_cast<void>(::nanosleep(&nap, nullptr));
    const std::uint64_t c1 = tsc_now();
    const std::uint64_t ns1 = monotonic_ns();
    return TscScale{c1 > c0 ? ((ns1 - ns0) << 32) / (c1 - c0) : (std::uint64_t{1} << 32)};
}

[[nodiscard]] inline Runtime start(const Args& args) noexcept {
    const char* app = args.app();
    install_signal_handlers();

    if (args.has("cpu")) {
        const auto cpu = static_cast<unsigned>(args.number("cpu", 0));
        const Status pinned = sys::pin_current_thread(cpu);
        if (!pinned.ok()) {
            die(app, "cannot pin to the requested cpu", pinned);
        }
        const sys::IsolationResult isolation = sys::is_cpu_isolated(cpu);
        static_cast<void>(std::fprintf(stderr, "%s: pinned to cpu %u (%s)\n", app, cpu,
                                       isolation.isolated ? "isolated" : "not isolated"));
    }
    if (args.flag("mlock")) {
        const Status locked = sys::lock_all_memory();
        if (!locked.ok()) {
            static_cast<void>(std::fprintf(stderr, "%s: warning: mlockall failed (errno %d)\n", app,
                                           locked.err()));
        }
    }

    Runtime rt{};
    rt.scale = calibrate(app);
    rt.ticks_per_second =
        rt.scale.ns_per_tick_q32 == 0 ? 0 : (1'000'000'000ULL << 32) / rt.scale.ns_per_tick_q32;
    rt.start_ns = monotonic_ns();
    const std::uint64_t seconds = args.number("seconds", 0);
    rt.deadline_ns = seconds == 0 ? 0 : rt.start_ns + seconds * 1'000'000'000ULL;
    rt.stats_interval_ns = args.number("stats-sec", 5) * 1'000'000'000ULL;
    rt.next_stats_ns = rt.start_ns + rt.stats_interval_ns;
    rt.idle_us = static_cast<std::uint32_t>(args.number("idle-us", 0));
    return rt;
}

inline void map_region(const Args& args, mem::Region& region, std::size_t bytes) noexcept {
    const char* app = args.app();
    Status st = Status{ENOMEM};
    if (!args.flag("no-hugepages")) {
        st = region.map(bytes, sys::PageKind::Huge2M);
    }
    if (!st.ok()) {
        st = region.map(bytes, sys::PageKind::Small);
        if (!st.ok()) {
            die(app, "cannot map memory", st);
        }
        if (!args.flag("no-hugepages")) {
            static_cast<void>(std::fprintf(stderr, "%s: no 2M huge pages available, using 4K pages\n", app));
        }
    }
    if (args.has("cpu")) {
        const Status bound = region.bind_to_current_node();
        if (!bound.ok()) {
            static_cast<void>(std::fprintf(stderr, "%s: warning: NUMA bind failed (errno %d)\n", app,
                                           bound.err()));
        }
    }
    region.prefault();
}

template <ShmSafe T>
void create_channel(const Args& args, ipc::Channel<T>& channel, const char* name,
                    std::uint64_t capacity) noexcept {
    static_cast<void>(ipc::Channel<T>::unlink(name));
    const Status st = channel.create(name, capacity);
    if (!st.ok()) {
        die(args.app(), name, st);
    }
}

template <ShmSafe T>
void open_channel(const Args& args, ipc::Channel<T>& channel, const char* name) noexcept {
    constexpr int kAttempts = 3000;
    Status st{};
    for (int attempt = 0; attempt < kAttempts && running(); ++attempt) {
        st = channel.open(name);
        if (st.ok()) {
            return;
        }
        if (attempt == 100) {
            static_cast<void>(std::fprintf(stderr, "%s: waiting for %s ...\n", args.app(), name));
        }
        static_cast<void>(::usleep(10'000));
    }
    if (!running()) {
        std::exit(0);
    }
    die(args.app(), name, st);
}

} // namespace hotpath::app

#endif // HOTPATH_APPS_COMMON_APP_HPP
