#ifndef HOTPATH_NET_SOCKET_HPP
#define HOTPATH_NET_SOCKET_HPP

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <span>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/net/endpoint.hpp"

namespace hotpath::net {

struct IoResult {
    std::size_t bytes{0};
    Status status{};
    bool eof{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok() && !eof; }
    [[nodiscard]] bool would_block() const noexcept { return status.err() == EAGAIN; }
};

class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(int fd) noexcept : fd_{fd} {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : fd_{other.fd_} { other.fd_ = -1; }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    ~Socket() { close(); }

    [[nodiscard]] Status open(int type) noexcept {
        HOTPATH_ASSERT(fd_ < 0);
        const int fd = ::socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return Status::from_errno();
        }
        fd_ = fd;
        return Status{};
    }

    void close() noexcept {
        if (fd_ >= 0) {
            static_cast<void>(::close(fd_));
            fd_ = -1;
        }
    }

    [[nodiscard]] Status set_option(int level, int name, int value) noexcept {
        if (::setsockopt(fd_, level, name, &value, sizeof(value)) != 0) {
            return Status::from_errno();
        }
        return Status{};
    }

    [[nodiscard]] Status bind(const Endpoint& local) noexcept {
        const sockaddr_in sa = local.to_sockaddr();
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
            return Status::from_errno();
        }
        return Status{};
    }

    [[nodiscard]] EndpointResult local_endpoint() const noexcept {
        sockaddr_in sa{};
        socklen_t len = sizeof(sa);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
            return EndpointResult{{}, Status::from_errno()};
        }
        return EndpointResult{Endpoint::from_sockaddr(sa), Status{}};
    }

    [[nodiscard]] [[gnu::always_inline]] IoResult recv(std::span<std::byte> buffer) noexcept {
        const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (n > 0) [[likely]] {
            return IoResult{static_cast<std::size_t>(n), Status{}, false};
        }
        if (n == 0) {
            return IoResult{0, Status{}, !buffer.empty()};
        }
        return IoResult{0, io_error(), false};
    }

    [[nodiscard]] [[gnu::always_inline]] IoResult send(std::span<const std::byte> data) noexcept {
        const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n >= 0) [[likely]] {
            return IoResult{static_cast<std::size_t>(n), Status{}, false};
        }
        return IoResult{0, io_error(), false};
    }

    [[nodiscard]] IoResult recv_from(std::span<std::byte> buffer, Endpoint& from) noexcept {
        sockaddr_in sa{};
        socklen_t len = sizeof(sa);
        const ssize_t n = ::recvfrom(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr*>(&sa), &len);
        if (n < 0) {
            return IoResult{0, io_error(), false};
        }
        from = Endpoint::from_sockaddr(sa);
        return IoResult{static_cast<std::size_t>(n), Status{}, false};
    }

    [[nodiscard]] IoResult send_to(const Endpoint& to, std::span<const std::byte> data) noexcept {
        const sockaddr_in sa = to.to_sockaddr();
        const ssize_t n = ::sendto(fd_, data.data(), data.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                                   reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
        if (n < 0) {
            return IoResult{0, io_error(), false};
        }
        return IoResult{static_cast<std::size_t>(n), Status{}, false};
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    [[nodiscard]] static Status io_error() noexcept {
        const int err = errno;
        return Status{(err == EWOULDBLOCK || err == EINTR) ? EAGAIN : err};
    }

    int fd_{-1};
};

} // namespace hotpath::net

#endif // HOTPATH_NET_SOCKET_HPP
