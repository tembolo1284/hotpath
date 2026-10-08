#ifndef HOTPATH_SIM_EXCHANGE_HPP
#define HOTPATH_SIM_EXCHANGE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hotpath/core/status.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/net/endpoint.hpp"
#include "hotpath/net/tcp.hpp"
#include "hotpath/net/udp.hpp"
#include "hotpath/sim/itch_publisher.hpp"
#include "hotpath/sim/liquidity.hpp"
#include "hotpath/sim/matching_engine.hpp"
#include "hotpath/sim/soup_server.hpp"
#include "hotpath/sim/token_map.hpp"
#include "hotpath/sim/venue.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/mold.hpp"
#include "hotpath/wire/ouch.hpp"

namespace hotpath::sim {

struct ExchangeConfig {
    net::Endpoint order_entry{};
    net::Endpoint feed_destination{};
    net::Endpoint retransmit{};
    std::uint32_t feed_interface_be{0};
    EngineConfig engine{};
    std::uint32_t max_tokens{0};
    SoupServerConfig soup{};
    PublisherConfig publisher{};
    LiquidityConfig liquidity{};
    std::uint64_t feed_heartbeat_ticks{1'000'000'000ULL};
    std::uint32_t liquidity_steps_per_poll{1};
};

struct ExchangeStats {
    std::uint64_t connections{0};
    std::uint64_t retransmit_requests{0};
    std::uint64_t retransmit_replies{0};
    std::uint64_t unknown_inbound{0};
};

class Exchange {
public:
    using Publisher = ItchPublisher<net::UdpSender>;
    using Session = SoupServer<net::TcpStream>;
    using VenueType = Venue<Publisher, Session>;

    Exchange() noexcept = default;
    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;

    [[nodiscard]] Status init(mem::Arena& arena, const ExchangeConfig& config,
                              std::span<const wire::Alpha<8>> tickers,
                              std::uint64_t now) noexcept {
        if (tickers.size() != config.engine.symbols) {
            return Status{EINVAL};
        }
        wire::Alpha<8>* owned = arena.allocate_array<wire::Alpha<8>>(tickers.size());
        if (owned == nullptr) {
            return Status{ENOMEM};
        }
        for (std::size_t i = 0; i < tickers.size(); ++i) {
            owned[i] = tickers[i];
        }
        tickers_ = std::span<const wire::Alpha<8>>{owned, tickers.size()};
        venue_.set_tickers(tickers_);

        Status st = engine_.init(arena, config.engine);
        if (st.ok()) {
            st = tokens_.init(arena, config.max_tokens);
        }
        if (st.ok()) {
            st = publisher_.init(arena, config.publisher);
        }
        if (st.ok()) {
            st = soup_.init(arena, config.soup);
        }
        if (st.ok()) {
            st = liquidity_.init(arena, config.engine.symbols, config.liquidity);
        }
        if (st.ok()) {
            st = feed_socket_.open(net::UdpSenderConfig{.destination = config.feed_destination,
                                                        .interface_be = config.feed_interface_be});
        }
        if (st.ok()) {
            st = retransmit_.open(config.retransmit);
        }
        if (st.ok()) {
            st = listener_.listen(config.order_entry, 4);
        }
        if (!st.ok()) {
            return st;
        }

        steps_per_poll_ = config.liquidity_steps_per_poll;
        heartbeat_ticks_ = config.feed_heartbeat_ticks;
        last_feed_ = now;
        venue_.set_time(now);
        venue_.open_market();
        publisher_.flush();
        return Status{};
    }

    void poll(std::uint64_t now) noexcept {
        venue_.set_time(now);

        net::TcpStream incoming;
        if (listener_.accept(incoming).ok()) {
            client_ = static_cast<net::TcpStream&&>(incoming);
            soup_.attach(&client_, now);
            stats_.connections += 1;
        }
        soup_.poll(now, *this);

        for (std::uint32_t i = 0; i < steps_per_poll_; ++i) {
            liquidity_.step(venue_);
        }

        const std::uint64_t before = publisher_.packets_sent();
        publisher_.flush();
        if (publisher_.packets_sent() != before) {
            last_feed_ = now;
        } else if (now - last_feed_ >= heartbeat_ticks_) {
            publisher_.heartbeat();
            last_feed_ = now;
        }

        serve_retransmits();
        soup_.pump(now);
    }

    void on_unsequenced(std::span<const std::byte> payload) noexcept {
        if (wire::ouch::dispatch_inbound(payload, venue_) != wire::Parse::Ok) {
            stats_.unknown_inbound += 1;
        }
    }

    void set_liquidity_steps(std::uint32_t steps) noexcept { steps_per_poll_ = steps; }

    [[nodiscard]] net::EndpointResult order_entry_endpoint() const noexcept {
        return listener_.local_endpoint();
    }
    [[nodiscard]] net::EndpointResult retransmit_endpoint() const noexcept {
        return retransmit_.local_endpoint();
    }

    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] const Publisher& publisher() const noexcept { return publisher_; }
    [[nodiscard]] const Session& session() const noexcept { return soup_; }
    [[nodiscard]] VenueType& venue() noexcept { return venue_; }
    [[nodiscard]] const Liquidity& liquidity() const noexcept { return liquidity_; }
    [[nodiscard]] const ExchangeStats& stats() const noexcept { return stats_; }

private:
    static constexpr unsigned kMaxRequestsPerPoll = 8;

    void serve_retransmits() noexcept {
        for (unsigned i = 0; i < kMaxRequestsPerPoll; ++i) {
            wire::mold::Request request{};
            net::Endpoint from{};
            const net::IoResult io = retransmit_.recv_from(
                std::as_writable_bytes(std::span<wire::mold::Request, 1>{&request, 1}), from);
            if (!io.ok() || io.bytes == 0) {
                return;
            }
            if (io.bytes != sizeof(request)) {
                continue;
            }
            stats_.retransmit_requests += 1;
            const std::span<const std::byte> reply = publisher_.retransmit(
                request.sequence.get(), request.requested_count.get(), reply_buffer_);
            if (!reply.empty() && retransmit_.send_to(from, reply).ok()) {
                stats_.retransmit_replies += 1;
            }
        }
    }

    MatchingEngine engine_{};
    TokenMap tokens_{};
    net::UdpSender feed_socket_{};
    Publisher publisher_{feed_socket_};
    Session soup_{};
    std::span<const wire::Alpha<8>> tickers_{};
    Liquidity liquidity_{};
    net::TcpListener listener_{};
    net::TcpStream client_{};
    net::UdpServer retransmit_{};
    VenueType venue_{engine_, tokens_, publisher_, soup_, tickers_};
    ExchangeStats stats_{};
    std::uint64_t heartbeat_ticks_{0};
    std::uint64_t last_feed_{0};
    std::uint32_t steps_per_poll_{0};
    std::array<std::byte, wire::mold::kMaxPacketBytes> reply_buffer_{};
};

} // namespace hotpath::sim

#endif // HOTPATH_SIM_EXCHANGE_HPP
