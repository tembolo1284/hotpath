#ifndef HOTPATH_NET_ENDPOINT_HPP
#define HOTPATH_NET_ENDPOINT_HPP

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>

#include "hotpath/core/status.hpp"

namespace hotpath::net {

struct Endpoint {
    std::uint32_t addr_be{0};
    std::uint16_t port{0};

    [[nodiscard]] bool is_multicast() const noexcept {
        return (ntohl(addr_be) >> 28) == 0xEU;
    }

    [[nodiscard]] sockaddr_in to_sockaddr() const noexcept {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = addr_be;
        return sa;
    }

    [[nodiscard]] static Endpoint from_sockaddr(const sockaddr_in& sa) noexcept {
        return Endpoint{sa.sin_addr.s_addr, ntohs(sa.sin_port)};
    }

    constexpr bool operator==(const Endpoint&) const noexcept = default;
};

struct EndpointResult {
    Endpoint endpoint{};
    Status status{};
};

[[nodiscard]] inline EndpointResult make_endpoint(const char* ipv4, std::uint16_t port) noexcept {
    if (ipv4 == nullptr) {
        return EndpointResult{{}, Status{EINVAL}};
    }
    in_addr addr{};
    if (::inet_pton(AF_INET, ipv4, &addr) != 1) {
        return EndpointResult{{}, Status{EINVAL}};
    }
    return EndpointResult{Endpoint{addr.s_addr, port}, Status{}};
}

} // namespace hotpath::net

#endif // HOTPATH_NET_ENDPOINT_HPP
