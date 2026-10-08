#ifndef HOTPATH_GATEWAY_SOUP_SESSION_HPP
#define HOTPATH_GATEWAY_SOUP_SESSION_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "hotpath/net/socket.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/soup.hpp"

namespace hotpath::gateway {

template <typename T>
concept Transport = requires(T& transport, std::span<const std::byte> out, std::span<std::byte> in) {
    { transport.send(out) } -> std::same_as<net::IoResult>;
    { transport.recv(in) } -> std::same_as<net::IoResult>;
};

enum class SessionState : std::uint8_t {
    Idle,
    LoggingIn,
    Active,
    Rejected,
    Ended,
    Failed,
};

struct SoupConfig {
    wire::Alpha<6> username{};
    wire::Alpha<10> password{};
    wire::Alpha<10> session{};
    std::uint64_t heartbeat_ticks{3'000'000'000ULL};
    std::uint64_t timeout_ticks{45'000'000'000ULL};
};

struct SoupStats {
    std::uint64_t packets_in{0};
    std::uint64_t sequenced_in{0};
    std::uint64_t heartbeats_in{0};
    std::uint64_t heartbeats_out{0};
    std::uint64_t malformed{0};
    std::uint64_t tx_backpressure{0};
};

template <wire::WireMessage M>
struct Frame {
    wire::soup::Header hdr;
    M msg;
};

template <Transport T>
class SoupSession {
public:
    static constexpr std::size_t kRxBytes = 4096;
    static constexpr std::size_t kTxBytes = 2048;
    static constexpr unsigned kMaxPacketsPerPoll = 64;

    explicit SoupSession(T& transport) noexcept : transport_{&transport} {}

    SoupSession(const SoupSession&) = delete;
    SoupSession& operator=(const SoupSession&) = delete;

    void login(const SoupConfig& config, std::uint64_t now) noexcept {
        config_ = config;
        rx_len_ = 0;
        tx_len_ = 0;
        last_rx_ = now;
        last_tx_ = now;
        reject_reason_ = 0;
        state_ = SessionState::LoggingIn;

        auto request = wire::soup::make<wire::soup::LoginRequest>(wire::soup::kLoginRequest);
        request.username = config.username;
        request.password = config.password;
        request.requested_session = config.session;
        request.requested_sequence = wire::format_numeric<20>(next_sequence_);
        static_cast<void>(raw_send(bytes_of(request), now));
    }

    void logout(std::uint64_t now) noexcept {
        if (state_ == SessionState::Active || state_ == SessionState::LoggingIn) {
            const wire::soup::Header hdr = wire::soup::make_header(wire::soup::kLogoutRequest, 0);
            static_cast<void>(raw_send(bytes_of(hdr), now));
            state_ = SessionState::Idle;
        }
    }

    template <typename H>
    void poll(std::uint64_t now, H& handler) noexcept {
        if (running()) {
            flush(now);
        }
        if (running()) {
            receive(now);
        }
        if (running()) {
            parse(handler);
        }
        if (state_ == SessionState::Active && now - last_tx_ >= config_.heartbeat_ticks) {
            const wire::soup::Header hdr = wire::soup::make_header(wire::soup::kClientHeartbeat, 0);
            if (raw_send(bytes_of(hdr), now)) {
                stats_.heartbeats_out += 1;
            }
        }
        if (running() && now - last_rx_ >= config_.timeout_ticks) {
            state_ = SessionState::Failed;
        }
        if (state_ != notified_) {
            notified_ = state_;
            if constexpr (requires { handler.on_session(state_); }) {
                handler.on_session(state_);
            }
        }
    }

    template <wire::WireMessage M>
    [[nodiscard]] bool send_unsequenced(const M& msg, std::uint64_t now) noexcept {
        if (state_ != SessionState::Active) {
            return false;
        }
        Frame<M> frame{wire::soup::make_header(wire::soup::kUnsequencedData, sizeof(M)), msg};
        return raw_send(bytes_of(frame), now);
    }

    void set_next_sequence(std::uint64_t sequence) noexcept { next_sequence_ = sequence; }

    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] bool active() const noexcept { return state_ == SessionState::Active; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
    [[nodiscard]] const wire::Alpha<10>& session_id() const noexcept { return config_.session; }
    [[nodiscard]] char reject_reason() const noexcept { return reject_reason_; }
    [[nodiscard]] std::size_t tx_pending() const noexcept { return tx_len_; }
    [[nodiscard]] const SoupStats& stats() const noexcept { return stats_; }

private:
    template <typename V>
    [[nodiscard]] static std::span<const std::byte> bytes_of(const V& value) noexcept {
        return std::as_bytes(std::span<const V, 1>{&value, 1});
    }

    [[nodiscard]] bool running() const noexcept {
        return state_ == SessionState::Active || state_ == SessionState::LoggingIn;
    }

    [[nodiscard]] bool stash(std::span<const std::byte> rest) noexcept {
        if (rest.size() > tx_.size() - tx_len_) {
            stats_.tx_backpressure += 1;
            return false;
        }
        std::memcpy(tx_.data() + tx_len_, rest.data(), rest.size());
        tx_len_ += rest.size();
        return true;
    }

    [[nodiscard]] bool raw_send(std::span<const std::byte> data, std::uint64_t now) noexcept {
        if (tx_len_ != 0) {
            return stash(data);
        }
        const net::IoResult io = transport_->send(data);
        if (!io.status.ok() && !io.would_block()) {
            state_ = SessionState::Failed;
            return false;
        }
        last_tx_ = now;
        if (io.bytes == data.size()) {
            return true;
        }
        return stash(data.subspan(io.bytes));
    }

    void flush(std::uint64_t now) noexcept {
        if (tx_len_ == 0) {
            return;
        }
        const net::IoResult io = transport_->send(std::span<const std::byte>{tx_.data(), tx_len_});
        if (!io.status.ok() && !io.would_block()) {
            state_ = SessionState::Failed;
            return;
        }
        if (io.bytes > 0) {
            std::memmove(tx_.data(), tx_.data() + io.bytes, tx_len_ - io.bytes);
            tx_len_ -= io.bytes;
            last_tx_ = now;
        }
    }

    void receive(std::uint64_t now) noexcept {
        if (rx_len_ == rx_.size()) {
            return;
        }
        const net::IoResult io =
            transport_->recv(std::span<std::byte>{rx_.data() + rx_len_, rx_.size() - rx_len_});
        if (io.eof || (!io.status.ok() && !io.would_block())) {
            state_ = SessionState::Failed;
            return;
        }
        if (io.bytes > 0) {
            rx_len_ += io.bytes;
            last_rx_ = now;
        }
    }

    template <typename H>
    void parse(H& handler) noexcept {
        std::size_t offset = 0;
        for (unsigned i = 0; i < kMaxPacketsPerPoll && running(); ++i) {
            const std::span<const std::byte> rest{rx_.data() + offset, rx_len_ - offset};
            const wire::soup::Packet packet = wire::soup::next_packet(rest);
            if (packet.result == wire::Parse::Truncated) {
                if (rest.size() >= sizeof(wire::U16) &&
                    reinterpret_cast<const wire::U16*>(rest.data())->get() + sizeof(wire::U16) >
                        rx_.size()) {
                    stats_.malformed += 1;
                    state_ = SessionState::Failed;
                }
                break;
            }
            if (packet.result != wire::Parse::Ok) {
                stats_.malformed += 1;
                state_ = SessionState::Failed;
                break;
            }
            offset += packet.consumed;
            stats_.packets_in += 1;
            handle(packet, handler);
        }
        if (offset > 0) {
            std::memmove(rx_.data(), rx_.data() + offset, rx_len_ - offset);
            rx_len_ -= offset;
        }
    }

    template <typename H>
    void handle(const wire::soup::Packet& packet, H& handler) noexcept {
        switch (packet.type) {
            case wire::soup::kSequencedData:
                if (state_ == SessionState::Active) {
                    next_sequence_ += 1;
                    stats_.sequenced_in += 1;
                    handler.on_sequenced(packet.payload);
                } else {
                    stats_.malformed += 1;
                }
                break;
            case wire::soup::kServerHeartbeat:
                stats_.heartbeats_in += 1;
                break;
            case wire::soup::kLoginAccepted:
                accept_login(packet.payload);
                break;
            case wire::soup::kLoginRejected:
                reject_reason_ = packet.payload.empty() ? char{0} : static_cast<char>(packet.payload[0]);
                state_ = SessionState::Rejected;
                break;
            case wire::soup::kEndOfSession:
                state_ = SessionState::Ended;
                break;
            case wire::soup::kDebug:
                break;
            default:
                stats_.malformed += 1;
                break;
        }
    }

    void accept_login(std::span<const std::byte> payload) noexcept {
        constexpr std::size_t kBody = sizeof(wire::soup::LoginAccepted) - sizeof(wire::soup::Header);
        if (state_ != SessionState::LoggingIn || payload.size() < kBody) {
            stats_.malformed += 1;
            state_ = SessionState::Failed;
            return;
        }
        wire::Alpha<10> session;
        wire::Alpha<20> sequence;
        std::memcpy(&session, payload.data(), sizeof(session));
        std::memcpy(&sequence, payload.data() + sizeof(session), sizeof(sequence));
        const wire::Numeric next = wire::parse_numeric(sequence.raw());
        if (!next.ok) {
            stats_.malformed += 1;
            state_ = SessionState::Failed;
            return;
        }
        config_.session = session;
        next_sequence_ = next.value;
        state_ = SessionState::Active;
    }

    T* transport_;
    SoupConfig config_{};
    SoupStats stats_{};
    SessionState state_{SessionState::Idle};
    SessionState notified_{SessionState::Idle};
    char reject_reason_{0};
    std::uint64_t next_sequence_{1};
    std::uint64_t last_rx_{0};
    std::uint64_t last_tx_{0};
    std::size_t rx_len_{0};
    std::size_t tx_len_{0};
    std::array<std::byte, kRxBytes> rx_{};
    std::array<std::byte, kTxBytes> tx_{};
};

} // namespace hotpath::gateway

#endif // HOTPATH_GATEWAY_SOUP_SESSION_HPP
