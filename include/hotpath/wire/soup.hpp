#ifndef HOTPATH_WIRE_SOUP_HPP
#define HOTPATH_WIRE_SOUP_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "hotpath/wire/fields.hpp"

namespace hotpath::wire::soup {

inline constexpr char kDebug = '+';
inline constexpr char kLoginAccepted = 'A';
inline constexpr char kLoginRejected = 'J';
inline constexpr char kSequencedData = 'S';
inline constexpr char kServerHeartbeat = 'H';
inline constexpr char kEndOfSession = 'Z';
inline constexpr char kLoginRequest = 'L';
inline constexpr char kUnsequencedData = 'U';
inline constexpr char kClientHeartbeat = 'R';
inline constexpr char kLogoutRequest = 'O';

inline constexpr char kRejectNotAuthorized = 'A';
inline constexpr char kRejectSessionNotAvailable = 'S';

struct Header {
    U16 length;
    char type;
};

struct LoginRequest {
    Header hdr;
    Alpha<6> username;
    Alpha<10> password;
    Alpha<10> requested_session;
    Alpha<20> requested_sequence;
};

struct LoginAccepted {
    Header hdr;
    Alpha<10> session;
    Alpha<20> sequence;
};

struct LoginRejected {
    Header hdr;
    char reason;
};

static_assert(WireMessage<Header> && sizeof(Header) == 3);
static_assert(WireMessage<LoginRequest> && sizeof(LoginRequest) == 49);
static_assert(WireMessage<LoginAccepted> && sizeof(LoginAccepted) == 33);
static_assert(WireMessage<LoginRejected> && sizeof(LoginRejected) == 4);

[[nodiscard]] constexpr Header make_header(char type, std::size_t payload_bytes) noexcept {
    Header hdr{};
    hdr.length.set(static_cast<std::uint16_t>(payload_bytes + 1U));
    hdr.type = type;
    return hdr;
}

template <WireMessage M>
[[nodiscard]] constexpr M make(char type) noexcept {
    M msg{};
    msg.hdr = make_header(type, sizeof(M) - sizeof(Header));
    return msg;
}

struct Packet {
    Parse result{Parse::Truncated};
    char type{0};
    std::span<const std::byte> payload{};
    std::size_t consumed{0};
};

[[nodiscard]] inline Packet next_packet(std::span<const std::byte> stream) noexcept {
    if (stream.size() < sizeof(Header)) {
        return Packet{};
    }
    const Header* hdr = reinterpret_cast<const Header*>(stream.data());
    const std::size_t length = hdr->length.get();
    if (length == 0) {
        return Packet{Parse::Malformed, 0, {}, 0};
    }
    const std::size_t total = sizeof(U16) + length;
    if (stream.size() < total) {
        return Packet{};
    }
    return Packet{Parse::Ok, hdr->type, stream.subspan(sizeof(Header), length - 1U), total};
}

[[nodiscard]] inline std::size_t write_packet(std::span<std::byte> out, char type,
                                              std::span<const std::byte> payload) noexcept {
    const std::size_t total = sizeof(Header) + payload.size();
    if (payload.size() > 0xFFFEU || out.size() < total) {
        return 0;
    }
    const Header hdr = make_header(type, payload.size());
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    if (!payload.empty()) {
        std::memcpy(out.data() + sizeof(hdr), payload.data(), payload.size());
    }
    return total;
}

} // namespace hotpath::wire::soup

#endif // HOTPATH_WIRE_SOUP_HPP
