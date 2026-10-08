#ifndef HOTPATH_NET_UDP_HPP
#define HOTPATH_NET_UDP_HPP

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "hotpath/core/status.hpp"
#include "hotpath/net/endpoint.hpp"
#include "hotpath/net/socket.hpp"

namespace hotpath::net {

struct UdpReceiverConfig {
    Endpoint listen{};
    std::uint32_t interface_be{0};
    int rcvbuf_bytes{0};
    int busy_poll_us{0};
};

class UdpReceiver {
public:
    [[nodiscard]] Status open(const UdpReceiverConfig& config) noexcept {
        Socket sock;
        Status st = sock.open(SOCK_DGRAM);
        if (st.ok()) {
            st = sock.set_option(SOL_SOCKET, SO_REUSEADDR, 1);
        }
        if (st.ok() && config.rcvbuf_bytes > 0) {
            st = sock.set_option(SOL_SOCKET, SO_RCVBUF, config.rcvbuf_bytes);
        }
        if (st.ok() && config.busy_poll_us > 0) {
            st = sock.set_option(SOL_SOCKET, SO_BUSY_POLL, config.busy_poll_us);
        }
        if (st.ok()) {
            st = sock.bind(config.listen);
        }
        if (st.ok() && config.listen.is_multicast()) {
            ip_mreq req{};
            req.imr_multiaddr.s_addr = config.listen.addr_be;
            req.imr_interface.s_addr = config.interface_be;
            if (::setsockopt(sock.fd(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &req, sizeof(req)) != 0) {
                st = Status::from_errno();
            }
        }
        if (st.ok()) {
            socket_ = static_cast<Socket&&>(sock);
        }
        return st;
    }

    [[nodiscard]] [[gnu::always_inline]] IoResult recv(std::span<std::byte> buffer) noexcept {
        return socket_.recv(buffer);
    }

    [[nodiscard]] EndpointResult local_endpoint() const noexcept {
        return socket_.local_endpoint();
    }

    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }

private:
    Socket socket_{};
};

struct UdpSenderConfig {
    Endpoint destination{};
    std::uint32_t interface_be{0};
    int multicast_ttl{1};
    bool multicast_loop{true};
};

class UdpSender {
public:
    [[nodiscard]] Status open(const UdpSenderConfig& config) noexcept {
        Socket sock;
        Status st = sock.open(SOCK_DGRAM);
        if (st.ok() && config.destination.is_multicast()) {
            st = sock.set_option(IPPROTO_IP, IP_MULTICAST_TTL, config.multicast_ttl);
            if (st.ok()) {
                st = sock.set_option(IPPROTO_IP, IP_MULTICAST_LOOP, config.multicast_loop ? 1 : 0);
            }
            if (st.ok() && config.interface_be != 0) {
                in_addr iface{};
                iface.s_addr = config.interface_be;
                if (::setsockopt(sock.fd(), IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) != 0) {
                    st = Status::from_errno();
                }
            }
        }
        if (st.ok()) {
            const sockaddr_in sa = config.destination.to_sockaddr();
            if (::connect(sock.fd(), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
                st = Status::from_errno();
            }
        }
        if (st.ok()) {
            socket_ = static_cast<Socket&&>(sock);
        }
        return st;
    }

    [[nodiscard]] [[gnu::always_inline]] IoResult send(std::span<const std::byte> data) noexcept {
        return socket_.send(data);
    }

    [[nodiscard]] [[gnu::always_inline]] IoResult recv(std::span<std::byte> buffer) noexcept {
        return socket_.recv(buffer);
    }

    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }

private:
    Socket socket_{};
};

class UdpServer {
public:
    [[nodiscard]] Status open(const Endpoint& listen) noexcept {
        Socket sock;
        Status st = sock.open(SOCK_DGRAM);
        if (st.ok()) {
            st = sock.set_option(SOL_SOCKET, SO_REUSEADDR, 1);
        }
        if (st.ok()) {
            st = sock.bind(listen);
        }
        if (st.ok()) {
            socket_ = static_cast<Socket&&>(sock);
        }
        return st;
    }

    [[nodiscard]] IoResult recv_from(std::span<std::byte> buffer, Endpoint& from) noexcept {
        return socket_.recv_from(buffer, from);
    }

    [[nodiscard]] IoResult send_to(const Endpoint& to, std::span<const std::byte> data) noexcept {
        return socket_.send_to(to, data);
    }

    [[nodiscard]] EndpointResult local_endpoint() const noexcept {
        return socket_.local_endpoint();
    }

    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }

private:
    Socket socket_{};
};

} // namespace hotpath::net

#endif // HOTPATH_NET_UDP_HPP
