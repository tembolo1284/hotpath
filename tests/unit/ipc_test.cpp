#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "hotpath/ipc/channel.hpp"
#include "hotpath/ipc/shm.hpp"
#include "hotpath/ipc/spin.hpp"
#include "hotpath/ipc/spsc_ring.hpp"

using namespace hotpath;

namespace {

struct Tick {
    std::uint64_t seq{0};
    std::uint64_t payload{0};
};

struct Wide {
    std::uint64_t words[4]{};
};

class RingTest : public ::testing::Test {
protected:
    static constexpr std::uint64_t kCapacity = 8;

    void SetUp() override {
        bytes_ = ipc::SpscRing<Tick>::bytes_for(kCapacity);
        mem_ = std::aligned_alloc(kCacheLine, bytes_);
        ASSERT_NE(mem_, nullptr);
    }

    void TearDown() override { std::free(mem_); }

    void* mem_{nullptr};
    std::size_t bytes_{0};
};

TEST_F(RingTest, FormatRejectsBadArguments) {
    using Ring = ipc::SpscRing<Tick>;
    EXPECT_EQ(Ring::format(mem_, bytes_, 6).status.err(), EINVAL);
    EXPECT_EQ(Ring::format(mem_, bytes_ - 1, kCapacity).status.err(), ENOMEM);
    EXPECT_EQ(Ring::format(static_cast<char*>(mem_) + 8, bytes_, kCapacity).status.err(), EINVAL);
}

TEST_F(RingTest, AttachValidatesHeader) {
    std::memset(mem_, 0, bytes_);
    EXPECT_EQ(ipc::SpscRing<Tick>::attach(mem_, bytes_).status.err(), EAGAIN);

    ASSERT_TRUE(ipc::SpscRing<Tick>::format(mem_, bytes_, kCapacity).status.ok());
    EXPECT_TRUE(ipc::SpscRing<Tick>::attach(mem_, bytes_).status.ok());
    EXPECT_EQ(ipc::SpscRing<Wide>::attach(mem_, bytes_).status.err(), EPROTO);
    EXPECT_EQ(ipc::SpscRing<Tick>::attach(mem_, bytes_ - 1).status.err(), EPROTO);
}

TEST_F(RingTest, FillsDrainsAndWraps) {
    const auto made = ipc::SpscRing<Tick>::format(mem_, bytes_, kCapacity);
    ASSERT_TRUE(made.status.ok());
    ipc::SpscProducer<Tick> tx{*made.ring};
    ipc::SpscConsumer<Tick> rx{*made.ring};

    Tick out{};
    EXPECT_FALSE(rx.try_pop(out));

    std::uint64_t next_in = 0;
    std::uint64_t next_out = 0;
    for (int round = 0; round < 5; ++round) {
        for (std::uint64_t i = 0; i < kCapacity; ++i) {
            ASSERT_TRUE(tx.try_push(Tick{next_in, next_in * 3}));
            ++next_in;
        }
        EXPECT_FALSE(tx.try_push(Tick{}));
        EXPECT_EQ(made.ring->size_approx(), kCapacity);
        for (std::uint64_t i = 0; i < kCapacity - 3; ++i) {
            ASSERT_TRUE(rx.try_pop(out));
            EXPECT_EQ(out.seq, next_out);
            EXPECT_EQ(out.payload, next_out * 3);
            ++next_out;
        }
        for (std::uint64_t i = 0; i < kCapacity - 3; ++i) {
            ASSERT_TRUE(tx.try_push(Tick{next_in, next_in * 3}));
            ++next_in;
        }
        while (rx.try_pop(out)) {
            EXPECT_EQ(out.seq, next_out);
            ++next_out;
        }
        EXPECT_EQ(next_out, next_in);
    }
}

TEST_F(RingTest, ZeroCopyClaimAndPeek) {
    const auto made = ipc::SpscRing<Tick>::format(mem_, bytes_, kCapacity);
    ASSERT_TRUE(made.status.ok());
    ipc::SpscProducer<Tick> tx{*made.ring};
    ipc::SpscConsumer<Tick> rx{*made.ring};

    Tick* slot = tx.claim();
    ASSERT_NE(slot, nullptr);
    slot->seq = 7;
    EXPECT_EQ(rx.peek(), nullptr);
    tx.publish();

    const Tick* seen = rx.peek();
    ASSERT_NE(seen, nullptr);
    EXPECT_EQ(seen, slot);
    EXPECT_EQ(seen->seq, 7U);
    rx.consume();
    EXPECT_EQ(rx.peek(), nullptr);
}

TEST(RingStress, PreservesOrderAcrossThreads) {
    constexpr std::uint64_t kCapacity = 1024;
    constexpr std::uint64_t kCount = 2'000'000;

    const std::size_t bytes = ipc::SpscRing<Tick>::bytes_for(kCapacity);
    void* mem = std::aligned_alloc(kCacheLine, bytes);
    ASSERT_NE(mem, nullptr);
    const auto made = ipc::SpscRing<Tick>::format(mem, bytes, kCapacity);
    ASSERT_TRUE(made.status.ok());

    std::uint64_t bad = 0;
    std::thread consumer{[&] {
        ipc::SpscConsumer<Tick> rx{*made.ring};
        for (std::uint64_t want = 0; want < kCount;) {
            const Tick* tick = rx.peek();
            if (tick == nullptr) {
                ipc::cpu_relax();
                continue;
            }
            bad += (tick->seq != want || tick->payload != ~want) ? 1U : 0U;
            rx.consume();
            ++want;
        }
    }};

    ipc::SpscProducer<Tick> tx{*made.ring};
    for (std::uint64_t seq = 0; seq < kCount;) {
        if (tx.try_push(Tick{seq, ~seq})) {
            ++seq;
        } else {
            ipc::cpu_relax();
        }
    }
    consumer.join();

    EXPECT_EQ(bad, 0U);
    EXPECT_EQ(made.ring->size_approx(), 0U);
    std::free(mem);
}

TEST(Shm, CreateIsExclusiveAndOpenSeesData) {
    char name[64];
    std::snprintf(name, sizeof(name), "/hotpath-shm-test-%d", static_cast<int>(::getpid()));
    static_cast<void>(ipc::ShmSegment::unlink(name));

    ipc::ShmSegment a;
    ASSERT_TRUE(a.create(name, 100).ok());
    EXPECT_EQ(a.size(), ipc::kShmPageBytes);
    ASSERT_TRUE(a.populate().ok());

    ipc::ShmSegment dup;
    EXPECT_EQ(dup.create(name, 100).err(), EEXIST);

    static_cast<char*>(a.data())[10] = 'x';
    ipc::ShmSegment b;
    ASSERT_TRUE(b.open(name).ok());
    EXPECT_EQ(b.size(), a.size());
    EXPECT_EQ(static_cast<char*>(b.data())[10], 'x');

    EXPECT_TRUE(ipc::ShmSegment::unlink(name).ok());
    ipc::ShmSegment gone;
    EXPECT_EQ(gone.open(name).err(), ENOENT);
}

int run_child_consumer(const char* name, std::uint64_t count) {
    ipc::Channel<Tick> channel;
    const bool opened = ipc::spin_until(
        [&] {
            if (channel.open(name).ok()) {
                return true;
            }
            static_cast<void>(::usleep(1000));
            return false;
        },
        5000);
    if (!opened) {
        return 2;
    }

    ipc::SpscConsumer<Tick> rx = channel.consumer();
    std::uint64_t want = 0;
    const bool done = ipc::spin_until(
        [&] {
            const Tick* tick = rx.peek();
            if (tick != nullptr) {
                if (tick->seq != want || tick->payload != want * 7) {
                    ::_exit(3);
                }
                rx.consume();
                ++want;
            }
            return want == count;
        },
        1ULL << 34);
    return done ? 0 : 4;
}

TEST(Channel, CrossProcessDelivery) {
    constexpr std::uint64_t kCount = 500'000;
    char name[64];
    std::snprintf(name, sizeof(name), "/hotpath-chan-test-%d", static_cast<int>(::getpid()));
    static_cast<void>(ipc::Channel<Tick>::unlink(name));

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::_exit(run_child_consumer(name, kCount));
    }

    ipc::Channel<Tick> channel;
    ASSERT_TRUE(channel.create(name, 4096).ok());
    EXPECT_EQ(channel.capacity(), 4096U);

    ipc::SpscProducer<Tick> tx = channel.producer();
    for (std::uint64_t seq = 0; seq < kCount;) {
        if (tx.try_push(Tick{seq, seq * 7})) {
            ++seq;
        } else {
            ipc::cpu_relax();
        }
    }

    int wstatus = 0;
    ASSERT_EQ(::waitpid(child, &wstatus, 0), child);
    EXPECT_TRUE(WIFEXITED(wstatus));
    EXPECT_EQ(WEXITSTATUS(wstatus), 0);
    EXPECT_TRUE(ipc::Channel<Tick>::unlink(name).ok());
}

} // namespace
