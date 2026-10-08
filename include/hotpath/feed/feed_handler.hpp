#ifndef HOTPATH_FEED_FEED_HANDLER_HPP
#define HOTPATH_FEED_FEED_HANDLER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hotpath/book/book_set.hpp"
#include "hotpath/core/clock.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/feed/book_builder.hpp"
#include "hotpath/feed/market_update.hpp"
#include "hotpath/feed/sequencer.hpp"
#include "hotpath/feed/symbol_table.hpp"
#include "hotpath/net/endpoint.hpp"
#include "hotpath/net/udp.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/itch.hpp"
#include "hotpath/wire/mold.hpp"

namespace hotpath::feed {

enum class Source : std::uint8_t { Live, Recovery };

struct FeedConfig {
    net::UdpReceiverConfig line_a{};
    net::UdpReceiverConfig line_b{};
    net::Endpoint recovery{};
    bool has_line_b{false};
    bool has_recovery{false};
    std::uint64_t start_sequence{1};
    std::uint16_t request_batch{512};
    std::uint64_t request_interval_ticks{3'000'000};
};

struct FeedStats {
    std::uint64_t packets{0};
    std::uint64_t duplicates{0};
    std::uint64_t heartbeats{0};
    std::uint64_t gaps{0};
    std::uint64_t lost_messages{0};
    std::uint64_t requests_sent{0};
    std::uint64_t malformed{0};
    std::uint64_t wrong_session{0};
    std::uint64_t unknown_types{0};
    bool end_of_session{false};
};

template <UpdateSink Sink>
class FeedHandler {
public:
    static constexpr std::size_t kBufferBytes = 65'536;

    FeedHandler(book::BookSet& books, SymbolTable& symbols, Sink& sink) noexcept
        : builder_{books, symbols, sink} {}

    FeedHandler(const FeedHandler&) = delete;
    FeedHandler& operator=(const FeedHandler&) = delete;

    void configure(std::uint64_t start_sequence, bool recover_gaps) noexcept {
        sequencer_ = Sequencer{start_sequence};
        recover_gaps_ = recover_gaps;
    }

    [[nodiscard]] Status open(const FeedConfig& config) noexcept {
        configure(config.start_sequence, config.has_recovery);
        request_batch_ = config.request_batch == 0 ? std::uint16_t{1} : config.request_batch;
        request_interval_ = config.request_interval_ticks;

        Status st = line_a_.open(config.line_a);
        if (st.ok() && config.has_line_b) {
            st = line_b_.open(config.line_b);
        }
        if (st.ok() && config.has_recovery) {
            st = recovery_.open(net::UdpSenderConfig{.destination = config.recovery});
        }
        return st;
    }

    std::uint32_t poll() noexcept {
        std::uint32_t handled = receive(line_a_, Source::Live);
        if (line_b_.valid()) {
            handled += receive(line_b_, Source::Live);
        }
        if (recovery_.valid()) {
            handled += receive(recovery_, Source::Recovery);
            request_missing(tsc_now());
        }
        return handled;
    }

    void on_packet(std::span<const std::byte> packet, Source source,
                   std::uint64_t rx_tsc) noexcept {
        const wire::mold::Header* hdr = wire::mold::parse_header(packet);
        if (hdr == nullptr) {
            stats_.malformed += 1;
            return;
        }
        stats_.packets += 1;

        const std::uint64_t sequence = hdr->sequence.get();
        const std::uint16_t count = hdr->count.get();
        Decision decision = sequencer_.inspect(hdr->session, sequence, count);
        if (decision.verdict == Verdict::WrongSession) {
            stats_.wrong_session += 1;
            return;
        }

        if (source == Source::Live) {
            const std::uint64_t end =
                sequence + (count == wire::mold::kEndOfSession ? std::uint64_t{0} : count);
            if (end > high_water_) {
                high_water_ = end;
            }
        }

        if (decision.verdict == Verdict::Gap) {
            if (source == Source::Recovery) {
                return;
            }
            if (recover_gaps_) {
                enter_recovery(rx_tsc);
                return;
            }
            stats_.gaps += 1;
            stats_.lost_messages += decision.missing;
            sequencer_.commit(sequence);
            raise_flag(kFlagLossy, rx_tsc);
            decision = sequencer_.inspect(hdr->session, sequence, count);
        }

        switch (decision.verdict) {
            case Verdict::Process:
                apply(packet, rx_tsc);
                request_due_ = recovering_;
                break;
            case Verdict::Duplicate:
                stats_.duplicates += 1;
                break;
            case Verdict::Heartbeat:
                stats_.heartbeats += 1;
                break;
            case Verdict::EndOfSession:
                stats_.end_of_session = true;
                break;
            case Verdict::Gap:
            case Verdict::WrongSession:
                break;
        }

        if (recovering_ && sequencer_.expected() >= high_water_) {
            recovering_ = false;
            request_due_ = false;
            builder_.set_feed_flags(static_cast<std::uint8_t>(builder_.feed_flags() & ~kFlagStale));
            builder_.publish_feed_status(sequencer_.expected(), rx_tsc);
        }
    }

    [[nodiscard]] bool recovering() const noexcept { return recovering_; }
    [[nodiscard]] std::uint64_t expected_sequence() const noexcept { return sequencer_.expected(); }
    [[nodiscard]] const FeedStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const BuilderStats& builder_stats() const noexcept { return builder_.stats(); }
    [[nodiscard]] net::EndpointResult line_a_endpoint() const noexcept {
        return line_a_.local_endpoint();
    }

private:
    template <typename Socket>
    [[nodiscard]] std::uint32_t receive(Socket& socket, Source source) noexcept {
        const net::IoResult io = socket.recv(buffer_);
        if (!io.ok() || io.bytes == 0) {
            return 0;
        }
        on_packet(std::span<const std::byte>{buffer_.data(), io.bytes}, source, tsc_now());
        return 1;
    }

    void apply(std::span<const std::byte> packet, std::uint64_t rx_tsc) noexcept {
        const std::uint64_t first_needed = sequencer_.expected();
        std::uint64_t applied_end = first_needed;

        const wire::Parse framing = wire::mold::for_each_message(
            packet, [&](std::uint64_t sequence, std::span<const std::byte> msg) noexcept {
                if (sequence < first_needed) {
                    return;
                }
                builder_.begin_message(sequence, rx_tsc);
                const wire::Parse parsed = wire::itch::dispatch(msg, builder_);
                if (parsed == wire::Parse::Unknown) {
                    stats_.unknown_types += 1;
                } else if (parsed != wire::Parse::Ok) {
                    stats_.malformed += 1;
                }
                applied_end = sequence + 1;
            });

        if (framing != wire::Parse::Ok) {
            stats_.malformed += 1;
        }
        sequencer_.commit(applied_end);
    }

    void enter_recovery(std::uint64_t rx_tsc) noexcept {
        if (recovering_) {
            return;
        }
        recovering_ = true;
        request_due_ = true;
        stats_.gaps += 1;
        raise_flag(kFlagStale, rx_tsc);
    }

    void raise_flag(std::uint8_t flag, std::uint64_t rx_tsc) noexcept {
        if ((builder_.feed_flags() & flag) != 0) {
            return;
        }
        builder_.set_feed_flags(static_cast<std::uint8_t>(builder_.feed_flags() | flag));
        builder_.publish_feed_status(sequencer_.expected(), rx_tsc);
    }

    void request_missing(std::uint64_t now) noexcept {
        if (!recovering_ || high_water_ <= sequencer_.expected()) {
            return;
        }
        if (!request_due_ && now - last_request_ < request_interval_) {
            return;
        }
        const std::uint64_t missing = high_water_ - sequencer_.expected();
        wire::mold::Request request{};
        request.session = sequencer_.session();
        request.sequence.set(sequencer_.expected());
        request.requested_count.set(
            missing < request_batch_ ? static_cast<std::uint16_t>(missing) : request_batch_);

        const auto bytes = std::as_bytes(std::span<const wire::mold::Request, 1>{&request, 1});
        if (recovery_.send(bytes).ok()) {
            stats_.requests_sent += 1;
        }
        last_request_ = now;
        request_due_ = false;
    }

    BookBuilder<Sink> builder_;
    Sequencer sequencer_{};
    net::UdpReceiver line_a_{};
    net::UdpReceiver line_b_{};
    net::UdpSender recovery_{};
    FeedStats stats_{};
    std::uint64_t high_water_{0};
    std::uint64_t last_request_{0};
    std::uint64_t request_interval_{3'000'000};
    std::uint16_t request_batch_{512};
    bool recover_gaps_{false};
    bool recovering_{false};
    bool request_due_{false};
    std::array<std::byte, kBufferBytes> buffer_{};
};

} // namespace hotpath::feed

#endif // HOTPATH_FEED_FEED_HANDLER_HPP
