#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <random>

#include "hotpath/core/clock.hpp"
#include "hotpath/perf/counters.hpp"
#include "hotpath/perf/histogram.hpp"
#include "hotpath/perf/report.hpp"
#include "hotpath/perf/timer.hpp"
#include "support/fakes.hpp"

using namespace hotpath;
using perf::Histogram;

namespace {

TEST(Histogram, BucketsBoundEveryValueWithinThreePercent) {
    std::mt19937_64 rng{3};
    for (int i = 0; i < 200'000; ++i) {
        const unsigned bits = 1U + static_cast<unsigned>(rng() % Histogram::kMaxBits);
        const std::uint64_t value = rng() & ((std::uint64_t{1} << bits) - 1U);
        const std::size_t index = Histogram::index_of(value);
        ASSERT_LT(index, Histogram::kBuckets);
        const std::uint64_t lo = Histogram::lower_bound(index);
        const std::uint64_t hi = Histogram::upper_bound(index);
        ASSERT_LE(lo, value);
        ASSERT_GE(hi, value);
        ASSERT_LE((hi - lo) * 32U, lo == 0 ? 32U : lo);
    }
    for (std::size_t index = 1; index < Histogram::kBuckets; ++index) {
        ASSERT_EQ(Histogram::lower_bound(index), Histogram::upper_bound(index - 1) + 1U);
    }
}

TEST(Histogram, QuantilesTrackAKnownDistribution) {
    auto h = std::make_unique<Histogram>();
    EXPECT_EQ(h->p50(), 0U);
    EXPECT_EQ(h->min(), 0U);
    for (std::uint64_t v = 1; v <= 100'000; ++v) {
        h->record(v);
    }
    EXPECT_EQ(h->count(), 100'000U);
    EXPECT_EQ(h->min(), 1U);
    EXPECT_EQ(h->max(), 100'000U);
    EXPECT_EQ(h->mean(), 50'000U);

    const auto near = [](std::uint64_t got, std::uint64_t want) {
        return got >= want && got <= want + want / 30U;
    };
    EXPECT_TRUE(near(h->p50(), 50'000)) << h->p50();
    EXPECT_TRUE(near(h->p90(), 90'000)) << h->p90();
    EXPECT_TRUE(near(h->p99(), 99'000)) << h->p99();
    EXPECT_TRUE(near(h->p999(), 99'900)) << h->p999();
    EXPECT_EQ(h->quantile_ppm(1'000'000), 100'000U);
    EXPECT_EQ(h->quantile_ppm(0), 1U);
}

TEST(Histogram, ClampsMergesAndResets) {
    auto a = std::make_unique<Histogram>();
    auto b = std::make_unique<Histogram>();
    a->record(10);
    a->record(Histogram::kMaxValue + 5);
    EXPECT_EQ(a->clamped(), 1U);
    EXPECT_EQ(a->max(), Histogram::kMaxValue);

    b->record(3);
    b->record(7);
    b->merge(*a);
    EXPECT_EQ(b->count(), 4U);
    EXPECT_EQ(b->min(), 3U);
    EXPECT_EQ(b->max(), Histogram::kMaxValue);
    EXPECT_EQ(b->clamped(), 1U);
    EXPECT_EQ(b->bucket(10), 1U);
    EXPECT_EQ(b->p50(), 7U);

    b->reset();
    EXPECT_EQ(b->count(), 0U);
    EXPECT_EQ(b->max(), 0U);
    EXPECT_EQ(b->bucket(10), 0U);
}

TEST(Report, ConvertsTicksToNanoseconds) {
    auto h = std::make_unique<Histogram>();
    for (std::uint64_t v = 1000; v < 2000; ++v) {
        h->record(v);
    }
    const TscScale half_ns_per_tick{std::uint64_t{1} << 31};
    const perf::LatencyReport report = perf::summarize(*h, half_ns_per_tick);
    EXPECT_EQ(report.count, 1000U);
    EXPECT_EQ(report.min_ns, 500U);
    EXPECT_EQ(report.max_ns, 999U);
    EXPECT_GE(report.p50_ns, 749U);
    EXPECT_LE(report.p50_ns, 775U);
    EXPECT_LE(report.p50_ns, report.p90_ns);
    EXPECT_LE(report.p90_ns, report.p99_ns);
    EXPECT_LE(report.p99_ns, report.max_ns);

    std::FILE* sink = std::fopen("/dev/null", "w");
    ASSERT_NE(sink, nullptr);
    perf::print(sink, "unit", report);
    static_cast<void>(std::fclose(sink));
}

TEST(Timer, ScopedTimerRecordsElapsedTicks) {
    auto h = std::make_unique<Histogram>();
    test::FakeClock::value = 100;
    {
        const perf::ScopedTimer<test::FakeClock> timer{*h};
        test::FakeClock::value = 175;
    }
    {
        const perf::ScopedTimer<test::FakeClock> timer{*h};
        test::FakeClock::value = 100;
    }
    EXPECT_EQ(h->count(), 2U);
    EXPECT_EQ(h->max(), 75U);
    EXPECT_EQ(h->min(), 0U);

    {
        const perf::ScopedTimer<> real{*h};
    }
    EXPECT_EQ(h->count(), 3U);
    if (has_invariant_tsc()) {
        EXPECT_LT(perf::measure_clock_overhead(1000), 100'000U);
    }
}

TEST(Counters, CountInstructionsWhenTheKernelAllowsIt) {
    perf::HardwareCounters counters;
    perf::CounterSample sample{};
    EXPECT_EQ(counters.read(sample).err(), EBADF);
    EXPECT_EQ(counters.start().err(), EBADF);

    const Status opened = counters.open();
    if (!opened.ok()) {
        EXPECT_FALSE(counters.is_open());
        GTEST_SKIP() << "perf_event_open unavailable here, errno " << opened.err();
    }
    ASSERT_TRUE(counters.start().ok());
    perf::CounterSample before{};
    ASSERT_TRUE(counters.read(before).ok());
    volatile std::uint64_t sink = 0;
    for (std::uint64_t i = 0; i < 1'000'000; ++i) {
        sink = sink + i;
    }
    perf::CounterSample after{};
    ASSERT_TRUE(counters.read(after).ok());
    ASSERT_TRUE(counters.stop().ok());

    const perf::CounterSample delta = after - before;
    EXPECT_GT(delta.instructions, 1'000'000U);
    EXPECT_GT(delta.cycles, 0U);
    EXPECT_GT(delta.instructions_per_kilocycle(), 0U);
}

} // namespace
