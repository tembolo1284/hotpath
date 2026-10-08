#ifndef HOTPATH_WIRE_MOLD_HPP
#define HOTPATH_WIRE_MOLD_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "hotpath/core/contract.hpp"
#include "hotpath/wire/fields.hpp"

namespace hotpath::wire::mold {

inline constexpr std::uint16_t kHeartbeat = 0;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;
inline constexpr std::size_t kMaxPacketBytes = 1400;

struct Header {
    Alpha<10> session;
    U64 sequence;
    U16 count;
};

struct Request {
    Alpha<10> session;
    U64 sequence;
    U16 requested_count;
};

static_assert(WireMessage<Header> && sizeof(Header) == 20);
static_assert(WireMessage<Request> && sizeof(Request) == 20);

[[nodiscard]] inline const Header* parse_header(std::span<const std::byte> packet) noexcept {
    if (packet.size() < sizeof(Header)) {
        return nullptr;
    }
    return reinterpret_cast<const Header*>(packet.data());
}

template <typename F>
[[nodiscard]] inline Parse for_each_message(std::span<const std::byte> packet, F&& fn) noexcept {
    const Header* hdr = parse_header(packet);
    if (hdr == nullptr) {
        return Parse::Truncated;
    }
    const std::uint16_t count = hdr->count.get();
    if (count == kEndOfSession) {
        return Parse::Ok;
    }

    std::uint64_t sequence = hdr->sequence.get();
    std::size_t offset = sizeof(Header);
    for (std::uint16_t i = 0; i < count; ++i) {
        if (packet.size() - offset < sizeof(U16)) {
            return Parse::Truncated;
        }
        const std::size_t len = reinterpret_cast<const U16*>(packet.data() + offset)->get();
        offset += sizeof(U16);
        if (packet.size() - offset < len) {
            return Parse::Truncated;
        }
        fn(sequence, packet.subspan(offset, len));
        offset += len;
        sequence += 1;
    }
    return Parse::Ok;
}

class PacketWriter {
public:
    explicit PacketWriter(std::span<std::byte> buffer) noexcept : buffer_{buffer} {
        HOTPATH_ASSERT(buffer.size() >= sizeof(Header));
    }

    void begin(const Alpha<10>& session, std::uint64_t first_sequence) noexcept {
        Header hdr{};
        hdr.session = session;
        hdr.sequence.set(first_sequence);
        std::memcpy(buffer_.data(), &hdr, sizeof(hdr));
        used_ = sizeof(Header);
        count_ = 0;
    }

    [[nodiscard]] bool append(std::span<const std::byte> msg) noexcept {
        HOTPATH_HOT_ASSERT(used_ >= sizeof(Header));
        const std::size_t need = sizeof(U16) + msg.size();
        if (msg.size() > 0xFFFFU || need > buffer_.size() - used_ || count_ == kEndOfSession - 1) {
            return false;
        }
        const U16 len{static_cast<std::uint16_t>(msg.size())};
        std::memcpy(buffer_.data() + used_, &len, sizeof(len));
        std::memcpy(buffer_.data() + used_ + sizeof(len), msg.data(), msg.size());
        used_ += need;
        count_ = static_cast<std::uint16_t>(count_ + 1U);
        return true;
    }

    template <WireMessage M>
    [[nodiscard]] bool append(const M& msg) noexcept {
        return append(std::as_bytes(std::span<const M, 1>{&msg, 1}));
    }

    [[nodiscard]] std::span<const std::byte> finish() noexcept {
        HOTPATH_HOT_ASSERT(used_ >= sizeof(Header));
        const U16 count{count_};
        std::memcpy(buffer_.data() + offsetof(Header, count), &count, sizeof(count));
        return buffer_.first(used_);
    }

    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }

private:
    std::span<std::byte> buffer_;
    std::size_t used_{0};
    std::uint16_t count_{0};
};

} // namespace hotpath::wire::mold

#endif // HOTPATH_WIRE_MOLD_HPP
