#ifndef HOTPATH_GATEWAY_RISK_HPP
#define HOTPATH_GATEWAY_RISK_HPP

#include <cstdint>

namespace hotpath::gateway {

struct RiskLimits {
    std::uint32_t max_order_qty{0};
    std::uint64_t max_order_notional{0};
    std::uint32_t max_orders_per_window{0};
    std::uint64_t window_ticks{0};
};

enum class RiskVerdict : char {
    Pass = 0,
    Quantity = 'Q',
    Notional = 'N',
    Rate = 'R',
};

class RiskGate {
public:
    constexpr RiskGate() noexcept = default;
    explicit constexpr RiskGate(const RiskLimits& limits) noexcept : limits_{limits} {}

    [[nodiscard]] constexpr RiskVerdict check(std::uint32_t qty, std::uint32_t price_ticks,
                                              std::uint64_t now) noexcept {
        if (qty == 0 || qty > limits_.max_order_qty) {
            return RiskVerdict::Quantity;
        }
        if (std::uint64_t{qty} * price_ticks > limits_.max_order_notional) {
            return RiskVerdict::Notional;
        }
        if (now - window_start_ >= limits_.window_ticks) {
            window_start_ = now;
            window_count_ = 0;
        }
        if (window_count_ >= limits_.max_orders_per_window) {
            return RiskVerdict::Rate;
        }
        window_count_ += 1U;
        return RiskVerdict::Pass;
    }

    [[nodiscard]] constexpr const RiskLimits& limits() const noexcept { return limits_; }

private:
    RiskLimits limits_{};
    std::uint64_t window_start_{0};
    std::uint32_t window_count_{0};
};

} // namespace hotpath::gateway

#endif // HOTPATH_GATEWAY_RISK_HPP
