#include <gtest/gtest.h>

#include <cstdint>

#include "hotpath/mem/arena.hpp"
#include "hotpath/mem/pool.hpp"
#include "hotpath/mem/region.hpp"

using namespace hotpath;

namespace {

struct Node {
    std::uint64_t id{0};
    std::uint32_t qty{0};
    mem::PoolIndex next{mem::kNilIndex};
};

class MemTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(region_.map(1U << 20, sys::PageKind::Small).ok());
        region_.prefault();
    }

    mem::Region region_;
};

TEST_F(MemTest, ArenaAlignsAndExhausts) {
    mem::Arena arena{region_};
    ASSERT_NE(arena.allocate(3, 1), nullptr);
    auto* words = arena.allocate_array<std::uint64_t>(4);
    ASSERT_NE(words, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(words) % alignof(std::uint64_t), 0U);
    EXPECT_EQ(words[3], 0U);
    EXPECT_EQ(arena.allocate(arena.remaining() + 1, 1), nullptr);
    arena.freeze();
    EXPECT_TRUE(arena.frozen());
}

TEST_F(MemTest, PoolRecyclesLifo) {
    mem::Arena arena{region_};
    mem::Pool<Node> pool;
    ASSERT_TRUE(pool.init(arena, 1024).ok());

    const mem::PoolIndex a = pool.acquire();
    const mem::PoolIndex b = pool.acquire();
    EXPECT_EQ(a, 0U);
    EXPECT_EQ(b, 1U);
    EXPECT_EQ(pool[a].next, mem::kNilIndex);

    pool.release(a);
    EXPECT_EQ(pool.acquire(), a);

    for (mem::PoolIndex i = 2; i < 1024; ++i) {
        ASSERT_NE(pool.acquire(), mem::kNilIndex);
    }
    EXPECT_EQ(pool.acquire(), mem::kNilIndex);
    EXPECT_EQ(pool.in_use(), 1024U);
}

TEST_F(MemTest, PoolRejectsOversizedCapacity) {
    mem::Arena arena{region_};
    mem::Pool<Node> pool;
    EXPECT_EQ(pool.init(arena, 1U << 20).err(), ENOMEM);
}

TEST_F(MemTest, RegionMoveTransfersOwnership) {
    mem::Region moved{std::move(region_)};
    EXPECT_TRUE(moved.mapped());
    EXPECT_TRUE(moved.prefaulted());
    EXPECT_FALSE(region_.mapped());
}

} // namespace
