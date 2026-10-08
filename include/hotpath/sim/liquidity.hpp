#ifndef HOTPATH_SIM_LIQUIDITY_HPP
#define HOTPATH_SIM_LIQUIDITY_HPP

#include <cstdint>

#include "hotpath/core/contract.hpp"
#include "hotpath/core/status.hpp"
#include "hotpath/core/types.hpp"
#include "hotpath/mem/arena.hpp"
#include "hotpath/sim/matching_engine.hpp"

namespace hotpath::sim {

struct LiquidityConfig {
    std::uint64_t seed{0x9E37'79B9'7F4A'7C15ULL};
    std::uint32_t mid_ticks{1'000'000};
    std::uint32_t tick{100};
    std::uint32_t lot{100};
    std::uint32_t max_live{32};
    std::uint32_t depth_ticks{5};
};

class Liquidity {
public:
    [[nodiscard]] Status init(mem::Arena& arena, std::uint16_t symbols,
                              const LiquidityConfig& config) noexcept {
        HOTPATH_ASSERT(mids_ == nullptr);
        if (symbols == 0 || config.tick == 0 || config.lot == 0 || config.max_live == 0 ||
            config.depth_ticks == 0 || config.seed == 0 ||
            config.mid_ticks <= config.tick * (config.depth_ticks + 2U)) {
            return Status{EINVAL};
        }
        std::uint32_t* mids = arena.allocate_array<std::uint32_t>(symbols);
        std::uint32_t* cursors = arena.allocate_array<std::uint32_t>(symbols);
        OrderHandle* handles =
            arena.allocate_array<OrderHandle>(std::size_t{symbols} * config.max_live);
        if (mids == nullptr || cursors == nullptr || handles == nullptr) {
            return Status{ENOMEM};
        }
        for (std::uint16_t i = 0; i < symbols; ++i) {
            mids[i] = config.mid_ticks;
        }
        mids_ = mids;
        cursors_ = cursors;
        handles_ = handles;
        symbols_ = symbols;
        config_ = config;
        state_ = config.seed;
        return Status{};
    }

    template <typename V>
    void step(V& venue) noexcept {
        const std::uint16_t symbol = next_symbol_;
        next_symbol_ = static_cast<std::uint16_t>((next_symbol_ + 1U) % symbols_);
        const std::uint64_t r = next();
        const std::uint32_t roll = static_cast<std::uint32_t>(r % 100U);
        const Side side = ((r >> 8) & 1U) != 0 ? Side::Buy : Side::Sell;
        const std::uint32_t lots = 1U + static_cast<std::uint32_t>((r >> 16) % 5U);
        const std::uint32_t mid = mids_[symbol];
        OrderHandle* slots = handles_ + std::size_t{symbol} * config_.max_live;

        if (roll < 50) {
            const std::uint32_t offset =
                (1U + static_cast<std::uint32_t>((r >> 24) % config_.depth_ticks)) * config_.tick;
            const std::uint32_t price = side == Side::Buy ? mid - offset : mid + offset;
            OrderHandle& slot = slots[cursors_[symbol]];
            cursors_[symbol] = (cursors_[symbol] + 1U) % config_.max_live;
            if (slot.valid()) {
                venue.house_cancel(slot);
            }
            slot = venue.house_submit(symbol, side, lots * config_.lot, price, false);
        } else if (roll < 80) {
            OrderHandle& slot = slots[(r >> 24) % config_.max_live];
            if (slot.valid()) {
                venue.house_cancel(slot);
                slot = OrderHandle{};
            }
        } else if (roll < 92) {
            const std::uint32_t reach = config_.tick * 2U;
            const std::uint32_t price = side == Side::Buy ? mid + reach : mid - reach;
            static_cast<void>(venue.house_submit(symbol, side, lots * config_.lot, price, true));
        } else {
            const std::uint32_t floor = config_.tick * (config_.depth_ticks + 2U);
            if (((r >> 9) & 1U) != 0) {
                mids_[symbol] = mid + config_.tick;
            } else if (mid - config_.tick > floor) {
                mids_[symbol] = mid - config_.tick;
            }
        }
        steps_ += 1;
    }

    [[nodiscard]] std::uint32_t mid(std::uint16_t symbol) const noexcept {
        HOTPATH_ASSERT(symbol < symbols_);
        return mids_[symbol];
    }
    [[nodiscard]] std::uint64_t steps() const noexcept { return steps_; }

private:
    [[nodiscard]] std::uint64_t next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }

    std::uint32_t* mids_{nullptr};
    std::uint32_t* cursors_{nullptr};
    OrderHandle* handles_{nullptr};
    LiquidityConfig config_{};
    std::uint64_t state_{0};
    std::uint64_t steps_{0};
    std::uint16_t symbols_{0};
    std::uint16_t next_symbol_{0};
};

} // namespace hotpath::sim

#endif // HOTPATH_SIM_LIQUIDITY_HPP
