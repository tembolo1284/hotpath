#ifndef HOTPATH_SIM_ITCH_PUBLISHER_HPP
#define HOTPATH_SIM_ITCH_PUBLISHER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/mold.hpp"

namespace hotpath::sim {

struct PublisherConfig {
    wire::Alpha<10> session{"HOTPATH001"};
    std::uint32_t store_capacity{0};
};

template <typename Sink>
class ItchPublisher {
public:
    static constexpr std::size_t kMaxMessageBytes = 63;

    explicit ItchPublisher(Sink& sink) noexcept : sink_{&sink}, writer_{buffer_} {}

    ItchPublisher(const ItchPublisher&) = delete;
    ItchPublisher& operator=(const ItchPublisher&) = delete;

    [[nodiscard]] Status init(mem::Arena& arena, const PublisherConfig& config) noexcept {
        HOTPATH_ASSERT(store_ == nullptr);
        if (config.store_capacity == 0) {
            return Status{EINVAL};
        }
        Stored* store = arena.allocate_array<Stored>(config.store_capacity);
        if (store == nullptr) {
            return Status{ENOMEM};
        }
        store_ = store;
        capacity_ = config.store_capacity;
        session_ = config.session;
        return Status{};
    }

    template <wire::WireMessage M>
    void publish(const M& msg) noexcept {
        static_assert(sizeof(M) <= kMaxMessageBytes);
        HOTPATH_HOT_ASSERT(store_ != nullptr);
        Stored& slot = store_[(next_sequence_ - 1U) % capacity_];
        slot.len = static_cast<std::uint8_t>(sizeof(M));
        std::memcpy(slot.bytes, &msg, sizeof(M));

        if (!open_) {
            writer_.begin(session_, next_sequence_);
            open_ = true;
        }
        if (!writer_.append(msg)) {
            flush();
            writer_.begin(session_, next_sequence_);
            open_ = true;
            const bool appended = writer_.append(msg);
            HOTPATH_HOT_ASSERT(appended);
            static_cast<void>(appended);
        }
        next_sequence_ += 1;
    }

    void flush() noexcept {
        if (!open_) {
            return;
        }
        open_ = false;
        if (writer_.count() == 0) {
            return;
        }
        static_cast<void>(sink_->send(writer_.finish()));
        packets_sent_ += 1;
    }

    void heartbeat() noexcept {
        flush();
        writer_.begin(session_, next_sequence_);
        static_cast<void>(sink_->send(writer_.finish()));
        packets_sent_ += 1;
    }

    [[nodiscard]] std::span<const std::byte> retransmit(std::uint64_t sequence, std::uint16_t count,
                                                        std::span<std::byte> out) const noexcept {
        if (out.size() < sizeof(wire::mold::Header) || sequence == 0 ||
            sequence >= next_sequence_ || next_sequence_ - sequence > capacity_) {
            return {};
        }
        wire::mold::PacketWriter writer{out};
        writer.begin(session_, sequence);
        for (std::uint16_t i = 0; i < count && sequence + i < next_sequence_; ++i) {
            const Stored& slot = store_[(sequence + i - 1U) % capacity_];
            const std::span<const std::byte> bytes{slot.bytes, slot.len};
            if (!writer.append(bytes)) {
                break;
            }
        }
        return writer.finish();
    }

    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }
    [[nodiscard]] std::uint64_t packets_sent() const noexcept { return packets_sent_; }
    [[nodiscard]] const wire::Alpha<10>& session() const noexcept { return session_; }

private:
    struct Stored {
        std::uint8_t len{0};
        std::byte bytes[kMaxMessageBytes]{};
    };

    Sink* sink_;
    std::array<std::byte, wire::mold::kMaxPacketBytes> buffer_{};
    wire::mold::PacketWriter writer_;
    Stored* store_{nullptr};
    std::uint32_t capacity_{0};
    wire::Alpha<10> session_{};
    std::uint64_t next_sequence_{1};
    std::uint64_t packets_sent_{0};
    bool open_{false};
};

} // namespace hotpath::sim

#endif // HOTPATH_SIM_ITCH_PUBLISHER_HPP
