#ifndef HOTPATH_PERF_HISTOGRAM_HPP
#define HOTPATH_PERF_HISTOGRAM_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "hotpath/core/types.hpp"

namespace hotpath::perf {

class Histogram {
public:
    static constexpr unsigned kSubBits = 5;
    static constexpr std::size_t kSubCount = std::size_t{1} << kSubBits;
    static constexpr unsigned kMaxBits = 40;
    static constexpr std::size_t kGroups = kMaxBits - kSubBits + 1;
    static constexpr std::size_t kBuckets = kGroups * kSubCount;
    static constexpr std::uint64_t kMaxValue = (std::uint64_t{1} << kMaxBits) - 1;

    [[nodiscard]] static constexpr std::size_t index_of(std::uint64_t value) noexcept {
        if (value < kSubCount) {
            return static_cast<std::size_t>(value);
        }
        const unsigned top = 63U - static_cast<unsigned>(std::countl_zero(value));
        const unsigned shift = top - kSubBits;
        return (std::size_t{shift} + 1U) * kSubCount +
               static_cast<std::size_t>((value >> shift) & (kSubCount - 1U));
    }

    [[nodiscard]] static constexpr std::uint64_t lower_bound(std::size_t index) noexcept {
        const std::size_t group = index / kSubCount;
        const std::uint64_t sub = index % kSubCount;
        return group == 0 ? sub : (kSubCount + sub) << (group - 1U);
    }

    [[nodiscard]] static constexpr std::uint64_t upper_bound(std::size_t index) noexcept {
        const std::size_t group = index / kSubCount;
        const std::uint64_t width = group <= 1 ? 1U : std::uint64_t{1} << (group - 1U);
        return lower_bound(index) + width - 1U;
    }

    constexpr void record(std::uint64_t value) noexcept {
        if (value > kMaxValue) {
            value = kMaxValue;
            clamped_ += 1;
        }
        counts_[index_of(value)] += 1;
        count_ += 1;
        sum_ += value;
        min_ = value < min_ ? value : min_;
        max_ = value > max_ ? value : max_;
    }

    [[nodiscard]] constexpr std::uint64_t quantile_ppm(std::uint32_t ppm) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        const std::uint64_t capped = ppm > 1'000'000U ? 1'000'000U : ppm;
        std::uint64_t rank = (count_ * capped + 999'999U) / 1'000'000U;
        rank = rank == 0 ? 1 : rank;

        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) {
                const std::uint64_t upper = upper_bound(i);
                return upper < max_ ? upper : max_;
            }
        }
        return max_;
    }

    [[nodiscard]] constexpr std::uint64_t p50() const noexcept { return quantile_ppm(500'000); }
    [[nodiscard]] constexpr std::uint64_t p90() const noexcept { return quantile_ppm(900'000); }
    [[nodiscard]] constexpr std::uint64_t p99() const noexcept { return quantile_ppm(990'000); }
    [[nodiscard]] constexpr std::uint64_t p999() const noexcept { return quantile_ppm(999'000); }

    [[nodiscard]] constexpr std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] constexpr std::uint64_t sum() const noexcept { return sum_; }
    [[nodiscard]] constexpr std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    [[nodiscard]] constexpr std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] constexpr std::uint64_t clamped() const noexcept { return clamped_; }
    [[nodiscard]] constexpr std::uint64_t mean() const noexcept {
        return count_ == 0 ? 0 : sum_ / count_;
    }
    [[nodiscard]] constexpr std::uint64_t bucket(std::size_t index) const noexcept {
        return counts_[index];
    }

    constexpr void merge(const Histogram& other) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) {
            counts_[i] += other.counts_[i];
        }
        count_ += other.count_;
        sum_ += other.sum_;
        clamped_ += other.clamped_;
        min_ = other.min_ < min_ ? other.min_ : min_;
        max_ = other.max_ > max_ ? other.max_ : max_;
    }

    constexpr void reset() noexcept { *this = Histogram{}; }

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t count_{0};
    std::uint64_t sum_{0};
    std::uint64_t min_{~std::uint64_t{0}};
    std::uint64_t max_{0};
    std::uint64_t clamped_{0};
};

static_assert(ShmSafe<Histogram>);
static_assert(Histogram::index_of(0) == 0 && Histogram::index_of(31) == 31);
static_assert(Histogram::index_of(32) == 32 && Histogram::index_of(63) == 63);
static_assert(Histogram::index_of(64) == 64 && Histogram::index_of(65) == 64);
static_assert(Histogram::index_of(Histogram::kMaxValue) == Histogram::kBuckets - 1);
static_assert(Histogram::lower_bound(64) == 64 && Histogram::upper_bound(64) == 65);
static_assert(Histogram::upper_bound(Histogram::kBuckets - 1) == Histogram::kMaxValue);

} // namespace hotpath::perf

#endif // HOTPATH_PERF_HISTOGRAM_HPP
