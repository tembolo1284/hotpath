#ifndef HOTPATH_SIM_SOUP_SERVER_HPP
#define HOTPATH_SIM_SOUP_SERVER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/gateway/soup_session.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/net/socket.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/soup.hpp"

namespace hotpath::sim {

struct SoupServerConfig {
    wire::Alpha<10> session{wire::Alpha<10>::right_justified("HOTPATH001")};
    std::uint64_t heartbeat_ticks{1'000'000'000ULL};
    std::uint32_t log_capacity{0};
};

struct SoupServerStats {
    std::uint64_t logins{0};
    std::uint64_t rejected_logins{0};
    std::uint64_t unsequenced_in{0};
    std::uint64_t sequenced_out{0};
    std::uint64_t heartbeats_out{0};
    std::uint64_t dropped_connections{0};
    std::uint64_t malformed{0};
};

template <gateway::Transport T>
class SoupServer {
public:
    static constexpr std::size_t kMaxMessageBytes = 87;
    static constexpr std::size_t kRxBytes = 4096;
    static constexpr unsigned kMaxFramesPerPump = 128;
    static constexpr unsigned kMaxPacketsPerPoll = 64;

    SoupServer() noexcept = default;
    SoupServer(const SoupServer&) = delete;
    SoupServer& operator=(const SoupServer&) = delete;

    [[nodiscard]] Status init(mem::Arena& arena, const SoupServerConfig& config) noexcept {
        HOTPATH_ASSERT(log_ == nullptr);
        if (config.log_capacity == 0) {
            return Status{EINVAL};
        }
        Logged* log = arena.allocate_array<Logged>(config.log_capacity);
        if (log == nullptr) {
            return Status{ENOMEM};
        }
        log_ = log;
        capacity_ = config.log_capacity;
        config_ = config;
        return Status{};
    }

    void attach(T* transport, std::uint64_t now) noexcept {
        transport_ = transport;
        logged_in_ = false;
        rx_len_ = 0;
        pending_len_ = 0;
        pending_off_ = 0;
        last_tx_ = now;
    }

    template <wire::WireMessage M>
    void send_sequenced(const M& msg) noexcept {
        static_assert(sizeof(M) <= kMaxMessageBytes);
        HOTPATH_HOT_ASSERT(log_ != nullptr);
        Logged& slot = log_[(next_sequence_ - 1U) % capacity_];
        slot.len = static_cast<std::uint8_t>(sizeof(M));
        std::memcpy(slot.bytes, &msg, sizeof(M));
        next_sequence_ += 1;
    }

    template <typename H>
    void poll(std::uint64_t now, H& handler) noexcept {
        if (transport_ == nullptr) {
            return;
        }
        pump(now);
        receive();
        parse(now, handler);
        pump(now);
        if (transport_ != nullptr && logged_in_ && pending_len_ == 0 &&
            now - last_tx_ >= config_.heartbeat_ticks) {
            const wire::soup::Header hdr = wire::soup::make_header(wire::soup::kServerHeartbeat, 0);
            queue(std::as_bytes(std::span<const wire::soup::Header, 1>{&hdr, 1}));
            stats_.heartbeats_out += 1;
            pump(now);
        }
    }

    void pump(std::uint64_t now) noexcept {
        for (unsigned i = 0; i < kMaxFramesPerPump && transport_ != nullptr; ++i) {
            if (pending_off_ < pending_len_) {
                const net::IoResult io = transport_->send(
                    std::span<const std::byte>{pending_.data() + pending_off_,
                                               pending_len_ - pending_off_});
                if (!io.status.ok() && !io.would_block()) {
                    drop();
                    return;
                }
                pending_off_ += io.bytes;
                if (io.bytes > 0) {
                    last_tx_ = now;
                }
                if (pending_off_ < pending_len_) {
                    return;
                }
                pending_len_ = 0;
                pending_off_ = 0;
            }
            if (!logged_in_ || cursor_ == next_sequence_) {
                return;
            }
            if (cursor_ < oldest()) {
                drop();
                return;
            }
            const Logged& slot = log_[(cursor_ - 1U) % capacity_];
            const wire::soup::Header hdr =
                wire::soup::make_header(wire::soup::kSequencedData, slot.len);
            std::memcpy(pending_.data(), &hdr, sizeof(hdr));
            std::memcpy(pending_.data() + sizeof(hdr), slot.bytes, slot.len);
            pending_len_ = sizeof(hdr) + slot.len;
            pending_off_ = 0;
            cursor_ += 1;
            stats_.sequenced_out += 1;
        }
    }

    [[nodiscard]] bool connected() const noexcept { return transport_ != nullptr; }
    [[nodiscard]] bool logged_in() const noexcept { return logged_in_; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
    [[nodiscard]] std::uint64_t client_cursor() const noexcept { return cursor_; }
    [[nodiscard]] const SoupServerStats& stats() const noexcept { return stats_; }

private:
    struct Logged {
        std::uint8_t len{0};
        std::byte bytes[kMaxMessageBytes]{};
    };

    [[nodiscard]] std::uint64_t oldest() const noexcept {
        return next_sequence_ > capacity_ ? next_sequence_ - capacity_ : 1U;
    }

    void drop() noexcept {
        transport_ = nullptr;
        logged_in_ = false;
        stats_.dropped_connections += 1;
    }

    void queue(std::span<const std::byte> frame) noexcept {
        HOTPATH_ASSERT(pending_len_ == 0 && frame.size() <= pending_.size());
        std::memcpy(pending_.data(), frame.data(), frame.size());
        pending_len_ = frame.size();
        pending_off_ = 0;
    }

    void receive() noexcept {
        if (transport_ == nullptr || rx_len_ == rx_.size()) {
            return;
        }
        const net::IoResult io =
            transport_->recv(std::span<std::byte>{rx_.data() + rx_len_, rx_.size() - rx_len_});
        if (io.eof || (!io.status.ok() && !io.would_block())) {
            drop();
            return;
        }
        rx_len_ += io.bytes;
    }

    template <typename H>
    void parse(std::uint64_t now, H& handler) noexcept {
        std::size_t offset = 0;
        for (unsigned i = 0; i < kMaxPacketsPerPoll && transport_ != nullptr; ++i) {
            const wire::soup::Packet packet = wire::soup::next_packet(
                std::span<const std::byte>{rx_.data() + offset, rx_len_ - offset});
            if (packet.result == wire::Parse::Truncated) {
                break;
            }
            if (packet.result != wire::Parse::Ok) {
                stats_.malformed += 1;
                drop();
                break;
            }
            offset += packet.consumed;
            switch (packet.type) {
                case wire::soup::kLoginRequest:
                    login(packet.payload, now);
                    break;
                case wire::soup::kUnsequencedData:
                    if (logged_in_) {
                        stats_.unsequenced_in += 1;
                        handler.on_unsequenced(packet.payload);
                    }
                    break;
                case wire::soup::kLogoutRequest:
                    transport_ = nullptr;
                    logged_in_ = false;
                    break;
                case wire::soup::kClientHeartbeat:
                case wire::soup::kDebug:
                    break;
                default:
                    stats_.malformed += 1;
                    break;
            }
        }
        if (offset > 0 && offset <= rx_len_) {
            std::memmove(rx_.data(), rx_.data() + offset, rx_len_ - offset);
            rx_len_ -= offset;
        }
    }

    void login(std::span<const std::byte> payload, std::uint64_t now) noexcept {
        constexpr std::size_t kBody = sizeof(wire::soup::LoginRequest) - sizeof(wire::soup::Header);
        if (logged_in_ || pending_len_ != 0 || payload.size() < kBody) {
            stats_.malformed += 1;
            return;
        }
        wire::Alpha<10> session;
        wire::Alpha<20> sequence;
        std::memcpy(&session, payload.data() + 16, sizeof(session));
        std::memcpy(&sequence, payload.data() + 26, sizeof(sequence));

        const wire::Numeric requested = wire::parse_numeric(sequence.raw());
        const bool session_ok =
            session.trimmed().empty() || session.trimmed() == config_.session.trimmed();
        std::uint64_t start = requested.value == 0 || requested.value > next_sequence_
                                  ? next_sequence_
                                  : requested.value;
        if (!requested.ok || !session_ok || start < oldest()) {
            auto rejected = wire::soup::make<wire::soup::LoginRejected>(wire::soup::kLoginRejected);
            rejected.reason = wire::soup::kRejectSessionNotAvailable;
            queue(std::as_bytes(std::span<const wire::soup::LoginRejected, 1>{&rejected, 1}));
            stats_.rejected_logins += 1;
            return;
        }

        auto accepted = wire::soup::make<wire::soup::LoginAccepted>(wire::soup::kLoginAccepted);
        accepted.session = config_.session;
        accepted.sequence = wire::format_numeric<20>(start);
        queue(std::as_bytes(std::span<const wire::soup::LoginAccepted, 1>{&accepted, 1}));
        cursor_ = start;
        logged_in_ = true;
        last_tx_ = now;
        stats_.logins += 1;
    }

    T* transport_{nullptr};
    Logged* log_{nullptr};
    std::uint32_t capacity_{0};
    SoupServerConfig config_{};
    SoupServerStats stats_{};
    std::uint64_t next_sequence_{1};
    std::uint64_t cursor_{1};
    std::uint64_t last_tx_{0};
    bool logged_in_{false};
    std::size_t rx_len_{0};
    std::size_t pending_len_{0};
    std::size_t pending_off_{0};
    std::array<std::byte, kRxBytes> rx_{};
    std::array<std::byte, sizeof(wire::soup::Header) + kMaxMessageBytes> pending_{};
};

} // namespace hotpath::sim

#endif // HOTPATH_SIM_SOUP_SERVER_HPP
