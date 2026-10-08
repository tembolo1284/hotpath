#include <gtest/gtest.h>

#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include "hotpath/ipc/spin.hpp"
#include "hotpath/net/endpoint.hpp"
#include "hotpath/net/socket.hpp"
#include "hotpath/net/tcp.hpp"
#include "hotpath/net/udp.hpp"

using namespace hotpath;

namespace {

constexpr std::uint64_t kSpins = 2'000'000;

std::span<const std::byte> text(const char* s) {
    return std::as_bytes(std::span<const char>{s, std::strlen(s)});
}

template <typename S>
net::IoResult recv_spin(S& source, std::span<std::byte> buffer) {
    net::IoResult io{};
    static_cast<void>(ipc::spin_until(
        [&] {
            io = source.recv(buffer);
            if (io.would_block()) {
                static_cast<void>(::usleep(10));
                return false;
            }
            return true;
        },
        kSpins));
    return io;
}

TEST(Endpoint, ParsesAndClassifies) {
    const net::EndpointResult lo = net::make_endpoint("127.0.0.1", 9000);
    ASSERT_TRUE(lo.status.ok());
    EXPECT_FALSE(lo.endpoint.is_multicast());
    EXPECT_EQ(net::Endpoint::from_sockaddr(lo.endpoint.to_sockaddr()), lo.endpoint);

    EXPECT_TRUE(net::make_endpoint("239.1.1.1", 1).endpoint.is_multicast());
    EXPECT_EQ(net::make_endpoint("not-an-ip", 1).status.err(), EINVAL);
    EXPECT_EQ(net::make_endpoint(nullptr, 1).status.err(), EINVAL);
}

TEST(Udp, LoopbackDatagramsArriveWhole) {
    net::UdpReceiver rx;
    ASSERT_TRUE(rx.open({.listen = net::make_endpoint("127.0.0.1", 0).endpoint}).ok());
    const net::EndpointResult bound = rx.local_endpoint();
    ASSERT_TRUE(bound.status.ok());
    ASSERT_NE(bound.endpoint.port, 0);

    std::array<std::byte, 64> buffer{};
    EXPECT_TRUE(rx.recv(buffer).would_block());

    net::UdpSender tx;
    ASSERT_TRUE(tx.open({.destination = bound.endpoint}).ok());
    ASSERT_EQ(tx.send(text("tick-1")).bytes, 6U);
    ASSERT_EQ(tx.send(text("tick-22")).bytes, 7U);

    net::IoResult io = recv_spin(rx, buffer);
    ASSERT_TRUE(io.ok());
    EXPECT_EQ(io.bytes, 6U);
    EXPECT_EQ(std::memcmp(buffer.data(), "tick-1", 6), 0);

    io = recv_spin(rx, buffer);
    ASSERT_TRUE(io.ok());
    EXPECT_EQ(io.bytes, 7U);
}

TEST(Udp, MulticastOverLoopback) {
    const net::Endpoint group = net::make_endpoint("239.255.42.99", 0).endpoint;
    const std::uint32_t lo = net::make_endpoint("127.0.0.1", 0).endpoint.addr_be;

    net::UdpReceiver rx;
    const Status joined = rx.open({.listen = group, .interface_be = lo});
    if (!joined.ok()) {
        GTEST_SKIP() << "multicast join unavailable, errno " << joined.err();
    }
    const net::EndpointResult bound = rx.local_endpoint();
    ASSERT_TRUE(bound.status.ok());

    net::UdpSender tx;
    const Status opened = tx.open(
        {.destination = {group.addr_be, bound.endpoint.port}, .interface_be = lo});
    if (!opened.ok()) {
        GTEST_SKIP() << "multicast send unavailable, errno " << opened.err();
    }
    const net::IoResult sent = tx.send(text("mcast"));
    if (!sent.ok()) {
        GTEST_SKIP() << "multicast send failed, errno " << sent.status.err();
    }

    std::array<std::byte, 64> buffer{};
    const net::IoResult io = recv_spin(rx, buffer);
    ASSERT_TRUE(io.ok());
    EXPECT_EQ(io.bytes, 5U);
}

TEST(Tcp, LoopbackStreamAndOrderlyClose) {
    net::TcpListener listener;
    ASSERT_TRUE(listener.listen(net::make_endpoint("127.0.0.1", 0).endpoint, 4).ok());
    const net::EndpointResult bound = listener.local_endpoint();
    ASSERT_TRUE(bound.status.ok());

    net::TcpStream server;
    EXPECT_EQ(listener.accept(server).err(), EAGAIN);

    net::TcpStream client;
    ASSERT_TRUE(client.connect(bound.endpoint, 1000).ok());

    ASSERT_TRUE(ipc::spin_until(
        [&] {
            if (listener.accept(server).ok()) {
                return true;
            }
            static_cast<void>(::usleep(10));
            return false;
        },
        kSpins));

    std::array<std::byte, 64> buffer{};
    EXPECT_TRUE(server.recv(buffer).would_block());

    ASSERT_EQ(client.send(text("enter-order")).bytes, 11U);
    net::IoResult io = recv_spin(server, buffer);
    ASSERT_TRUE(io.ok());
    EXPECT_EQ(io.bytes, 11U);
    EXPECT_EQ(std::memcmp(buffer.data(), "enter-order", 11), 0);

    ASSERT_EQ(server.send(text("accepted")).bytes, 8U);
    io = recv_spin(client, buffer);
    ASSERT_TRUE(io.ok());
    EXPECT_EQ(io.bytes, 8U);

    client.close();
    io = recv_spin(server, buffer);
    EXPECT_TRUE(io.eof);
    EXPECT_FALSE(io.ok());
}

TEST(Tcp, ConnectToClosedPortFails) {
    net::TcpListener listener;
    ASSERT_TRUE(listener.listen(net::make_endpoint("127.0.0.1", 0).endpoint, 1).ok());
    const net::Endpoint dead = listener.local_endpoint().endpoint;
    listener = net::TcpListener{};

    net::TcpStream client;
    EXPECT_EQ(client.connect(dead, 1000).err(), ECONNREFUSED);
    EXPECT_FALSE(client.valid());
}

} // namespace
