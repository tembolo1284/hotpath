#ifndef HOTPATH_TESTS_SUPPORT_FAKES_HPP
#define HOTPATH_TESTS_SUPPORT_FAKES_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <span>
#include <vector>

#include "hotpath/core/types.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/net/socket.hpp"
#include "hotpath/wire/fields.hpp"
#include "hotpath/wire/soup.hpp"

namespace hotpath::test {

struct FakeClock {
    static inline std::uint64_t value = 0;
    [[nodiscard]] static std::uint64_t now() noexcept { return value; }
};

template <typename T>
struct VectorSink {
    std::vector<T> items;
    T scratch{};
    bool full{false};

    T* claim() { return full ? nullptr : &scratch; }
    void publish() { items.push_back(scratch); }
};

template <typename T>
struct QueueSource {
    std::deque<T> items;

    const T* peek() { return items.empty() ? nullptr : &items.front(); }
    void consume() { items.pop_front(); }
};

template <typename T>
class HeapRing {
public:
    explicit HeapRing(std::uint64_t capacity) {
        const std::size_t bytes = ipc::SpscRing<T>::bytes_for(capacity);
        mem_ = std::aligned_alloc(kCacheLine, bytes);
        ring_ = ipc::SpscRing<T>::format(mem_, bytes, capacity).ring;
    }
    HeapRing(const HeapRing&) = delete;
    HeapRing& operator=(const HeapRing&) = delete;
    ~HeapRing() { std::free(mem_); }

    [[nodiscard]] ipc::SpscRing<T>& ring() { return *ring_; }

private:
    void* mem_{nullptr};
    ipc::SpscRing<T>* ring_{nullptr};
};

struct FakeTransport {
    std::vector<std::byte> sent;
    std::deque<std::byte> inbound;
    std::size_t send_limit{~std::size_t{0}};
    std::size_t recv_limit{~std::size_t{0}};
    bool send_blocked{false};
    bool closed{false};
    int send_error{0};

    net::IoResult send(std::span<const std::byte> data) {
        if (send_error != 0) {
            return net::IoResult{0, Status{send_error}, false};
        }
        if (send_blocked) {
            return net::IoResult{0, Status{EAGAIN}, false};
        }
        const std::size_t n = data.size() < send_limit ? data.size() : send_limit;
        sent.insert(sent.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
        return net::IoResult{n, Status{}, false};
    }

    net::IoResult recv(std::span<std::byte> buffer) {
        if (inbound.empty()) {
            return closed ? net::IoResult{0, Status{}, true}
                          : net::IoResult{0, Status{EAGAIN}, false};
        }
        std::size_t n = buffer.size() < inbound.size() ? buffer.size() : inbound.size();
        n = n < recv_limit ? n : recv_limit;
        for (std::size_t i = 0; i < n; ++i) {
            buffer[i] = inbound.front();
            inbound.pop_front();
        }
        return net::IoResult{n, Status{}, false};
    }

    void feed(std::span<const std::byte> data) { inbound.insert(inbound.end(), data.begin(), data.end()); }

    template <wire::WireMessage M>
    void feed_packet(char type, const M& msg) {
        const wire::soup::Header hdr = wire::soup::make_header(type, sizeof(M));
        feed(std::as_bytes(std::span<const wire::soup::Header, 1>{&hdr, 1}));
        feed(std::as_bytes(std::span<const M, 1>{&msg, 1}));
    }

    void feed_empty(char type) {
        const wire::soup::Header hdr = wire::soup::make_header(type, 0);
        feed(std::as_bytes(std::span<const wire::soup::Header, 1>{&hdr, 1}));
    }

    void accept_login(const char* session, std::uint64_t next_sequence) {
        auto accepted = wire::soup::make<wire::soup::LoginAccepted>(wire::soup::kLoginAccepted);
        accepted.session = wire::Alpha<10>::right_justified(session);
        accepted.sequence = wire::format_numeric<20>(next_sequence);
        feed(std::as_bytes(std::span<const wire::soup::LoginAccepted, 1>{&accepted, 1}));
    }

    template <wire::WireMessage M>
    [[nodiscard]] M sent_at(std::size_t offset) const {
        M out{};
        std::memcpy(&out, sent.data() + offset, sizeof(M));
        return out;
    }
};

} // namespace hotpath::test

#endif // HOTPATH_TESTS_SUPPORT_FAKES_HPP
