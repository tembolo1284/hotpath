#include <gtest/gtest.h>

#include "hotpath/core/clock.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/sys/affinity.hpp"
#include "hotpath/sys/isolation.hpp"
#include "hotpath/sys/numa.hpp"

using namespace hotpath;

TEST(Core, QtyAndPriceArithmetic) {
    Qty q{10};
    q += Qty{5};
    q -= Qty{15};
    EXPECT_TRUE(q.is_zero());
    EXPECT_EQ(Price{105} - Price{100}, 5);
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
}

TEST(Core, TscScaleConvertsTicks) {
    if (!has_invariant_tsc()) {
        GTEST_SKIP() << "no invariant TSC";
    }
    const TscScale scale = calibrate_tsc(10'000'000ULL);
    EXPECT_GT(scale.ns_per_tick_q32, 0U);
    const std::uint64_t t0 = tsc_begin();
    const std::uint64_t t1 = tsc_end();
    EXPECT_LT(scale.to_ns(t1 - t0), 1'000'000U);
}

TEST(Sys, PinsToCurrentCpu) {
    const sys::CpuResult cpu = sys::current_cpu();
    ASSERT_TRUE(cpu.status.ok());
    ASSERT_TRUE(sys::pin_current_thread(cpu.cpu).ok());
    EXPECT_EQ(sys::current_cpu().cpu, cpu.cpu);
    EXPECT_TRUE(sys::current_node().status.ok());
}

TEST(Sys, IsolationCheckReads) {
    const sys::IsolationResult iso = sys::is_cpu_isolated(0);
    EXPECT_TRUE(iso.status.ok() || iso.status.err() == ENOENT);
}
