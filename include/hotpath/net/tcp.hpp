#ifndef HOTPATH_NET_TCP_HPP
#define HOTPATH_NET_TCP_HPP

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <cstddef>
#include <span>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/net/endpoint.hpp"
#include "hotpath/net/socket.hpp"

namespace hotpath::net {

class TcpStream {
public:
    TcpStream() noexcept = default;
    explicit TcpStream(Socket&& socket) noexcept : socket_{static_cast<Socket&&>(socket)} {}

    [[nodiscard]] Status connect(const Endpoint& remote, int timeout_ms) noexcept {
        HOTPATH_ASSERT(!socket_.valid());
        Socket sock;
        Status st = sock.open(SOCK_STREAM);
        if (st.ok()) {
            st = sock.set_option(IPPROTO_TCP, TCP_NODELAY, 1);
        }
        if (!st.ok()) {
            return st;
        }

        const sockaddr_in sa = remote.to_sockaddr();
        if (::connect(sock.fd(), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
            if (errno != EINPROGRESS) {
                return Status::from_errno();
            }
            pollfd pfd{sock.fd(), POLLOUT, 0};
            const int ready = ::poll(&pfd, 1, timeout_ms);
            if (ready < 0) {
                return Status::from_errno();
            }
            if (ready == 0) {
                return Status{ETIMEDOUT};
            }
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(sock.fd(), SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
                return Status::from_errno();
            }
            if (err != 0) {
                return Status{err};
            }
        }
        socket_ = static_cast<Socket&&>(sock);
        return Status{};
    }

    [[nodiscard]] [[gnu::always_inline]] IoResult send(std::span<const std::byte> data) noexcept {
        return socket_.send(data);
    }

    [[nodiscard]] [[gnu::always_inline]] IoResult recv(std::span<std::byte> buffer) noexcept {
        return socket_.recv(buffer);
    }

    void close() noexcept { socket_.close(); }

    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }

private:
    Socket socket_{};
};

class TcpListener {
public:
    [[nodiscard]] Status listen(const Endpoint& local, int backlog) noexcept {
        Socket sock;
        Status st = sock.open(SOCK_STREAM);
        if (st.ok()) {
            st = sock.set_option(SOL_SOCKET, SO_REUSEADDR, 1);
        }
        if (st.ok()) {
            st = sock.bind(local);
        }
        if (st.ok() && ::listen(sock.fd(), backlog) != 0) {
            st = Status::from_errno();
        }
        if (st.ok()) {
            socket_ = static_cast<Socket&&>(sock);
        }
        return st;
    }

    [[nodiscard]] Status accept(TcpStream& out) noexcept {
        const int fd = ::accept4(socket_.fd(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            const int err = errno;
            return Status{(err == EWOULDBLOCK || err == EINTR) ? EAGAIN : err};
        }
        Socket sock{fd};
        const Status st = sock.set_option(IPPROTO_TCP, TCP_NODELAY, 1);
        if (!st.ok()) {
            return st;
        }
        out = TcpStream{static_cast<Socket&&>(sock)};
        return Status{};
    }

    [[nodiscard]] EndpointResult local_endpoint() const noexcept {
        return socket_.local_endpoint();
    }

    [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }

private:
    Socket socket_{};
};

} // namespace hotpath::net

#endif // HOTPATH_NET_TCP_HPP
