#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "hotpath/core/clock.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/ipc/spsc_ring.hpp"
#include "hotpath/perf/histogram.hpp"
#include "hotpath/sys/affinity.hpp"
#include "support/bench.hpp"

using namespace hotpath;

namespace {

struct alignas(kCacheLine) Message {
    std::uint64_t stamp{0};
    std::uint64_t sequence{0};
    std::uint64_t payload[6]{};
};

static_assert(sizeof(Message) == kCacheLine);

class Ring {
public:
    explicit Ring(std::uint64_t capacity) {
        const std::size_t bytes = ipc::SpscRing<Message>::bytes_for(capacity);
        mem_ = std::aligned_alloc(kCacheLine, bytes);
        if (mem_ == nullptr) {
            bench::die("out of memory");
        }
        const auto formatted = ipc::SpscRing<Message>::format(mem_, bytes, capacity);
        bench::require(formatted.status, "ring format");
        ring_ = formatted.ring;
    }
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;
    ~Ring() { std::free(mem_); }

    [[nodiscard]] ipc::SpscRing<Message>& get() noexcept { return *ring_; }

private:
    void* mem_{nullptr};
    ipc::SpscRing<Message>* ring_{nullptr};
};

void same_thread(bench::Session& session) {
    Ring ring{1U << 10};
    ipc::SpscProducer<Message> tx{ring.get()};
    ipc::SpscConsumer<Message> rx{ring.get()};

    session.run("push+pop", session.ops(2'000'000), [] {}, [&](std::uint64_t i) {
        Message* slot = tx.claim();
        slot->sequence = i;
        tx.publish();
        const Message* seen = rx.peek();
        bench::keep(seen->sequence);
        rx.consume();
    });

    session.run("burst 16", session.ops(200'000), [] {}, [&](std::uint64_t i) {
        for (unsigned k = 0; k < 16; ++k) {
            Message* slot = tx.claim();
            slot->sequence = i + k;
            tx.publish();
        }
        for (unsigned k = 0; k < 16; ++k) {
            bench::keep(rx.peek()->sequence);
            rx.consume();
        }
    });
}

void cross_core(bench::Session& session, std::uint64_t count, std::uint64_t gap_ticks,
                const char* name) {
    if (!session.selected(name)) {
        return;
    }
    Ring ring{1U << 16};
    ipc::SpscProducer<Message> tx{ring.get()};
    ipc::SpscConsumer<Message> rx{ring.get()};
    std::atomic<bool> ready{false};
    std::atomic<bool> start{false};

    std::thread producer{[&] {
        bench::require(sys::pin_current_thread(static_cast<unsigned>(session.cpu2())),
                       "cannot pin --cpu2");
        ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
        }
        std::uint64_t next = tsc_now();
        for (std::uint64_t i = 0; i < count; ++i) {
            while (gap_ticks != 0 && tsc_now() < next) {
            }
            next += gap_ticks;
            Message* slot = tx.claim();
            while (slot == nullptr) {
                slot = tx.claim();
            }
            slot->sequence = i;
            slot->stamp = tsc_now();
            tx.publish();
        }
    }};

    while (!ready.load(std::memory_order_acquire)) {
    }
    perf::Histogram hop;
    const std::uint64_t warmup = count / 10;
    start.store(true, std::memory_order_release);
    const std::uint64_t t0 = tsc_now();
    for (std::uint64_t received = 0; received < count;) {
        const Message* seen = rx.peek();
        if (seen == nullptr) {
            continue;
        }
        const std::uint64_t now = tsc_now();
        if (gap_ticks != 0 && received >= warmup) {
            hop.record(now >= seen->stamp ? now - seen->stamp : 0);
        }
        rx.consume();
        received += 1;
    }
    const std::uint64_t t1 = tsc_now();
    producer.join();

    if (gap_ticks != 0) {
        session.report(name, hop);
        return;
    }
    const std::uint64_t ns = session.scale().to_ns(t1 - t0);
    std::printf("%-18s n=%-9llu throughput=%llu msgs/s\n", name,
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(count * 1'000'000'000ULL / (ns == 0 ? 1 : ns)));
}

} // namespace

int main(int argc, char** argv) {
    bench::Session session{argc, argv, "ring_bench: SPSC ring, 64-byte messages"};
    same_thread(session);

    if (session.cpu2() == bench::Session::kNoCpu || session.cpu2() == session.cpu()) {
        session.note("cross-core cases skipped (pass --cpu=N --cpu2=M on different cores)");
        return 0;
    }
    const std::uint64_t gap = session.ticks_per_second() / 200'000U;
    cross_core(session, session.ops(500'000), gap, "hop, paced 5us");
    cross_core(session, session.ops(5'000'000), 0, "hop, saturated");
    return 0;
}
